#pragma once
#include "permute/PermutationAssignment.hpp"
#ifndef LODESTONE_COOKED_LIBRARY_HPP
#define LODESTONE_COOKED_LIBRARY_HPP
#include "ContentHash.hpp"
#include "ContentInterner.hpp"
#include "CookerErrors.hpp"
#include "ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationSpace.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


/** The frozen model every emitter reads.
 *
 * One cook builds this once. The C++ emitter, the binary manifest emitter, and the dedup report all
 * read the same object, so two artifacts of one cook cannot describe different shaders.
 *
 * A variant does not hold its source text. It holds an index into the module's source table. Today
 * that table has one entry for each artifact, so the indices are an identity mapping. Dedup replaces
 * how the table gets filled, and nothing else changes: the emitters already read through the index.
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
    uint32_t Index{ 0u };
    std::string Suffix;
    std::string Description;
    CanonicalAssignment Canonical;
    uint32_t ResourceListIndex{ 0u };
    uint32_t FootprintListIndex{ 0u };
    std::vector<uint32_t> SourceIndices;
    std::vector<uint32_t> VisibilityIndices;
    std::vector<uint32_t> RasterIndices;
    std::vector<WorkgroupSize> Workgroups;
};

/**@brief Indices into `CookedModule::Resources`: the resources one variant declares. */
using ResourceList = std::vector<uint32_t>;
/**@brief Indices into a variant's own resource list: the resources one entry point reads. Local, so the
 * list stays valid when the resource table changes, and small enough to collapse well. */
using VisibilityList = std::vector<uint32_t>;
/**@brief One footprint for each entry of a variant's resource list. */
using FootprintList = std::vector<ResourceFootprint>;
/**@brief What a caller gets for one entry point: the resources it reads, joined with their footprints. */
using ShaderLayout = std::vector<ResolvedBinding>;
using ShaderLayoutView = std::vector<ResolvedBindingView>;

ContentHashValue HashIndexList(const std::vector<uint32_t>& indices) noexcept;
ContentHashValue HashSourceString(const std::string& source) noexcept;
ContentHashValue HashResourceList(const ResourceList& resources) noexcept;
ContentHashValue HashVisibilityList(const VisibilityList& visibility) noexcept;
ContentHashValue HashFootprintList(const FootprintList& footprints) noexcept;

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
    uint32_t SpaceSize{ 0u };
    std::vector<LibraryEntryPoint> EntryPoints;
    std::vector<LibraryVariant> Variants;
    // Every interner takes the name from `k_HashName`, because the name reaches the output and a new
    // hash needs a new name. A literal here is a second place to change, and the two spellings drifted
    // apart once already.
    ContentInterner<std::string> SourceInterner{ &HashSourceString, k_HashName };
    ContentInterner<ReflectedBinding> ResourceInterner{ &HashReflectedBinding, k_HashName };
    ContentInterner<ResourceList> ResourceListInterner{ &HashResourceList, k_HashName };
    ContentInterner<FootprintList> FootprintListInterner{ &HashFootprintList, k_HashName };
    ContentInterner<VisibilityList> VisibilityInterner{ &HashVisibilityList, k_HashName };
    ContentInterner<ReflectedRasterState> RasterInterner{ &HashReflectedRasterState, k_HashName };
};

/**@brief Interned tables and information about how efficiently they were built. We store these
 * separately as they collapse at very different rates, so it's worth having insight into each.
 * This object holds the results of the interning process, but doesn't do it itself. */
struct CookedModule
{
    std::string Name;
    const PermutationSpace* Space{ nullptr };
    /** @brief Size of the dense index range, holes included. */
    uint32_t SpaceSize{ 0u };
    std::vector<LibraryEntryPoint> EntryPoints;
    std::vector<std::string> Sources;
    std::vector<ReflectedBinding> Resources;
    std::vector<ResourceList> ResourceLists;
    std::vector<FootprintList> FootprintLists;
    std::vector<VisibilityList> VisibilityLists;
    std::vector<ReflectedRasterState> RasterStates;
    std::vector<LibraryVariant> Variants;

    TableStatistics SourceTable;
    TableStatistics ResourceTable;
    TableStatistics ResourceListTable;
    TableStatistics FootprintListTable;
    TableStatistics VisibilityTable;
    TableStatistics RasterTable;
};

/** @brief Temp. Slang is designed for big libraries of shaders, but for now we're trying
 *  to ensure core module handling is robust and modular: most Library operations will just
 *  be further fold/combine operations over the datastructures we already have. */
struct CookedLibrary
{
    std::vector<CookedModule> Modules;
};

void DisableDedupe(InternedModule& module) noexcept;

/** Adds one compiled variant to the module, interning each source, layout, and raster state. */
CookError AppendVariantToModule(InternedModule& module,
                                const CompiledVariant& variant,
                                const CanonicalAssignment& canonical);

/**@brief "Freezes" the module by *consuming* `InternedModule`. CookedModule takes the results, gathering
 * all the data so far in one place. The intent was that CookedModule is a bundle of data, it doesn't hold
 * systems that build or modify that data. */
CookedModule FreezeModuleTables(InternedModule&& interned);

/**@brief Resolves what a caller would get back for one entry point of one variant. The round-trip check
 * compares this against the text the compiler produced. */
std::string_view ResolveSource(const CookedModule& module,
                               const LibraryVariant& variant,
                               size_t entry_point_index) noexcept;

/**@brief Retrieve the final shader layout built for one entry point of one variant
 * within a module.*/
ShaderLayout ResolveLayout(const CookedModule& module,
                           const LibraryVariant& variant,
                           size_t entry_point_index);

ShaderLayoutView ResolveLayoutView(const CookedModule& module,
                                   const LibraryVariant& variant,
                                   size_t entry_point_index);

} // namespace lodestone

#endif // !LODESTONE_COOKED_LIBRARY_HPP