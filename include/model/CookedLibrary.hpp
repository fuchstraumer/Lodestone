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


/** The frozen model every emitter reads, so that two artifacts of one cook cannot describe different shaders. */
namespace lodestone
{

class DiagnosticSink;

struct LibraryEntryPoint
{
    std::string Name;
    ShaderStageKind Stage{ ShaderStageKind::Invalid };
};

/** @brief One variant: indices into the module's tables, per variant and per entry point. */
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
    std::vector<uint32_t> SamplerPairIndices;
    std::vector<uint32_t> ConstantsIndices;
};

/** @brief The axes that a resource's size attributes name, directly or through derived constants. */
struct FootprintAxisDeclaration
{
    std::string ScopeName;
    std::string Name;
    /** Sorted and unique. */
    std::vector<uint32_t> Axes;
};

using ResourceList = std::vector<uint32_t>;
/** @brief Positions in the variant's resource list, so the list survives table changes and collapses well. */
using VisibilityList = std::vector<uint32_t>;
using SamplerPairList = std::vector<SamplerTexturePair>;
using FootprintList = std::vector<ResourceFootprint>;
using ShaderLayout = std::vector<ResolvedBinding>;
using ShaderLayoutView = std::vector<ResolvedBindingView>;

ContentHashValue HashIndexList(const std::vector<uint32_t>& indices) noexcept;
ContentHashValue HashSourceCode(const std::vector<std::byte>& source) noexcept;
ContentHashValue HashResourceList(const ResourceList& resources) noexcept;
ContentHashValue HashVisibilityList(const VisibilityList& visibility) noexcept;
ContentHashValue HashFootprintList(const FootprintList& footprints) noexcept;
ContentHashValue HashSamplerPairList(const SamplerPairList& pairs) noexcept;
void AppendFootprintHash(StreamingHash& hasher, const ResourceFootprint& footprint) noexcept;

struct TableStatistics
{
    std::string_view HashName;
    bool DedupeEnabled{ true };
    InternerStatistics Interning;
};

/** @brief The stage 6 builder: interns each variant as it arrives. `FreezeModuleTables` consumes it. */
struct InternedModule
{
    std::string Name;
    const PermutationSpace* Space{ nullptr };
    uint64_t SpaceSize{ 0u };
    std::vector<LibraryEntryPoint> EntryPoints;
    std::vector<VariantKey> VariantKeys;
    std::vector<LibraryVariant> Variants;
    ContentInterner<std::vector<std::byte>> SourceInterner{ &HashSourceCode, k_HashName };
    ContentInterner<ReflectedBinding> ResourceInterner{ &HashReflectedBinding, k_HashName };
    ContentInterner<ResourceList> ResourceListInterner{ &HashResourceList, k_HashName };
    ContentInterner<FootprintList> FootprintListInterner{ &HashFootprintList, k_HashName };
    ContentInterner<VisibilityList> VisibilityInterner{ &HashVisibilityList, k_HashName };
    ContentInterner<ReflectedRasterState> RasterInterner{ &HashReflectedRasterState, k_HashName };
    ContentInterner<SamplerPairList> SamplerPairInterner{ &HashSamplerPairList, k_HashName };
    ContentInterner<EntryPointConstants> ConstantsInterner{ &HashEntryPointConstants, k_HashName };
    /** Parallel to the source table. The code decides the facts, so the first variant with a source gives them. */
    std::vector<SourceCodeFacts> SourceFacts;
    /** Sorted and unique. */
    std::vector<std::string> ObservedCapabilities;
    /** Sorted by scope and name. The first declaring variant adds an entry: the attribute text never varies. */
    std::vector<FootprintAxisDeclaration> FootprintDeclarations;

    void DisableDedupe() noexcept
    {
        SourceInterner.Disable();
        ResourceInterner.Disable();
        ResourceListInterner.Disable();
        FootprintListInterner.Disable();
        VisibilityInterner.Disable();
        RasterInterner.Disable();
        SamplerPairInterner.Disable();
        ConstantsInterner.Disable();
    }
};

