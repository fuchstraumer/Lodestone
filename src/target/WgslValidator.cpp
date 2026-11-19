#include "target/WgslValidator.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetUtils.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "target/TargetProfile.hpp"
#include "model/ShaderDataSchema.hpp"
#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include "tint/tint.h"
#include "src/tint/lang/core/ir/reflection.h"
#include <magic_enum/magic_enum.hpp>

namespace lodestone
{

namespace
{

    /** What one read of WGSL keeps for its comparisons: the declared bindings of each entry point, sorted by placement. */
    struct WgslParsedCode final : ParsedEmittedCode
    {
        std::vector<std::vector<tint::inspector::ResourceBinding>> EntryPointBindings;
    };

    /** @brief Allows every extension: the compiler knew what to emit. Invalid WGSL is a parse error, not a mismatch. */
    CookResult<tint::Program> ParseWgsl(std::span<const std::byte> source_code, DiagnosticSink& sink);

    /** @brief Workgroup memory comes from Tint's `GetWorkgroupInfo`, Dawn's own rule. It fails without a workgroup
     * size, so only compute entry points read it. */
    CookError ReadCodeFacts(const tint::Program& program,
                            tint::inspector::Inspector& inspector,
                            EmittedCodeFacts& facts,
                            DiagnosticSink& sink);

    /** @brief Returns the inputs with a location of each vertex entry point, sorted by location. */
    std::vector<EmittedVertexInput> ReadVertexInputs(tint::inspector::Inspector& inspector);

    VertexScalarType VertexScalarTypeFromTint(tint::inspector::ComponentType component_type) noexcept;

    /** @brief Returns 0 for a composition that Tint does not know. */
    uint32_t ComponentCountFromTint(tint::inspector::CompositionType composition_type) noexcept;

    /** @brief Texture uses with no sampler (textureLoad) are not pairs. */
    std::vector<SamplerTexturePlacement> ReadPairs(tint::inspector::Inspector& inspector, const std::string& entry_point);

    std::optional<tint::inspector::ResourceBinding::ResourceType> ToTintStorageTextureType(ResourceAccess access);

    /** @brief Returns nothing for a kind or an access that has no WGSL form. */
    std::optional<tint::inspector::ResourceBinding::ResourceType> TintTypeForBindingKind(BindingKind kind, ResourceShape shape, ResourceAccess access);

    bool TintTypeAgreesWithReflectionKind(tint::inspector::ResourceBinding::ResourceType tint_kind,
                                          BindingKind kind,
                                          ResourceShape shape,
                                          ResourceAccess access);

    /** Maps the curated set of `FromSlangImageFormat`. Other formats are Invalid on both sides. */
    TextureFormat TextureFormatFromTint(tint::inspector::ResourceBinding::TexelFormat format) noexcept;

    std::string_view TintTypeToString(tint::inspector::ResourceBinding::ResourceType tint_kind);

    void ReportDeclaredOnly(BindingComparison& comparison, const tint::inspector::ResourceBinding& declared);

    void ReportReflectedOnly(BindingComparison& comparison, const ReflectedBinding& reflected);

    /** @brief One merge walk over both lists, which must be sorted by placement. */
    BindingComparison CompareBindings(std::span<const tint::inspector::ResourceBinding> wgsl_bindings,
                                      std::span<const ReflectedBinding*> reflected_bindings) noexcept;

} // namespace

WgslValidator::WgslValidator() = default;
WgslValidator::~WgslValidator() = default;

CookResult<std::unique_ptr<const ParsedEmittedCode>> WgslValidator::parseEmittedCode(std::span<const std::byte> source_code,
                                                                                      [[maybe_unused]] bool validate,
                                                                                      DiagnosticSink& sink) const
{
    CookResult<tint::Program> entrypointProgram = ParseWgsl(source_code, sink);
    if (!entrypointProgram)
    {
        return std::unexpected(entrypointProgram.error());
    }

    tint::inspector::Inspector epInspector(*entrypointProgram);
    auto parsed = std::make_unique<WgslParsedCode>();
    EmittedCodeFacts& facts = parsed->Facts;
    for (const auto& entrypoint : epInspector.GetEntryPoints())
    {
        std::vector<tint::inspector::ResourceBinding> wgslBindings = epInspector.GetResourceBindings(entrypoint.name);
        std::ranges::sort(wgslBindings, [](const tint::inspector::ResourceBinding& lhs, const tint::inspector::ResourceBinding& rhs)
        {
            return std::tie(lhs.bind_group, lhs.binding) < std::tie(rhs.bind_group, rhs.binding);
        });

        facts.SamplerTexturePairs.append_range(ReadPairs(epInspector, entrypoint.name));
        for (const tint::inspector::ResourceBinding& binding : wgslBindings)
        {
            facts.DeclaredPlacements.emplace_back(binding.bind_group, binding.binding);
        }

        parsed->EntryPointBindings.push_back(std::move(wgslBindings));
    }

    const CookError read = ReadCodeFacts(*entrypointProgram, epInspector, facts, sink);
    if (read != CookError::Success)
    {
        return std::unexpected(read);
    }

    return std::unique_ptr<const ParsedEmittedCode>{ std::move(parsed) };
}

BindingComparison WgslValidator::compareEmittedCode(const ParsedEmittedCode& parsed,
                                                    std::span<const ReflectedBinding*> bound,
                                                    [[maybe_unused]] std::span<const ReflectedBinding*> stored,
                                                    [[maybe_unused]] DiagnosticSink& sink) const
{
    const auto& code = static_cast<const WgslParsedCode&>(parsed);

    BindingComparison result;
    for (const std::vector<tint::inspector::ResourceBinding>& wgslBindings : code.EntryPointBindings)
    {
        BindingComparison epResult = CompareBindings(wgslBindings, bound);
        result.Mismatches.append_range(std::move(epResult.Mismatches));
    }
    result.Emitted = code.Facts;

    return result;
}

namespace
{

