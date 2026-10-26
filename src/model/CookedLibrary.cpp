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

    constexpr uint32_t k_PortableFloat16 = 1u << 0u;
    constexpr uint32_t k_PortableSubgroups = 1u << 1u;

    struct NamedPair
    {
        std::string_view TextureScope;
        std::string_view TextureName;
        std::string_view SamplerScope;
        std::string_view SamplerName;
        auto operator<=>(const NamedPair&) const = default;
    };

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

    // returns true when the source is new, so the caller can record what only a new source adds
    bool InternCodeForm(CodeFormInterners& interners,
                        const EntryPointCode& form,
                        const ProvenanceRecord& origin,
                        CodeFormIndices& indices);
    /** The capabilities that both targets can spell: `f16` and `Float16`, `subgroups` and the `GroupNonUniform` family. */
    uint32_t PortableCapabilities(ShaderCodeFormat format, const std::vector<std::string>& names);
    /** `has_unread_pairs` is set when the code cannot show every pair: a combined texture sampler, which SPIR-V samples
     * with no `OpSampledImage`. */
    CookResult<std::vector<NamedPair>> NamePairs(const CookedModule& module,
                                                 const LibraryVariant& variant,
                                                 size_t entry_point_index,
                                                 bool& has_unread_pairs);
    /** @brief Constructs the frozen codeform tables by consuming the interners */
    CodeFormTables FreezeCodeForm(CodeFormInterners& interners);
    
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

    template<typename PayloadType>
    TableStatistics DescribeTable(const ContentInterner<PayloadType>& interner)
    {
        return TableStatistics{ .HashName = interner.HashName(),
                                .DedupeEnabled = interner.IsEnabled(),
                                .Interning = interner.Statistics() };
    }

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

CodeFormInterners::CodeFormInterners()
    : SourceInterner{ &HashSourceCode, k_HashName },
      VisibilityInterner{ &HashVisibilityList, k_HashName },
      SamplerPairInterner{ &HashSamplerPairList, k_HashName },
      ConstantsInterner{ &HashEntryPointConstants, k_HashName }
{
}

void CodeFormInterners::DisableDedupe() noexcept
{
    SourceInterner.Disable();
    VisibilityInterner.Disable();
    SamplerPairInterner.Disable();
    ConstantsInterner.Disable();
}

InternedModule::InternedModule()
    : ResourceInterner{ &HashReflectedBinding, k_HashName },
      ResourceListInterner{ &HashResourceList, k_HashName },
      FootprintListInterner{ &HashFootprintList, k_HashName },
      RasterInterner{ &HashReflectedRasterState, k_HashName }
{
}

void InternedModule::DisableDedupe() noexcept
{
    ResourceInterner.Disable();
    ResourceListInterner.Disable();
    FootprintListInterner.Disable();
    RasterInterner.Disable();
    Delivered.DisableDedupe();
    Base.DisableDedupe();
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
    module.DisableDedupe();
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
    record.RasterIndices.reserve(variant.EntryPoints.size());
    record.Workgroups.reserve(variant.EntryPoints.size());

    const bool hasBase = !variant.EntryPoints.empty() && variant.EntryPoints.front().Base.has_value();
    if (hasBase)
    {
        record.Base.emplace();
    }

    for (const CompiledEntryPoint& entryPoint : variant.EntryPoints)
    {
        const ProvenanceRecord origin{ .EntryPointName = entryPoint.Name,
                                       .VariantDescription = variant.VariantDescription,
                                       .VariantIndex = variant.VariantIndex };

        const bool newSource = InternCodeForm(module.Delivered, entryPoint.Delivered, origin, record.Delivered);
        if (newSource)
        {
            for (const std::string& name : entryPoint.Delivered.CodeFacts.CapabilityNames)
            {
                const auto position = std::ranges::lower_bound(module.ObservedCapabilities, name);
                if (position == module.ObservedCapabilities.end() || *position != name)
                {
                    module.ObservedCapabilities.insert(position, name);
                }
            }
        }

        if (hasBase)
        {
            InternCodeForm(module.Base, *entryPoint.Base, origin, *record.Base);
        }

        const InternResult raster = module.RasterInterner.Intern(entryPoint.Reflection.Raster, origin);
        record.RasterIndices.push_back(raster.Index);
        record.Workgroups.push_back(entryPoint.Reflection.Workgroup);
    }

    module.Variants.emplace_back(std::move(record));

    return CookError::Success;
}


