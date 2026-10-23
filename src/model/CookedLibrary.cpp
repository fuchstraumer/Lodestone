#include "model/CookedLibrary.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include "model/ContentHash.hpp"
#include "model/ContentInterner.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <expected>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
#endif // __clang__

namespace lodestone
{

namespace
{

    /** One variant that the baseline profile and another profile both cooked, for the same module. */
    struct SharedVariant
    {
        size_t Module;
        size_t Profile;
        size_t BaselineProfile;
        const CookedModule& Environment;
        const CookedModule& Baseline;
        const LibraryVariant& Variant;
        const LibraryVariant& BaselineVariant;
    };

    /** Calls `visit` for each variant that the first recording profile of a module and a later one both cooked. Each
     * profile applies its own policy, so only the shared keys are compared. */
    template <typename Visitor>
    CookError VisitSharedVariants(const CookedLibrary& library, bool CookedProfile::* recorded, Visitor&& visit)
    {
        const size_t moduleCount = library.ModuleNames.size();
        for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
        {
            const CookedModule* baseline = nullptr;
            size_t baselineProfile = 0u;
            std::unordered_map<VariantKey, size_t> baselineVariants;
            for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
            {
                const std::optional<CookedModule>& environment =
                    library.Environments[(profileIndex * moduleCount) + moduleIndex];

                if (!environment.has_value() || !(library.Profiles[profileIndex].*recorded))
                {
                    continue;
                }

                if (baseline == nullptr)
                {
                    baseline = std::to_address(environment);
                    baselineProfile = profileIndex;
                    for (auto&& [variantIndex, variantKey] : std::span<const VariantKey>{ baseline->VariantKeys } | std::views::enumerate)
                    {
                        baselineVariants.emplace(variantKey, variantIndex);
                    }
                    continue;
                }

                for (size_t variantIndex = 0u; variantIndex < environment->VariantKeys.size(); ++variantIndex)
                {
                    const auto match = baselineVariants.find(environment->VariantKeys[variantIndex]);
                    if (match == baselineVariants.end())
                    {
                        continue;
                    }

                    const CookError visited = visit(SharedVariant{ .Module = moduleIndex,
                                                                   .Profile = profileIndex,
                                                                   .BaselineProfile = baselineProfile,
                                                                   .Environment = *environment,
                                                                   .Baseline = *baseline,
                                                                   .Variant = environment->Variants[variantIndex],
                                                                   .BaselineVariant = baseline->Variants[match->second] });
                    if (visited != CookError::Success)
                    {
                        return visited;
                    }
                }
            }
        }

        return CookError::Success;
    }

    constexpr uint32_t k_PortableFloat16 = 1u << 0u;
    constexpr uint32_t k_PortableSubgroups = 1u << 1u;

    /** The capabilities that both targets can spell: `f16` and `Float16`, `subgroups` and the `GroupNonUniform` family. */
    uint32_t PortableCapabilities(ShaderCodeFormat format, const std::vector<std::string>& names)
    {
        uint32_t portable = 0u;
        for (const std::string& name : names)
        {
            if (format == ShaderCodeFormat::Wgsl)
            {
                portable |= name == "f16" ? k_PortableFloat16 : 0u;
                portable |= name == "subgroups" ? k_PortableSubgroups : 0u;
            }
            else
            {
                portable |= name == "Float16" ? k_PortableFloat16 : 0u;
                portable |= name.starts_with("GroupNonUniform") ? k_PortableSubgroups : 0u;
            }
        }
        return portable;
    }

    /** Texture scope, texture name, sampler scope, sampler name. */
    using NamedPair = std::array<std::string_view, 4u>;