    CookResult<tint::Program> ParseWgsl(std::span<const std::byte> source_code, DiagnosticSink& sink)
    {
        using namespace tint;
        wgsl::reader::Options readerOptions;
        readerOptions.allowed_features = wgsl::AllowedFeatures::Everything();
        std::string_view source_code_view(reinterpret_cast<const char*>(source_code.data()), source_code.size());
        Source::File entrypointSource("source.wgsl", source_code_view);

        Program entrypointProgram = wgsl::reader::Parse(&entrypointSource, readerOptions);
        if (!entrypointProgram.IsValid())
        {
            std::string tintDiagnostics; tintDiagnostics.reserve(1024);
            for (const auto& diag : entrypointProgram.Diagnostics())
            {
                tintDiagnostics += ToString(diag.source);
                tintDiagnostics += diag.message.Plain() + "\n";
            }
            std::string fullMessage = std::format("WGSL program could not be constructed from given source. Diagnostics:\n{}", tintDiagnostics);
            return std::unexpected(ReportError(sink,
                                               CookError::TargetValidationEntryPointParseFailed,
                                               fullMessage));
        }

        return entrypointProgram;
    }

    CookError ReadCodeFacts(const tint::Program& program,
                            tint::inspector::Inspector& inspector,
                            EmittedCodeFacts& facts,
                            DiagnosticSink& sink)
    {
        facts.CapabilityNames = inspector.GetUsedExtensionNames();
        std::ranges::sort(facts.CapabilityNames);
        const auto repeated = std::ranges::unique(facts.CapabilityNames);
        facts.CapabilityNames.erase(repeated.begin(), repeated.end());
        facts.VertexInputs = ReadVertexInputs(inspector);

        const bool compute = std::ranges::any_of(inspector.GetEntryPoints(),
                                                 [](const tint::inspector::EntryPoint& entry_point)
                                                 { return entry_point.stage == tint::inspector::PipelineStage::kCompute; });
        if (!compute)
        {
            return CookError::Success;
        }

        auto ir = tint::wgsl::reader::ProgramToLoweredIR(program);
        if (ir != tint::Success)
        {
            return ReportError(sink,
                               CookError::TargetValidationEntryPointInvalid,
                               std::format("Tint cannot lower the WGSL to IR: {}", ir.Failure().reason));
        }
        auto workgroup = tint::core::ir::GetWorkgroupInfo(ir.Get());
        if (workgroup != tint::Success)
        {
            return ReportError(sink,
                               CookError::TargetValidationEntryPointInvalid,
                               std::format("Tint cannot read the workgroup of the WGSL: {}", workgroup.Failure().reason));
        }
        facts.WorkgroupMemoryBytes = static_cast<uint32_t>(workgroup.Get().storage_size);
        return CookError::Success;
    }

