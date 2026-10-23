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

    /** @brief One merge walk over both lists, which must be sorted by placement. */
    BindingComparison CompareBindings(std::span<const tint::inspector::ResourceBinding> wgsl_bindings,
                                      std::span<const ReflectedBinding*> reflected_bindings) noexcept;

    /** @brief Allows every extension: the compiler knew what to emit. Invalid WGSL is a parse error, not a mismatch. */
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

    /** @brief Workgroup memory comes from Tint's `GetWorkgroupInfo`, Dawn's own rule. It fails without a workgroup
     * size, so only compute entry points read it. */
    CookError ReadCodeFacts(const tint::Program& program,
                            tint::inspector::Inspector& inspector,
                            EmittedCodeFacts& facts,
                            DiagnosticSink& sink)
    {
        facts.CapabilityNames = inspector.GetUsedExtensionNames();
        std::ranges::sort(facts.CapabilityNames);
        const auto repeated = std::ranges::unique(facts.CapabilityNames);
        facts.CapabilityNames.erase(repeated.begin(), repeated.end());

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

    /** @brief Texture uses with no sampler (textureLoad) are not pairs. */
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
}

WgslValidator::WgslValidator() = default;
WgslValidator::~WgslValidator() = default;

CookResult<BindingComparison> WgslValidator::validateEntryPoint(std::span<const std::byte> source_code,
                                                                std::span<const ReflectedBinding*> bindings,
                                                                DiagnosticSink& sink) const
{
    CookResult<tint::Program> entrypointProgram = ParseWgsl(source_code, sink);
    if (!entrypointProgram)
    {
        return std::unexpected(entrypointProgram.error());
    }

    tint::inspector::Inspector epInspector(*entrypointProgram);
    using tint::inspector::ResourceBinding;
    BindingComparison result{ .Matches = true, .Report = "" };
    for (const auto& entrypoint : epInspector.GetEntryPoints())
    {
        std::vector<ResourceBinding> wgslBindings = epInspector.GetResourceBindings(entrypoint.name);

        auto sortResourceBindings = [](const ResourceBinding& lhs, const ResourceBinding& rhs)
        {
            if (lhs.bind_group != rhs.bind_group)
            {
                return lhs.bind_group < rhs.bind_group;
            }
            return lhs.binding < rhs.binding;
        };

        std::ranges::sort(wgslBindings, sortResourceBindings);

        BindingComparison epResult = CompareBindings(wgslBindings, bindings);
        epResult.Emitted.SamplerTexturePairs = ReadPairs(epInspector, entrypoint.name);
        if (!epResult.Matches)
        {
            return epResult;
        }
        result.Emitted.SamplerTexturePairs.append_range(std::move(epResult.Emitted.SamplerTexturePairs));
    }

    const CookError read = ReadCodeFacts(*entrypointProgram, epInspector, result.Emitted, sink);
    if (read != CookError::Success)
    {
        return std::unexpected(read);
    }
    return result;
}

CookResult<EmittedCodeFacts> WgslValidator::readEmittedCodeFacts(std::span<const std::byte> source_code,
                                                                 DiagnosticSink& sink) const
{
    CookResult<tint::Program> entrypointProgram = ParseWgsl(source_code, sink);
    if (!entrypointProgram)
    {
        return std::unexpected(entrypointProgram.error());
    }

    tint::inspector::Inspector epInspector(*entrypointProgram);
    EmittedCodeFacts facts;
    for (const auto& entrypoint : epInspector.GetEntryPoints())
    {
        facts.SamplerTexturePairs.append_range(ReadPairs(epInspector, entrypoint.name));
        for (const tint::inspector::ResourceBinding& binding : epInspector.GetResourceBindings(entrypoint.name))
        {
            facts.DeclaredPlacements.emplace_back(binding.bind_group, binding.binding);
        }
    }

    const CookError read = ReadCodeFacts(*entrypointProgram, epInspector, facts, sink);
    if (read != CookError::Success)
    {
        return std::unexpected(read);
    }
    return facts;
}