    /** Empty with `has_combined` set for a combined texture sampler. */
    CookResult<std::vector<NamedPair>> NamePairs(const CookedModule& module,
                                                 const LibraryVariant& variant,
                                                 size_t entry_point_index,
                                                 bool& has_combined);

} // namespace

ContentHashValue HashIndexList(const std::vector<uint32_t>& indices) noexcept
{
    // this feels a little.... UB
    const std::span<const uint32_t> indicesSpan{ indices.data(), indices.size() };
    const std::span<const std::byte> indicesBytesSpan = std::as_bytes(indicesSpan);
    return HashBytes(indicesBytesSpan);
}

ContentHashValue HashSourceCode(const std::vector<std::byte>& source) noexcept
{
    const std::span<const std::byte> bytesSpan{ source.data(), source.size() };
    return HashBytes(bytesSpan);
}

ContentHashValue HashResourceList(const ResourceList& resources) noexcept
{
    return HashIndexList(resources);
}

ContentHashValue HashVisibilityList(const std::vector<uint32_t>& visibility) noexcept
{
    return HashIndexList(visibility);
}

void AppendFootprintHash(StreamingHash& hasher, const ResourceFootprint& footprint) noexcept
{
    std::array<uint64_t, 4> footprintScalars{ 0u, 0u, 0u, 0u };
    if (const BufferFootprint* buffer = std::get_if<BufferFootprint>(&footprint))
    {
        footprintScalars[0] = static_cast<uint64_t>(footprint.index());
        footprintScalars[1] = static_cast<uint64_t>(buffer->ElementCount);
        hasher.Append(std::span{ footprintScalars.data(), 2u });
        hasher.Append(std::string_view{ buffer->Expression });
    }
    else if (const TextureFootprint* texture = std::get_if<TextureFootprint>(&footprint))
    {
        footprintScalars[0] = static_cast<uint64_t>(footprint.index());
        footprintScalars[1] = static_cast<uint64_t>(texture->ExtentX);
        footprintScalars[2] = static_cast<uint64_t>(texture->ExtentY);
        footprintScalars[3] = static_cast<uint64_t>(texture->ExtentZ);
        hasher.Append(std::span{ footprintScalars.data(), footprintScalars.size() });
        hasher.Append(std::string_view{ texture->Expression });
    }
    else
    {
        hasher.Append(static_cast<uint64_t>(footprint.index()));
    }
}

ContentHashValue HashSamplerPairList(const SamplerPairList& pairs) noexcept
{
    static_assert(sizeof(SamplerTexturePair) == 2u * sizeof(uint32_t));
    return HashBytes(std::as_bytes(std::span<const SamplerTexturePair>{ pairs.data(), pairs.size() }));
}

ContentHashValue HashFootprintList(const std::vector<ResourceFootprint>& footprints) noexcept
{
    thread_local StreamingHash compositeHasher;
    compositeHasher.Reset();
    for (const ResourceFootprint& footprint : footprints)
    {
        AppendFootprintHash(compositeHasher, footprint);
    }

    return compositeHasher.Finalize();
}

void DisableDedupe(InternedModule& module) noexcept
{
    // todo: brittle when interners change. Can we do something smarter?
    module.SourceInterner.Disable();
    module.ResourceInterner.Disable();
    module.ResourceListInterner.Disable();
    module.FootprintListInterner.Disable();
    module.VisibilityInterner.Disable();
    module.RasterInterner.Disable();
    module.SamplerPairInterner.Disable();
    module.ConstantsInterner.Disable();
}

CookError AppendVariantToModule(InternedModule& module,
                                const CompiledVariant& variant,
                                const VariantDescriptor& descriptor,
                                DiagnosticSink& sink)
{
    if (variant.EntryPoints.size() != module.EntryPoints.size())
    {
        return ReportError(sink,
                           CookError::ReflectionMismatch,
                           std::format("variant [{}] has {} entry points, but module {} declares {}",
                                       variant.VariantDescription,
                                       variant.EntryPoints.size(),
                                       module.Name,
                                       module.EntryPoints.size()));
    }

    const ProvenanceRecord variantOrigin{ .EntryPointName = {},
                                          .VariantDescription = variant.VariantDescription,
                                          .VariantIndex = variant.VariantIndex };

    ResourceList resources;
    resources.reserve(variant.Bindings.size());
    for (const ReflectedBinding& binding : variant.Bindings)
    {
        resources.push_back(module.ResourceInterner.Intern(binding, variantOrigin).Index);
    }

    LibraryVariant record;
    record.Index = variant.VariantIndex;
    record.Suffix = variant.VariantSuffix;
    record.Description = variant.VariantDescription;
    record.Active = descriptor.Active;
    record.Canonical = descriptor.Canonical;
    record.ResourceListIndex = module.ResourceListInterner.Intern(resources, variantOrigin).Index;
    record.FootprintListIndex = module.FootprintListInterner.Intern(variant.Footprints, variantOrigin).Index;
    record.SourceIndices.reserve(variant.EntryPoints.size());
    record.VisibilityIndices.reserve(variant.EntryPoints.size());
    record.RasterIndices.reserve(variant.EntryPoints.size());
    record.Workgroups.reserve(variant.EntryPoints.size());
    record.SamplerPairIndices.reserve(variant.EntryPoints.size());
    record.ConstantsIndices.reserve(variant.EntryPoints.size());
    for (size_t entryPointIndex = 0u; entryPointIndex < variant.EntryPoints.size(); ++entryPointIndex)
    {
        const CompiledEntryPoint& entryPoint = variant.EntryPoints[entryPointIndex];
        const ProvenanceRecord origin{ .EntryPointName = entryPoint.Name,
                                       .VariantDescription = variant.VariantDescription,
                                       .VariantIndex = variant.VariantIndex };

        const InternResult source = module.SourceInterner.Intern(entryPoint.Code, origin);
        record.SourceIndices.push_back(source.Index);
        if (source.WasNew)
        {
            module.SourceFacts.push_back(entryPoint.CodeFacts);
            for (const std::string& name : entryPoint.CodeFacts.CapabilityNames)
            {
                const auto position = std::ranges::lower_bound(module.ObservedCapabilities, name);
                if (position == module.ObservedCapabilities.end() || *position != name)
                {
                    module.ObservedCapabilities.insert(position, name);
                }
            }
        }

        const InternResult visibility =
            module.VisibilityInterner.Intern(entryPoint.Reflection.UsedBindingIndices, origin);
        record.VisibilityIndices.push_back(visibility.Index);

        const InternResult raster = module.RasterInterner.Intern(entryPoint.Reflection.Raster, origin);
        record.RasterIndices.push_back(raster.Index);

        record.Workgroups.push_back(entryPoint.Reflection.Workgroup);

        const InternResult samplerPairs = module.SamplerPairInterner.Intern(entryPoint.SamplerTexturePairs, origin);
        record.SamplerPairIndices.push_back(samplerPairs.Index);

        const InternResult constants = module.ConstantsInterner.Intern(entryPoint.Reflection.Constants, origin);
        record.ConstantsIndices.push_back(constants.Index);
    }

    module.Variants.emplace_back(std::move(record));

    return CookError::Success;
}

namespace
{

