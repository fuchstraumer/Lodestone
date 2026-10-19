#pragma once
#ifndef LODESTONE_COOKED_LIBRARY_HPP
#define LODESTONE_COOKED_LIBRARY_HPP
#include "ContentHash.hpp"
#include "ContentInterner.hpp"
#include "CookerErrors.hpp"
#include "ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "VariantKey.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>


/** The frozen model every emitter reads.
 *
 * Each cook builds this once. The manifest emitter and the dedup report read the same object, so that
 * two artifacts of one cook cannot describe different shaders.
 *
 * Variants store an index into the module's source table, not the source text. With dedup, equal sources
 * share one entry. With `--no-dedupe`, every artifact gets its own entry. The emitters read through the
 * index either way.
 */
namespace lodestone
{

struct LibraryEntryPoint
{
    std::string Name;
    ShaderStageKind Stage{ ShaderStageKind::Invalid };
};

/**@brief One (module, permutation) pair.
 * `ResourceListIndex` and `FootprintListIndex` are per variant, and say what resources
 * the variant uses and the derived sizes/dims (footprints) of each. VisibilityIndices is
 * the binding locations for each entrypoint - which can vary for the same pointed-to resources */
struct LibraryVariant
{
    uint64_t Index{ 0u };
    std::string Suffix;
    std::string Description;
    PermutationAssignment Active;
    CanonicalAssignment Canonical;
    uint32_t ResourceListIndex{ 0u };
    uint32_t FootprintListIndex{ 0u };
    std::vector<uint32_t> SourceIndices;
    std::vector<uint32_t> VisibilityIndices;
    std::vector<uint32_t> RasterIndices;
    std::vector<WorkgroupSize> Workgroups;
    /** One index into `CookedModule::SamplerPairLists` for each entry point. */
    std::vector<uint32_t> SamplerPairIndices;
};

/**@brief Indices into `CookedModule::Resources`: the resources one variant declares. */
using ResourceList = std::vector<uint32_t>;
/**@brief Indices into a variant's own resource list: the resources one entry point reads. Local, so the
 * list stays valid when the resource table changes, and small enough to collapse well. */
using VisibilityList = std::vector<uint32_t>;
/**@brief The sampler and texture pairs of one entry point, as positions in its visibility list. */
using SamplerPairList = std::vector<SamplerTexturePair>;
/**@brief One footprint for each entry of a variant's resource list. */
using FootprintList = std::vector<ResourceFootprint>;
/**@brief What a caller gets for one entry point: the resources it reads, joined with their footprints. */
using ShaderLayout = std::vector<ResolvedBinding>;
using ShaderLayoutView = std::vector<ResolvedBindingView>;

ContentHashValue HashIndexList(const std::vector<uint32_t>& indices) noexcept;
ContentHashValue HashSourceCode(const std::vector<std::byte>& source) noexcept;
ContentHashValue HashResourceList(const ResourceList& resources) noexcept;
ContentHashValue HashVisibilityList(const VisibilityList& visibility) noexcept;
ContentHashValue HashFootprintList(const FootprintList& footprints) noexcept;
ContentHashValue HashSamplerPairList(const SamplerPairList& pairs) noexcept;
void AppendFootprintHash(StreamingHash& hasher, const ResourceFootprint& footprint) noexcept;

/**@brief The hash that generated a table, whether or not it used dedupe logic,
 *  and the results of the process (regardless of if it ran dedupe or not) */
struct TableStatistics
{
    std::string_view HashName;
    bool DedupeEnabled{ true };
    InternerStatistics Interning;
};

/**@brief An interned module is procedurally built by adding variants as they arrive, and represents
 *  the deduplicated (if enabled) contents of a Slang module bundled together for the final cooking
 *  stage to process as it sees fit. This object actually *does* the interning piece by piece, as
 *  compared to `CookedModule` which holds the completed results from this processing.
 *
 * @note As of now, there is no `InternedLibrary`: interning is a per-module operation for now.
 * In the future, we could intern libraries - but those would be most efficient running on interned
 * modules, anyways. */
struct InternedModule
{
    std::string Name;
    const PermutationSpace* Space{ nullptr };
    uint64_t SpaceSize{ 0u };
    std::vector<LibraryEntryPoint> EntryPoints;
    std::vector<VariantKey> VariantKeys;
    std::vector<LibraryVariant> Variants;
    // Every interner takes the name from `k_HashName`, because the name reaches the output and a new
    // hash needs a new name. A literal here is a second place to change, and the two spellings drifted
    // apart once already.
    ContentInterner<std::vector<std::byte>> SourceInterner{ &HashSourceCode, k_HashName };
    ContentInterner<ReflectedBinding> ResourceInterner{ &HashReflectedBinding, k_HashName };
    ContentInterner<ResourceList> ResourceListInterner{ &HashResourceList, k_HashName };
    ContentInterner<FootprintList> FootprintListInterner{ &HashFootprintList, k_HashName };
    ContentInterner<VisibilityList> VisibilityInterner{ &HashVisibilityList, k_HashName };
    ContentInterner<ReflectedRasterState> RasterInterner{ &HashReflectedRasterState, k_HashName };
    ContentInterner<SamplerPairList> SamplerPairInterner{ &HashSamplerPairList, k_HashName };
    // Required capabilities read back from the target's validator in any variant, stored as names since those
    // are easiest to associate with renderer runtime caps (e.g., Vulkan device features/extensions)
    std::vector<std::string> ObservedCapabilities;