CookedModule FreezeModuleTables(InternedModule&& interned)
{
    CookedModule module;
    module.Name = std::move(interned.Name);
    module.Space = interned.Space;
    module.SpaceSize = interned.SpaceSize;
    module.EntryPoints = std::move(interned.EntryPoints);
    module.Variants = std::move(interned.Variants);
    module.VariantKeys = std::move(interned.VariantKeys);

    module.Resources = interned.ResourceInterner.ConsumeTable();
    module.ResourceLists = interned.ResourceListInterner.ConsumeTable();
    module.FootprintLists = interned.FootprintListInterner.ConsumeTable();
    module.RasterStates = interned.RasterInterner.ConsumeTable();
    module.ObservedCapabilities = std::move(interned.ObservedCapabilities);
    module.FootprintDeclarations = std::move(interned.FootprintDeclarations);

    module.ResourceTable = DescribeTable(interned.ResourceInterner);
    module.ResourceListTable = DescribeTable(interned.ResourceListInterner);
    module.FootprintListTable = DescribeTable(interned.FootprintListInterner);
    module.RasterTable = DescribeTable(interned.RasterInterner);

    // every variant of one module is compiled with the same flags, so the first decides
    const bool hasBase = !module.Variants.empty() && module.Variants.front().Base.has_value();
    module.Delivered = FreezeCodeForm(interned.Delivered);
    if (hasBase)
    {
        module.Base = FreezeCodeForm(interned.Base);
    }

    return module;
}

const CodeFormTables& FormTables(const CookedModule& module, CodeForm form) noexcept
{
    if (form == CodeForm::Base && module.Base.has_value())
    {
        return *module.Base;
    }
    else
    {
        return module.Delivered;
    }
}

const CodeFormIndices& FormIndices(const LibraryVariant& variant, CodeForm form) noexcept
{
    if (form == CodeForm::Base && variant.Base.has_value())
    {
        return *variant.Base;
    }
    else
    {
        return variant.Delivered;
    }
}

std::span<const std::byte> ResolveSource(const CookedModule& module,
                                         const LibraryVariant& variant,
                                         CodeForm form,
                                         size_t entry_point_index) noexcept
{
    const CodeFormTables& tables = FormTables(module, form);
    const CodeFormIndices& indices = FormIndices(variant, form);
    if (entry_point_index >= indices.SourceIndices.size())
    {
        return {};
    }

    const uint32_t sourceIndex = indices.SourceIndices[entry_point_index];
    if (sourceIndex >= tables.Sources.size())
    {
        return {};
    }

    return tables.Sources[sourceIndex];
}