    template<typename PayloadType>
    TableStatistics DescribeTable(const ContentInterner<PayloadType>& interner)
    {
        return TableStatistics{ .HashName = interner.HashName(),
                                .DedupeEnabled = interner.IsEnabled(),
                                .Interning = interner.Statistics() };
    }

} // namespace

CookedModule FreezeModuleTables(InternedModule&& interned)
{
    CookedModule module;
    module.Name = std::move(interned.Name);
    module.Space = interned.Space;
    module.SpaceSize = interned.SpaceSize;
    module.EntryPoints = std::move(interned.EntryPoints);
    module.Variants = std::move(interned.Variants);
    module.VariantKeys = std::move(interned.VariantKeys);

    module.Sources = interned.SourceInterner.ConsumeTable();
    module.Resources = interned.ResourceInterner.ConsumeTable();
    module.ResourceLists = interned.ResourceListInterner.ConsumeTable();
    module.FootprintLists = interned.FootprintListInterner.ConsumeTable();
    module.VisibilityLists = interned.VisibilityInterner.ConsumeTable();
    module.RasterStates = interned.RasterInterner.ConsumeTable();
    module.SamplerPairLists = interned.SamplerPairInterner.ConsumeTable();
    module.EntryPointConstantsTable = interned.ConstantsInterner.ConsumeTable();
    module.SourceFacts = std::move(interned.SourceFacts);
    module.ObservedCapabilities = std::move(interned.ObservedCapabilities);
    module.FootprintDeclarations = std::move(interned.FootprintDeclarations);

    module.SourceTable = DescribeTable(interned.SourceInterner);
    module.ResourceTable = DescribeTable(interned.ResourceInterner);
    module.ResourceListTable = DescribeTable(interned.ResourceListInterner);
    module.FootprintListTable = DescribeTable(interned.FootprintListInterner);
    module.VisibilityTable = DescribeTable(interned.VisibilityInterner);
    module.RasterTable = DescribeTable(interned.RasterInterner);
    module.SamplerPairTable = DescribeTable(interned.SamplerPairInterner);
    module.ConstantsTable = DescribeTable(interned.ConstantsInterner);

    return module;
}

std::span<const std::byte> ResolveSource(const CookedModule& module,
                                         const LibraryVariant& variant,
                                         size_t entry_point_index) noexcept
{
    if (entry_point_index >= variant.SourceIndices.size())
    {
        return {};
    }

    const uint32_t sourceIndex = variant.SourceIndices[entry_point_index];
    if (sourceIndex >= module.Sources.size())
    {
        return {};
    }

    return module.Sources[sourceIndex];
}

CookResult<ShaderLayout> ResolveLayout(const CookedModule& module,
                                       const LibraryVariant& variant,
                                       size_t entry_point_index)
{
    if (entry_point_index >= variant.VisibilityIndices.size() ||
        variant.ResourceListIndex >= module.ResourceLists.size() ||
        variant.FootprintListIndex >= module.FootprintLists.size())
    {
        return std::unexpected(CookError::ManifestShaderLayoutVariantIndicesInvalid);
    }

    const uint32_t visibilityIndex = variant.VisibilityIndices[entry_point_index];
    if (visibilityIndex >= module.VisibilityLists.size())
    {
        return std::unexpected(CookError::ManifestShaderLayoutVisiblityIndexInvalid);
    }

    const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
    const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];