    void DisableDedupe() noexcept
    {
        SourceInterner.Disable();
        ResourceInterner.Disable();
        ResourceListInterner.Disable();
        FootprintListInterner.Disable();
        VisibilityInterner.Disable();
        RasterInterner.Disable();
        SamplerPairInterner.Disable();
    }
};

/**@brief Interned tables and information about how efficiently they were built. We store these
 * separately as they collapse at very different rates, so it's worth having insight into each.
 * This object holds the results of the interning process, but doesn't do it itself. */
struct CookedModule
{
    std::string Name;
    const PermutationSpace* Space{ nullptr };
    /** @brief Size of the dense index range, holes included. */
    uint64_t SpaceSize{ 0u };
    std::vector<LibraryEntryPoint> EntryPoints;
    std::vector<std::vector<std::byte>> Sources;
    std::vector<ReflectedBinding> Resources;
    std::vector<ResourceList> ResourceLists;
    std::vector<FootprintList> FootprintLists;
    std::vector<VisibilityList> VisibilityLists;
    std::vector<ReflectedRasterState> RasterStates;
    std::vector<SamplerPairList> SamplerPairLists;
    std::vector<VariantKey> VariantKeys;
    std::vector<LibraryVariant> Variants;
    std::vector<std::string> ObservedCapabilities;

    TableStatistics SourceTable;
    TableStatistics ResourceTable;
    TableStatistics ResourceListTable;
    TableStatistics FootprintListTable;
    TableStatistics VisibilityTable;
    TableStatistics RasterTable;
    TableStatistics SamplerPairTable;
};

/** @brief One cooked form of the library: a target and the access model it binds with. */
struct CookedProfile
{
    std::string TargetName;
    PlacementKind AccessModel{ PlacementKind::None };
    ShaderCodeFormat CodeFormat{ ShaderCodeFormat::None };
    /** This only indicates if we could (or chose to) read the sampler pairs - not if any 
      * implicitly exist in the shader code itself. */
    bool SamplerTexturePairsRecorded{ false };
};

/**@brief The whole cook: every module, cooked for every profile.
 * `Environments` is just the flattened vector for the profiles * modulenames grid. 
 * Modules are listed together for each profile, so they're linear in memory.
 * Empty entries just mean that the module was not cooked for that profile. */
struct CookedLibrary
{
    std::vector<std::string> ModuleNames;
    std::vector<CookedProfile> Profiles;
    std::vector<std::optional<CookedModule>> Environments;
};

void DisableDedupe(InternedModule& module) noexcept;

/** Adds one compiled variant to the module, interning each source, layout, and raster state */
CookError AppendVariantToModule(InternedModule& module,
                                const CompiledVariant& variant,
                                const VariantDescriptor& descriptor);

/**@brief The cross-target validator for sampler and texture pairs. Since profiles cook the same inputs to
 * different output formats, we can use this to validate that both paths produce consistent sampler and
 * texture pairings.
 *
 * Skips profiles that did not or cannot record these pairings, as well as entry points that use combined 
 * texture samplers since those are not supported by all profiles.
 *
 * @returns `SamplerPairsTargetMismatch` and names the module, the profiles, the variant, and the entry point. */
CookError VerifySamplerPairsAcrossProfiles(const CookedLibrary& library);

/**@brief "Freezes" the module by *consuming* `InternedModule`. CookedModule takes the results, gathering
 * all the data so far in one place. The intent was that CookedModule is a bundle of data, it doesn't hold
 * systems that build or modify that data. */
CookedModule FreezeModuleTables(InternedModule&& interned);

/**@brief Resolves what a caller would get back for one entry point of one variant. The round-trip check
 * compares this against the text the compiler produced. */
std::span<const std::byte> ResolveSource(const CookedModule& module,
                                         const LibraryVariant& variant,
                                         size_t entry_point_index) noexcept;

/**@brief Retrieve the final shader layout built for one entry point of one variant
 * within a module.*/
CookResult<ShaderLayout> ResolveLayout(const CookedModule& module,
                                       const LibraryVariant& variant,
                                       size_t entry_point_index);

CookResult<ShaderLayoutView> ResolveLayoutView(const CookedModule& module,
                                               const LibraryVariant& variant,
                                               size_t entry_point_index);

} // namespace lodestone

#endif // !LODESTONE_COOKED_LIBRARY_HPP
