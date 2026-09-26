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

namespace lodestone
{

namespace
{

    /**@brief Runs the actual binding-by-binding comparison and validation, after we've extracted and sorted
     * the bindings. Bindings *MUST* be sorted, otherwise this comparison won't work. It uses a symmetric walk
     * through bot sets of bindings to keep complexity manageable, and this also serves to elevate binding count
     * and placement mismatches (which are an error). */
    BindingComparison CompareBindings(std::span<const tint::inspector::ResourceBinding> wgsl_bindings,
                                      std::span<const ReflectedBinding*> reflected_bindings) noexcept;
}

WgslValidator::WgslValidator() = default;
WgslValidator::~WgslValidator() = default;

CookResult<BindingComparison> WgslValidator::validateEntryPoint(std::span<const std::byte> source_code,
                                                                std::span<const ReflectedBinding*> bindings,
                                                                DiagnosticSink& sink) const
{
    using namespace tint;
    // set the wgsl reader options to just enable all extensions: trust the compiler knew what to output
    // in the future, we can make this queryable but for now it's not needed
    wgsl::reader::Options readerOptions;
    readerOptions.allowed_features = wgsl::AllowedFeatures::Everything();
    // build a source file
    // cast the source code span to a string_view
    std::string_view source_code_view(reinterpret_cast<const char*>(source_code.data()), source_code.size());
    Source::File entrypointSource("source.wgsl", source_code_view);

    Program entrypointProgram = wgsl::reader::Parse(&entrypointSource, readerOptions);
    if (!entrypointProgram.IsValid())
    {
        std::string tintDiagnostics; tintDiagnostics.reserve(1024);
        for (const auto& diag : entrypointProgram.Diagnostics())
        {
            // location, then message
            tintDiagnostics += ToString(diag.source);
            tintDiagnostics += diag.message.Plain() + "\n";
        }
        std::string fullMessage = std::format("WGSL program could not be constructed from given source. Diagnostics:\n{}", tintDiagnostics);
        return std::unexpected(ReportError(sink,
                                           CookError::TargetValidationEntryPointParseFailed,
                                           fullMessage));
    }
    
    BindingComparison result;
    inspector::Inspector epInspector(entrypointProgram);
    using inspector::ResourceBinding;
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

        // now resources should be precisely sorted, we can use the comparison iterator
        BindingComparison epResult = CompareBindings(wgslBindings, bindings);
        if (!epResult.Matches)
        {
            return epResult;
        }
    }