    ShaderLayout layout;
    layout.reserve(module.VisibilityLists[visibilityIndex].size());
    for (const uint32_t localRsrcIndex : module.VisibilityLists[visibilityIndex])
    {
        if (localRsrcIndex >= resources.size() || resources[localRsrcIndex] >= module.Resources.size())
        {
            return std::unexpected(CookError::ManifestShaderLayoutResourceListIndexInvalid);
        }

        layout.emplace_back(module.Resources[resources[localRsrcIndex]],
                            localRsrcIndex < footprints.size() ? footprints[localRsrcIndex] : ResourceFootprint{});
    }

    return layout;
}

CookResult<ShaderLayoutView> ResolveLayoutView(const CookedModule& module,
                                               const LibraryVariant& variant,
                                               size_t entry_point_index)
{
    if (entry_point_index >= variant.VisibilityIndices.size() ||
        variant.ResourceListIndex >= module.ResourceLists.size() ||
        variant.FootprintListIndex >= module.FootprintLists.size()) [[unlikely]]
    {
        return std::unexpected(CookError::ManifestShaderLayoutVariantIndicesInvalid);
    }

    const uint32_t visibilityIndex = variant.VisibilityIndices[entry_point_index];
    if (visibilityIndex >= module.VisibilityLists.size()) [[unlikely]]
    {
        return std::unexpected(CookError::ManifestShaderLayoutVisiblityIndexInvalid);
    }

    const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
    const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];

    std::vector<ResolvedBindingView> layoutView;
    layoutView.reserve(module.VisibilityLists[visibilityIndex].size());
    for (const uint32_t localRsrcIndex : module.VisibilityLists[visibilityIndex])
    {
        if (localRsrcIndex >= resources.size() || resources[localRsrcIndex] >= module.Resources.size())
            [[unlikely]]
        {
            return std::unexpected(CookError::ManifestShaderLayoutResourceListIndexInvalid);
        }


        layoutView.emplace_back(&module.Resources[resources[localRsrcIndex]],
                                localRsrcIndex < footprints.size() ? &footprints[localRsrcIndex] : nullptr);
    }

    return layoutView;
}