namespace
{
    tint::inspector::ResourceBinding::ResourceType ToTintStorageTextureType(ResourceAccess access)
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
            return static_cast<tResourceType>(-1); // invalid access
        }

        return static_cast<tResourceType>(-1);
    }

    tint::inspector::ResourceBinding::ResourceType TintTypeForBindingKind(BindingKind kind, ResourceShape shape, ResourceAccess access)
    {
        using tResourceType = tint::inspector::ResourceBinding::ResourceType;
        const bool isShadow = ResourceShapeIsShadow(shape);
        const bool isMultisample = ResourceShapeIsMultisample(shape);
        switch (kind)
        {
        case BindingKind::Invalid:
            return static_cast<tResourceType>(-1); // invalid binding kind
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
            std::unreachable();
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
                return static_cast<tResourceType>(-1); // invalid access
            }
        case BindingKind::CombinedTextureSampler:
            return static_cast<tResourceType>(-1); // CombinedTextureSampler is not directly supported
        case BindingKind::InputRenderTarget:
            return tResourceType::kInputAttachment;
        case BindingKind::InlineUniform:
        case BindingKind::RayTracingAccelerationStructure:
        case BindingKind::PushConstant:
            return static_cast<tResourceType>(-1); // none of these exist in WGSL
        case BindingKind::StorageTexture:
            return ToTintStorageTextureType(access);
        }

        return static_cast<tResourceType>(-1);
    }

    bool TintTypeAgreesWithReflectionKind(tint::inspector::ResourceBinding::ResourceType tint_kind,
                                          BindingKind kind,
                                          ResourceShape shape,
                                          ResourceAccess access)
    {
        tint::inspector::ResourceBinding::ResourceType reflectionTintType = TintTypeForBindingKind(kind, shape, access);
        return reflectionTintType == tint_kind;
    }

    /** Maps the curated set of `FromSlangImageFormat`. Other formats are Invalid on both sides. */
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
            default:
                std::unreachable();
        }
    }

    void ReportDeclaredOnly(BindingComparison& comparison, const tint::inspector::ResourceBinding& declared)
    {
        comparison.Matches = false;
        comparison.Report += std::format("  wgsl declares @group({}) @binding({}) {} : reflection has no binding at that location\n",
                                         declared.bind_group,
                                         declared.binding,
                                         StripSlangNameMangling(declared.variable_name));
    }

    void ReportReflectedOnly(BindingComparison& comparison, const ReflectedBinding& reflected)
    {
        comparison.Matches = false;
        comparison.Report += std::format("  reflection has @group({}) @binding({}) {} : wgsl has no binding at that location\n",
                                         GroupOf(reflected),
                                         BindingOf(reflected),
                                         MakeScopedName(reflected));
    }

    BindingComparison CompareBindings(std::span<const tint::inspector::ResourceBinding> wgsl_bindings,
                                      std::span<const ReflectedBinding*> reflected_bindings) noexcept
    {
        using tint::inspector::ResourceBinding;
        BindingComparison comparison;
        comparison.Matches = true;
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
                const auto report = [&](std::string_view what)
                {
                    comparison.Matches = false;
                    comparison.Report += std::format("  wgsl declares @group({}) @binding({}) {} : {}\n",
                                                     declaredBinding.bind_group,
                                                     declaredBinding.binding,
                                                     unmangledName,
                                                     what);
                };

                if (unmangledName != MakeScopedName(*reflectedBinding))
                {
                    report(std::format("reflection has mismatched name \"{}\"", reflectedBinding->Name));
                }

                if (!TintTypeAgreesWithReflectionKind(declaredBinding.resource_type,
                                                      reflectedBinding->Kind,
                                                      reflectedBinding->Shape,
                                                      reflectedBinding->Access))
                {
                    const auto tintKind = TintTypeForBindingKind(reflectedBinding->Kind,
                                                                 reflectedBinding->Shape,
                                                                 reflectedBinding->Access);
                    report(std::format("declared as {}, reflection has kind {}, which needs {}",
                                       TintTypeToString(declaredBinding.resource_type),
                                       ToString(reflectedBinding->Kind),
                                       TintTypeToString(tintKind)));
                }
                else if (reflectedBinding->Kind == BindingKind::StorageTexture)
                {
                    const TextureFormat declaredFormat = TextureFormatFromTint(declaredBinding.image_format);
                    if (declaredFormat != reflectedBinding->StorageFormat)
                    {
                        report(std::format("declared with format {}, reflection has format {}",
                                           magic_enum::enum_name(declaredFormat),
                                           magic_enum::enum_name(reflectedBinding->StorageFormat)));
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
}

}