    std::vector<EmittedVertexInput> ReadVertexInputs(tint::inspector::Inspector& inspector)
    {
        std::vector<EmittedVertexInput> inputs;
        for (const tint::inspector::EntryPoint& entryPoint : inspector.GetEntryPoints())
        {
            if (entryPoint.stage != tint::inspector::PipelineStage::kVertex)
            {
                continue;
            }

            for (const tint::inspector::StageVariable& variable : entryPoint.input_variables)
            {
                if (!variable.attributes.location.has_value())
                {
                    continue;
                }

                inputs.push_back(EmittedVertexInput{ .Location = *variable.attributes.location,
                                                     .ScalarType = VertexScalarTypeFromTint(variable.component_type),
                                                     .ComponentCount = ComponentCountFromTint(variable.composition_type) });
            }
        }

        std::ranges::sort(inputs, {}, &EmittedVertexInput::Location);

        return inputs;
    }

    VertexScalarType VertexScalarTypeFromTint(tint::inspector::ComponentType component_type) noexcept
    {
        using tint::inspector::ComponentType;
        switch (component_type)
        {
        case ComponentType::kF16:
            return VertexScalarType::Float16;
        case ComponentType::kF32:
            return VertexScalarType::Float32;
        case ComponentType::kI32:
            return VertexScalarType::SignedInteger32;
        case ComponentType::kU32:
            return VertexScalarType::UnsignedInteger32;
        case ComponentType::kUnknown:
            return VertexScalarType::Invalid;
        }

        return VertexScalarType::Invalid;
    }

    uint32_t ComponentCountFromTint(tint::inspector::CompositionType composition_type) noexcept
    {
        using tint::inspector::CompositionType;
        switch (composition_type)
        {
        case CompositionType::kScalar:
            return 1u;
        case CompositionType::kVec2:
            return 2u;
        case CompositionType::kVec3:
            return 3u;
        case CompositionType::kVec4:
            return 4u;
        case CompositionType::kUnknown:
            return 0u;
        }

        return 0u;
    }

    std::vector<SamplerTexturePlacement> ReadPairs(tint::inspector::Inspector& inspector, const std::string& entry_point)
    {
        std::vector<SamplerTexturePlacement> pairs;
        for (const tint::inspector::SamplerTexturePair& use : inspector.GetSamplerTextureUses(entry_point))
        {
            pairs.push_back(SamplerTexturePlacement{ .TextureGroup = use.texture_binding_point.group,
                                                     .TextureBinding = use.texture_binding_point.binding,
                                                     .SamplerGroup = use.sampler_binding_point.group,
                                                     .SamplerBinding = use.sampler_binding_point.binding });
        }
        return pairs;
    }

    std::optional<tint::inspector::ResourceBinding::ResourceType> ToTintStorageTextureType(ResourceAccess access)
    {
        using tResourceType = tint::inspector::ResourceBinding::ResourceType;
        switch (access)
        {
        case ResourceAccess::ReadOnly:
            return tResourceType::kReadOnlyStorageTexture;
        case ResourceAccess::ReadWrite:
            return tResourceType::kReadWriteStorageTexture;
        case ResourceAccess::WriteOnly:
            return tResourceType::kWriteOnlyStorageTexture;
        case ResourceAccess::Invalid:
        case ResourceAccess::RasterizerOrdered:
        case ResourceAccess::Append:
        case ResourceAccess::Consume:
        case ResourceAccess::Feedback:
            return std::nullopt; // invalid access
        }

        return std::nullopt;
    }

    std::optional<tint::inspector::ResourceBinding::ResourceType> TintTypeForBindingKind(BindingKind kind, ResourceShape shape, ResourceAccess access)
    {
        using tResourceType = tint::inspector::ResourceBinding::ResourceType;
        const bool isShadow = ResourceShapeIsShadow(shape);
        const bool isMultisample = ResourceShapeIsMultisample(shape);
        switch (kind)
        {
        case BindingKind::Invalid:
            return std::nullopt; // invalid binding kind
        case BindingKind::Sampler:
            return tResourceType::kSampler;
        case BindingKind::Texture:
            if (isShadow)
            {
                return isMultisample ? tResourceType::kDepthMultisampledTexture : tResourceType::kDepthTexture;
            }
            else
            {
                return isMultisample ? tResourceType::kMultisampledTexture : tResourceType::kSampledTexture;
            }
        case BindingKind::UniformBuffer:
            return tResourceType::kUniformBuffer;
        case BindingKind::ParameterBlock:
            return std::nullopt; // reflection flattens a block into its contents
        case BindingKind::StorageBuffer:
            return (access == ResourceAccess::ReadOnly) ? tResourceType::kReadOnlyStorageBuffer : tResourceType::kStorageBuffer;
        case BindingKind::TexelBuffer:
            if (access == ResourceAccess::ReadOnly)
            {
                return tResourceType::kReadOnlyTexelBuffer;
            }
            else if (access == ResourceAccess::ReadWrite)
            {
                return tResourceType::kReadWriteTexelBuffer;
            }
            else
            {
                return std::nullopt; // invalid access
            }
        case BindingKind::CombinedTextureSampler:
            return std::nullopt; // CombinedTextureSampler is not directly supported
        case BindingKind::InputRenderTarget:
            return tResourceType::kInputAttachment;
        case BindingKind::InlineUniform:
        case BindingKind::RayTracingAccelerationStructure:
        case BindingKind::PushConstant:
            return std::nullopt; // none of these exist in WGSL
        case BindingKind::StorageTexture:
            return ToTintStorageTextureType(access);
        }

        return std::nullopt;
    }