CookError VerifySamplerPairsAcrossProfiles(const CookedLibrary& library, DiagnosticSink& sink)
{
    return VisitSharedVariants(library,
                               &CookedProfile::SamplerTexturePairsRecorded,
                               [&library, &sink](const SharedVariant& shared) -> CookError
                               {
        for (size_t entryPointIndex = 0u; entryPointIndex < shared.Environment.EntryPoints.size(); ++entryPointIndex)
        {
            bool combined = false;
            bool baselineCombined = false;

            const CookResult<std::vector<NamedPair>> pairs =
                NamePairs(shared.Environment, shared.Variant, entryPointIndex, combined);
            const CookResult<std::vector<NamedPair>> baselinePairs =
                NamePairs(shared.Baseline, shared.BaselineVariant, entryPointIndex, baselineCombined);

            if (!pairs || !baselinePairs)
            {
                return !pairs ? pairs.error() : baselinePairs.error();
            }

            if (combined || baselineCombined || *pairs == *baselinePairs)
            {
                continue;
            }

            return ReportError(sink,
                               CookError::SamplerPairsTargetMismatch,
                               std::format("sampler pairs: module '{}' variant [{}] entry point '{}' samples {} pairs in "
                                           "profile '{}' and {} pairs in profile '{}', or one pair differs",
                                           library.ModuleNames[shared.Module],
                                           shared.Variant.Description,
                                           shared.Environment.EntryPoints[entryPointIndex].Name,
                                           pairs->size(),
                                           library.Profiles[shared.Profile].TargetName,
                                           baselinePairs->size(),
                                           library.Profiles[shared.BaselineProfile].TargetName));
        }
        return CookError::Success;
    });
}

CookError VerifySourceFactsAcrossProfiles(const CookedLibrary& library, DiagnosticSink& sink)
{
    return VisitSharedVariants(
        library,
        &CookedProfile::CodeFactsRead,
        [&library, &sink](const SharedVariant& shared) -> CookError
        {
        const ShaderCodeFormat format = library.Profiles[shared.Profile].CodeFormat;
        const ShaderCodeFormat baselineFormat = library.Profiles[shared.BaselineProfile].CodeFormat;
        for (size_t entryPointIndex = 0u; entryPointIndex < shared.Environment.EntryPoints.size(); ++entryPointIndex)
        {
            const SourceCodeFacts& facts = shared.Environment.SourceFacts[shared.Variant.SourceIndices[entryPointIndex]];
            const SourceCodeFacts& baselineFacts =
                shared.Baseline.SourceFacts[shared.BaselineVariant.SourceIndices[entryPointIndex]];

            const uint32_t portable = PortableCapabilities(format, facts.CapabilityNames);
            const uint32_t baselinePortable = PortableCapabilities(baselineFormat, baselineFacts.CapabilityNames);
            if (facts.WorkgroupMemoryBytes == baselineFacts.WorkgroupMemoryBytes && portable == baselinePortable)
            {
                continue;
            }

            return ReportError(sink,
                               CookError::SourceFactsTargetMismatch,
                               std::format("source facts: module '{}' variant [{}] entry point '{}' needs {} bytes of workgroup "
                                           "memory and portable capabilities {:#x} in profile '{}', and {} bytes and {:#x} "
                                           "in profile '{}'",
                                           library.ModuleNames[shared.Module],
                                           shared.Variant.Description,
                                           shared.Environment.EntryPoints[entryPointIndex].Name,
                                           facts.WorkgroupMemoryBytes,
                                           portable,
                                           library.Profiles[shared.Profile].TargetName,
                                           baselineFacts.WorkgroupMemoryBytes,
                                           baselinePortable,
                                           library.Profiles[shared.BaselineProfile].TargetName));
        }
        return CookError::Success;
    });
}

namespace
{

    CookResult<std::vector<NamedPair>> NamePairs(const CookedModule& module,
                                                 const LibraryVariant& variant,
                                                 size_t entry_point_index,
                                                 bool& has_combined)
    {
        const CookResult<ShaderLayoutView> layout = ResolveLayoutView(module, variant, entry_point_index);
        if (!layout)
        {
            return std::unexpected(layout.error());
        }

        auto isCombinedTextureSampler = [](const ResolvedBindingView& binding)
        {
            return binding.Resource->Kind == BindingKind::CombinedTextureSampler;
        };

        has_combined = std::ranges::any_of(*layout, isCombinedTextureSampler);

        std::vector<NamedPair> named;
        const ShaderLayoutView& layout_view = *layout;
        for (const SamplerTexturePair& pair : module.SamplerPairLists[variant.SamplerPairIndices[entry_point_index]])
        {
            const ReflectedBinding* texture = layout_view[pair.Texture].Resource;
            const ReflectedBinding* sampler = layout_view[pair.Sampler].Resource;
            named.push_back(NamedPair{ texture->ScopeName, texture->Name, sampler->ScopeName, sampler->Name });
        }

        std::ranges::sort(named);
        
        return named;
    }

}

} // namespace lodestone

#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