CookResult<ShaderLayout> ResolveLayout(const CookedModule& module,
                                       const LibraryVariant& variant,
                                       CodeForm form,
                                       size_t entry_point_index)
{
    const CodeFormTables& tables = FormTables(module, form);
    const CodeFormIndices& indices = FormIndices(variant, form);
    if (entry_point_index >= indices.VisibilityIndices.size() ||
        variant.ResourceListIndex >= module.ResourceLists.size() ||
        variant.FootprintListIndex >= module.FootprintLists.size())
    {
        return std::unexpected(CookError::ManifestShaderLayoutVariantIndicesInvalid);
    }

    const uint32_t visibilityIndex = indices.VisibilityIndices[entry_point_index];
    if (visibilityIndex >= tables.VisibilityLists.size())
    {
        return std::unexpected(CookError::ManifestShaderLayoutVisiblityIndexInvalid);
    }

    const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
    const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];

    ShaderLayout layout;
    layout.reserve(tables.VisibilityLists[visibilityIndex].size());
    for (const uint32_t localRsrcIndex : tables.VisibilityLists[visibilityIndex])
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
                                               CodeForm form,
                                               size_t entry_point_index)
{
    const CodeFormTables& tables = FormTables(module, form);
    const CodeFormIndices& indices = FormIndices(variant, form);
    if (entry_point_index >= indices.VisibilityIndices.size() ||
        variant.ResourceListIndex >= module.ResourceLists.size() ||
        variant.FootprintListIndex >= module.FootprintLists.size()) [[unlikely]]
    {
        return std::unexpected(CookError::ManifestShaderLayoutVariantIndicesInvalid);
    }

    const uint32_t visibilityIndex = indices.VisibilityIndices[entry_point_index];
    if (visibilityIndex >= tables.VisibilityLists.size()) [[unlikely]]
    {
        return std::unexpected(CookError::ManifestShaderLayoutVisiblityIndexInvalid);
    }

    const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
    const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];

    std::vector<ResolvedBindingView> layoutView;
    layoutView.reserve(tables.VisibilityLists[visibilityIndex].size());
    for (const uint32_t localRsrcIndex : tables.VisibilityLists[visibilityIndex])
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
            bool unreadPairs = false;
            bool baselineUnreadPairs = false;

            CookResult<std::vector<NamedPair>> pairs =
                NamePairs(shared.Environment, shared.Variant, entryPointIndex, unreadPairs);
            CookResult<std::vector<NamedPair>> baselinePairs =
                NamePairs(shared.Baseline, shared.BaselineVariant, entryPointIndex, baselineUnreadPairs);

            if (!pairs || !baselinePairs)
            {
                return ReportError(sink,
                                   !pairs ? pairs.error() : baselinePairs.error(),
                                   std::format("sampler pairs: module '{}' variant [{}] entry point '{}' has no layout to "
                                               "name its pairs",
                                               library.ModuleNames[shared.Module],
                                               shared.Variant.Description,
                                               shared.Environment.EntryPoints[entryPointIndex].Name));
            }

            if (unreadPairs || baselineUnreadPairs || *pairs == *baselinePairs)
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
        const CookedProfile& profile = library.Profiles[shared.Profile];
        const CookedProfile& baselineProfile = library.Profiles[shared.BaselineProfile];
        for (size_t entryPointIndex = 0u; entryPointIndex < shared.Environment.EntryPoints.size(); ++entryPointIndex)
        {
            const SourceCodeFacts& facts =
                FormTables(shared.Environment, CodeForm::Base)
                    .SourceFacts[FormIndices(shared.Variant, CodeForm::Base).SourceIndices[entryPointIndex]];
            const SourceCodeFacts& baselineFacts =
                FormTables(shared.Baseline, CodeForm::Base)
                    .SourceFacts[FormIndices(shared.BaselineVariant, CodeForm::Base).SourceIndices[entryPointIndex]];

            const uint32_t portable = PortableCapabilities(profile.CodeFormat, facts.CapabilityNames);
            const uint32_t baselinePortable = PortableCapabilities(baselineProfile.CodeFormat, baselineFacts.CapabilityNames);
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

    bool InternCodeForm(CodeFormInterners& interners,
                        const EntryPointCode& form,
                        const ProvenanceRecord& origin,
                        CodeFormIndices& indices)
    {
        const InternResult source = interners.SourceInterner.Intern(form.Code, origin);
        indices.SourceIndices.push_back(source.Index);
        if (source.WasNew)
        {
            interners.SourceFacts.push_back(form.CodeFacts);
        }

        indices.VisibilityIndices.push_back(interners.VisibilityInterner.Intern(form.UsedBindingIndices, origin).Index);
        indices.SamplerPairIndices.push_back(interners.SamplerPairInterner.Intern(form.SamplerTexturePairs, origin).Index);
        indices.ConstantsIndices.push_back(interners.ConstantsInterner.Intern(form.Constants, origin).Index);

        return source.WasNew;
    }

    uint32_t PortableCapabilities(ShaderCodeFormat format, const std::vector<std::string>& names)
    {
        // we could probably make this more robust in the future
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

    CookResult<std::vector<NamedPair>> NamePairs(const CookedModule& module,
                                                 const LibraryVariant& variant,
                                                 size_t entry_point_index,
                                                 bool& has_unread_pairs)
    {
        const CookResult<ShaderLayoutView> layout = ResolveLayoutView(module, variant, CodeForm::Base, entry_point_index);
        if (!layout)
        {
            return std::unexpected(layout.error());
        }

        auto hidesPairs = [](const ResolvedBindingView& binding)
        {
            return binding.Resource->Kind == BindingKind::CombinedTextureSampler;
        };

        has_unread_pairs = std::ranges::any_of(*layout, hidesPairs);

        std::vector<NamedPair> named;
        const ShaderLayoutView& layoutView = *layout;

        const CodeFormIndices& formIndices = FormIndices(variant, CodeForm::Base);
        const uint32_t samplerPairIndex = formIndices.SamplerPairIndices[entry_point_index];
        const CodeFormTables& formTables = FormTables(module, CodeForm::Base);
        const SamplerPairList& pairs = formTables.SamplerPairLists[samplerPairIndex];

        for (const SamplerTexturePair& pair : pairs)
        {
            const ReflectedBinding* texture = layoutView[pair.Texture].Resource;
            const ReflectedBinding* sampler = layoutView[pair.Sampler].Resource;
            named.push_back(NamedPair{ texture->ScopeName, texture->Name, sampler->ScopeName, sampler->Name });
        }

        std::ranges::sort(named);

        return named;
    }

    CodeFormTables FreezeCodeForm(CodeFormInterners& interners)
    {
        CodeFormTables tables;
        tables.Sources = interners.SourceInterner.ConsumeTable();
        tables.SourceFacts = std::move(interners.SourceFacts);
        tables.VisibilityLists = interners.VisibilityInterner.ConsumeTable();
        tables.SamplerPairLists = interners.SamplerPairInterner.ConsumeTable();
        tables.EntryPointConstantsTable = interners.ConstantsInterner.ConsumeTable();

        tables.SourceTable = DescribeTable(interners.SourceInterner);
        tables.VisibilityTable = DescribeTable(interners.VisibilityInterner);
        tables.SamplerPairTable = DescribeTable(interners.SamplerPairInterner);
        tables.ConstantsTable = DescribeTable(interners.ConstantsInterner);

        return tables;
    }

}

} // namespace lodestone

#ifdef __clang__
#pragma clang diagnostic pop
#endif // __clang__