    bool TintTypeAgreesWithReflectionKind(tint::inspector::ResourceBinding::ResourceType tint_kind,
                                          BindingKind kind,
                                          ResourceShape shape,
                                          ResourceAccess access)
    {
        const std::optional<tint::inspector::ResourceBinding::ResourceType> reflectionTintType =
            TintTypeForBindingKind(kind, shape, access);
        return reflectionTintType == tint_kind;
    }

    TextureFormat TextureFormatFromTint(tint::inspector::ResourceBinding::TexelFormat format) noexcept
    {
        using tTexelFormat = tint::inspector::ResourceBinding::TexelFormat;
        switch (format)
        {
        case tTexelFormat::kR8Unorm:
            return TextureFormat::R8Unorm;
        case tTexelFormat::kRg8Unorm:
            return TextureFormat::RG8Unorm;
        case tTexelFormat::kRgba8Unorm:
            return TextureFormat::RGBA8Unorm;
        case tTexelFormat::kRgba8Snorm:
            return TextureFormat::RGBA8Snorm;
        case tTexelFormat::kR16Float:
            return TextureFormat::R16Float;
        case tTexelFormat::kRg16Float:
            return TextureFormat::RG16Float;
        case tTexelFormat::kRgba16Float:
            return TextureFormat::RGBA16Float;
        case tTexelFormat::kR32Float:
            return TextureFormat::R32Float;
        case tTexelFormat::kRg32Float:
            return TextureFormat::RG32Float;
        case tTexelFormat::kRgba32Float:
            return TextureFormat::RGBA32Float;
        case tTexelFormat::kR32Uint:
            return TextureFormat::R32Uint;
        case tTexelFormat::kRg32Uint:
            return TextureFormat::RG32Uint;
        case tTexelFormat::kRgba32Uint:
            return TextureFormat::RGBA32Uint;
        case tTexelFormat::kRgb10A2Unorm:
            return TextureFormat::R10G10B10A2Unorm;
        case tTexelFormat::kRg11B10Ufloat:
            return TextureFormat::R11G11B10Ufloat;
        default:
            return TextureFormat::Invalid;
        }
    }

    std::string_view TintTypeToString(tint::inspector::ResourceBinding::ResourceType tint_kind)
    {
        switch (tint_kind)
        {
            case tint::inspector::ResourceBinding::ResourceType::kUniformBuffer:
                return "UniformBuffer";
            case tint::inspector::ResourceBinding::ResourceType::kStorageBuffer:
                return "StorageBuffer";
            case tint::inspector::ResourceBinding::ResourceType::kReadOnlyStorageBuffer:
                return "ReadOnlyStorageBuffer";
            case tint::inspector::ResourceBinding::ResourceType::kSampler:
                return "Sampler";
            case tint::inspector::ResourceBinding::ResourceType::kSampledTexture:
                return "SampledTexture";
            case tint::inspector::ResourceBinding::ResourceType::kMultisampledTexture:
                return "MultisampledTexture";
            case tint::inspector::ResourceBinding::ResourceType::kWriteOnlyStorageTexture:
                return "WriteOnlyStorageTexture";
            case tint::inspector::ResourceBinding::ResourceType::kReadOnlyStorageTexture:
                return "ReadOnlyStorageTexture";
            case tint::inspector::ResourceBinding::ResourceType::kReadWriteStorageTexture:
                return "ReadWriteStorageTexture";
            case tint::inspector::ResourceBinding::ResourceType::kDepthTexture:
                return "DepthTexture";
            case tint::inspector::ResourceBinding::ResourceType::kDepthMultisampledTexture:
                return "DepthMultisampledTexture";
            case tint::inspector::ResourceBinding::ResourceType::kExternalTexture:
                return "ExternalTexture";
            case tint::inspector::ResourceBinding::ResourceType::kReadOnlyTexelBuffer:
                return "ReadOnlyTexelBuffer";
            case tint::inspector::ResourceBinding::ResourceType::kReadWriteTexelBuffer:
                return "ReadWriteTexelBuffer"; // no texel buffer support yet
            case tint::inspector::ResourceBinding::ResourceType::kInputAttachment:
                return "InputAttachment";
        }

        std::unreachable();
    }