    return BindingComparison{.Matches = true, .Report = ""};
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
    }

    // our frontend pulls out the kind, shape, and access (nicely) into separate fields, so going back to a discrete binding type
    // for most APIs means using these fields together to find the proper binding type (just look at how storage textures work)
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
            // separate function bc it's another switch (3 separate values, potentially)
            return ToTintStorageTextureType(access);
        }
    }

    bool TintTypeAgreesWithReflectionKind(tint::inspector::ResourceBinding::ResourceType tint_kind,
                                          BindingKind kind,
                                          ResourceShape shape,
                                          ResourceAccess access)
    {
        tint::inspector::ResourceBinding::ResourceType reflectionTintType = TintTypeForBindingKind(kind, shape, access);
        return reflectionTintType == tint_kind;
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

    BindingComparison CompareBindings(std::span<const tint::inspector::ResourceBinding> wgsl_bindings,
                                      std::span<const ReflectedBinding*> reflected_bindings) noexcept
    {
        using tint::inspector::ResourceBinding;
        BindingComparison comparison;
        comparison.Matches = true;
        // recently overhauled: now we can use a simple iterator walk to make this O(N+M)
        // instead of O(N*M) or even O(Nlog(M)). because both spans are sorted, they should
        // just match and we don't need to spend time doing nested searches
        auto iterSource = wgsl_bindings.begin();
        auto iterReflected = reflected_bindings.begin();

        while (iterSource != wgsl_bindings.end() && iterReflected != reflected_bindings.end())
        {
            const ResourceBinding& declaredBinding = *iterSource;
            const ReflectedBinding* reflectedBinding = *iterReflected;
            // std::tie to create tuple of references we can directly compare
            const auto declaredTuple = std::tie(declaredBinding.bind_group, declaredBinding.binding);
            // GroupOf/BindingOf return lvalues so we need to make a tuple of copies to compare with the declared tuple
            const auto reflectedTuple = std::make_tuple(GroupOf(*reflectedBinding), BindingOf(*reflectedBinding));

            if (declaredTuple == reflectedTuple)
            {
                std::string_view unmangledName = StripSlangNameMangling(declaredBinding.variable_name);
                std::string scopedName = MakeScopedName(*reflectedBinding); 
                if (unmangledName != scopedName)
                {
                    comparison.Matches = false;
                    comparison.Report += std::format("  wgsl declares @group({}) @binding({}) {} : reflection has "
                                                    "mismatched name \"{}\"\n",
                                                    declaredBinding.bind_group,
                                                    declaredBinding.binding,
                                                    unmangledName,
                                                    reflectedBinding->Name);
                }

                if (!TintTypeAgreesWithReflectionKind(declaredBinding.resource_type,
                                                      reflectedBinding->Kind,
                                                      reflectedBinding->Shape,
                                                      reflectedBinding->Access))
                {
                    comparison.Matches = false;
                    const std::string messageFirstHalf = std::format("  wgsl declares @group({}) @binding({}) {} as {}",
                                                                    declaredBinding.bind_group,
                                                                    declaredBinding.binding,
                                                                    unmangledName,
                                                                    TintTypeToString(declaredBinding.resource_type));
                    const auto tintKind = TintTypeForBindingKind(reflectedBinding->Kind,
                                                                 reflectedBinding->Shape,
                                                                 reflectedBinding->Access);
                    const std::string messageSecondHalf = std::format(" : reflection has kind {}, which needs {}\n",
                                                                    ToString(reflectedBinding->Kind),
                                                                    TintTypeToString(tintKind));
                    comparison.Report += messageFirstHalf + messageSecondHalf;
                }

                ++iterSource;
                ++iterReflected;
            }
            else if (declaredTuple < reflectedTuple)
            {
                // Declared binding is missing in reflection
                comparison.Matches = false;
                comparison.Report += std::format("  wgsl declares @group({}) @binding({}) {} : reflection has "
                                                "no binding at that location\n",
                                                declaredBinding.bind_group,
                                                declaredBinding.binding,
                                                StripSlangNameMangling(declaredBinding.variable_name));
                ++iterSource;
            }
            else
            {
                comparison.Matches = false;
                // declare what is here in the wgsl first, to match format of previous message
                comparison.Report += std::format("  wgsl declares @group({}) @binding({}) {}",
                                                declaredBinding.bind_group,
                                                declaredBinding.binding,
                                                StripSlangNameMangling(declaredBinding.variable_name));
                // now add what is in the reflection
                comparison.Report += std::format(" : reflection has @group({}) @binding({}) {}\n",
                                                std::get<0>(reflectedTuple),
                                                std::get<1>(reflectedTuple),
                                                MakeScopedName(*reflectedBinding));
                ++iterReflected;
            }
        }

        // Drain and report any remaining bindings that weren't matched

        while (iterSource != wgsl_bindings.end())
        {
            const ResourceBinding& declaredBinding = *iterSource;
            comparison.Matches = false;
            comparison.Report += std::format("  wgsl declares @group({}) @binding({}) {} : reflection has "
                                            "no binding at that location\n",
                                            declaredBinding.bind_group,
                                            declaredBinding.binding,
                                            StripSlangNameMangling(declaredBinding.variable_name));
            ++iterSource;
        }

        while (iterReflected != reflected_bindings.end())
        {
            const ReflectedBinding* reflectedBinding = *iterReflected;
            comparison.Matches = false;
            comparison.Report += std::format("  reflection has @group({}) @binding({}) {} : wgsl has "
                                            "no binding at that location\n",
                                            GroupOf(*reflectedBinding),
                                            BindingOf(*reflectedBinding),
                                            MakeScopedName(*reflectedBinding));
            ++iterReflected;
        }

        return comparison;
    }
}

}