/** @brief The frozen tables of one module, with statistics for each, since they collapse at very different rates. */
struct CookedModule
{
    std::string Name;
    const PermutationSpace* Space{ nullptr };
    /** @brief Holes included. */
    uint64_t SpaceSize{ 0u };
    std::vector<LibraryEntryPoint> EntryPoints;
    std::vector<std::vector<std::byte>> Sources;
    std::vector<ReflectedBinding> Resources;
    std::vector<ResourceList> ResourceLists;
    std::vector<FootprintList> FootprintLists;
    std::vector<VisibilityList> VisibilityLists;
    std::vector<ReflectedRasterState> RasterStates;
    std::vector<SamplerPairList> SamplerPairLists;
    std::vector<EntryPointConstants> EntryPointConstantsTable;
    std::vector<VariantKey> VariantKeys;
    std::vector<LibraryVariant> Variants;
    /** Parallel to `Sources`. */
    std::vector<SourceCodeFacts> SourceFacts;
    /** The manifest's capability name table: sorted and unique. */
    std::vector<std::string> ObservedCapabilities;
    std::vector<FootprintAxisDeclaration> FootprintDeclarations;

    TableStatistics SourceTable;
    TableStatistics ResourceTable;
    TableStatistics ResourceListTable;
    TableStatistics FootprintListTable;
    TableStatistics VisibilityTable;
    TableStatistics RasterTable;
    TableStatistics SamplerPairTable;
    TableStatistics ConstantsTable;
};

struct CookedProfile
{
    std::string TargetName;
    PlacementForms Forms{};
    ShaderCodeFormat CodeFormat{ ShaderCodeFormat::None };
    /** Whether the pairs were read, not whether any exist. */
    bool SamplerTexturePairsRecorded{ false };
    /** Whether the capability names and workgroup memory were read */
    bool CodeFactsRead{ false };
    /** An optimizer ran on the emitted code, so its facts can need less than an unoptimized profile's */
    bool CodeOptimized{ false };
};

/** @brief The whole cook. `Environments` is the profile-major grid, and an empty entry was not cooked. */
struct CookedLibrary
{
    std::vector<std::string> ModuleNames;
    std::vector<CookedProfile> Profiles;
    std::vector<std::optional<CookedModule>> Environments;
};

void DisableDedupe(InternedModule& module) noexcept;

CookError AppendVariantToModule(InternedModule& module,
                                const CompiledVariant& variant,
                                const VariantDescriptor& descriptor,
                                DiagnosticSink& sink);

/** @brief Compares the pairs of each variant that two profiles cooked. Skips profiles that did not record pairs, and
 * entry points with combined texture samplers. */
CookError VerifySamplerPairsAcrossProfiles(const CookedLibrary& library, DiagnosticSink& sink);

/** @brief Compares the workgroup memory and the portable capabilities (16-bit floats, subgroups) of each entry point
 * that two profiles cooked for one variant. Skips profiles that did not read their facts. */
CookError VerifySourceFactsAcrossProfiles(const CookedLibrary& library, DiagnosticSink& sink);

/** @brief Consumes the builder, so nothing after the freeze can reach an interner. */
CookedModule FreezeModuleTables(InternedModule&& interned);

/** @brief Returns the code of one entry point of one variant. The round trip compares it with the compiler output. */
std::span<const std::byte> ResolveSource(const CookedModule& module,
                                         const LibraryVariant& variant,
                                         size_t entry_point_index) noexcept;

CookResult<ShaderLayout> ResolveLayout(const CookedModule& module,
                                       const LibraryVariant& variant,
                                       size_t entry_point_index);

CookResult<ShaderLayoutView> ResolveLayoutView(const CookedModule& module,
                                               const LibraryVariant& variant,
                                               size_t entry_point_index);

} // namespace lodestone

#endif // !LODESTONE_COOKED_LIBRARY_HPP