    void ReportDeclaredOnly(BindingComparison& comparison, const tint::inspector::ResourceBinding& declared)
    {
        comparison.Mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::EmittedOnly,
                                                         .Group = declared.bind_group,
                                                         .Binding = declared.binding,
                                                         .Name = std::string{ StripSlangNameMangling(declared.variable_name) },
                                                         .Emitted = {},
                                                         .Reflected = {} });
    }

    void ReportReflectedOnly(BindingComparison& comparison, const ReflectedBinding& reflected)
    {
        comparison.Mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::ReflectedOnly,
                                                         .Group = GroupOf(reflected),
                                                         .Binding = BindingOf(reflected),
                                                         .Name = MakeScopedName(reflected),
                                                         .Emitted = {},
                                                         .Reflected = {} });
    }

    BindingComparison CompareBindings(std::span<const tint::inspector::ResourceBinding> wgsl_bindings,
                                      std::span<const ReflectedBinding*> reflected_bindings) noexcept
    {
        using tint::inspector::ResourceBinding;
        BindingComparison comparison;
        auto iterSource = wgsl_bindings.begin();
        auto iterReflected = reflected_bindings.begin();

        while (iterSource != wgsl_bindings.end() && iterReflected != reflected_bindings.end())
        {
            const ResourceBinding& declaredBinding = *iterSource;
            const ReflectedBinding* reflectedBinding = *iterReflected;
            const auto declaredTuple = std::tie(declaredBinding.bind_group, declaredBinding.binding);
            const auto reflectedTuple = std::make_tuple(GroupOf(*reflectedBinding), BindingOf(*reflectedBinding));

            if (declaredTuple == reflectedTuple)
            {
                const std::string_view unmangledName = StripSlangNameMangling(declaredBinding.variable_name);
                const auto record = [&](BindingMismatchKind kind, std::string emitted, std::string reflected_value)
                {
                    comparison.Mismatches.push_back(BindingMismatch{ .Kind = kind,
                                                                     .Group = declaredBinding.bind_group,
                                                                     .Binding = declaredBinding.binding,
                                                                     .Name = std::string{ unmangledName },
                                                                     .Emitted = std::move(emitted),
                                                                     .Reflected = std::move(reflected_value) });
                };

                const std::string scopedName = MakeScopedName(*reflectedBinding);
                if (unmangledName != scopedName)
                {
                    record(BindingMismatchKind::Name, std::string{ unmangledName }, scopedName);
                }

                if (!TintTypeAgreesWithReflectionKind(declaredBinding.resource_type,
                                                      reflectedBinding->Kind,
                                                      reflectedBinding->Shape,
                                                      reflectedBinding->Access))
                {
                    const std::optional<tint::inspector::ResourceBinding::ResourceType> tintKind =
                        TintTypeForBindingKind(reflectedBinding->Kind, reflectedBinding->Shape, reflectedBinding->Access);
                    const std::string_view neededName = tintKind.has_value() ? TintTypeToString(*tintKind) : "(no WGSL form)";
                    record(BindingMismatchKind::Kind,
                           std::string{ TintTypeToString(declaredBinding.resource_type) },
                           std::format("{} (needs {})", ToString(reflectedBinding->Kind), neededName));
                }
                else if (reflectedBinding->Kind == BindingKind::StorageTexture)
                {
                    const TextureFormat declaredFormat = TextureFormatFromTint(declaredBinding.image_format);
                    if (declaredFormat != reflectedBinding->StorageFormat)
                    {
                        record(BindingMismatchKind::StorageFormat,
                               std::string{ magic_enum::enum_name(declaredFormat) },
                               std::string{ magic_enum::enum_name(reflectedBinding->StorageFormat) });
                    }
                }

                ++iterSource;
                ++iterReflected;
            }
            else if (declaredTuple < reflectedTuple)
            {
                ReportDeclaredOnly(comparison, declaredBinding);
                ++iterSource;
            }
            else
            {
                ReportReflectedOnly(comparison, *reflectedBinding);
                ++iterReflected;
            }
        }

        for (; iterSource != wgsl_bindings.end(); ++iterSource)
        {
            ReportDeclaredOnly(comparison, *iterSource);
        }

        for (; iterReflected != reflected_bindings.end(); ++iterReflected)
        {
            ReportReflectedOnly(comparison, **iterReflected);
        }

        return comparison;
    }

} // namespace

} // namespace lodestone
