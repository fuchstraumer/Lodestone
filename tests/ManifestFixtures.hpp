#pragma once
#ifndef LODESTONE_TESTS_MANIFEST_FIXTURES_HPP
#define LODESTONE_TESTS_MANIFEST_FIXTURES_HPP
#include "ShaderLibraryTypes.hpp"
#include "model/CookedLibrary.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"

#include <cstdint>
#include <span>
#include <utility>

/** Builders for the `CookedModule` values that tests write by hand and pass to the emitter. A `LibraryVariant`
 * keeps several vectors with one entry for each entry point, and each new field of that kind used to break every
 * hand-built fixture at once (first the canonical assignment, then the sampler pair index). These
 * builders set every such field, so a new field changes this file and no fixture. */
namespace lodestone::tests
{

/** One entry point of a hand-built variant: the index into each per-entry-point table of the module. */
struct FixtureInstance
{
    uint32_t Source;
    uint32_t Visibility;
    uint32_t Raster;
    uint32_t SamplerPairs;
    uint32_t Constants;
    WorkgroupSize Workgroup;
};

/** Returns an instance that reads source `source` and visibility list `visibility`, with raster state 0, sampler
 * pair list 0, entry point constants 0, and a 64x1x1 workgroup. Most fixtures vary only those two indices. */
inline FixtureInstance ComputeInstance(uint32_t source, uint32_t visibility)
{
    return FixtureInstance{ .Source = source,
                            .Visibility = visibility,
                            .Raster = 0u,
                            .SamplerPairs = 0u,
                            .Constants = 0u,
                            .Workgroup = WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u } };
}

/** Returns a variant of `space` with the assignment `active`, its canonical assignment, and one entry in every
 * per-entry-point vector for each of `instances`. The resource list and the footprint list are 0; a test that
 * needs others sets them on the result. */
inline LibraryVariant MakeFixtureVariant(const PermutationSpace& space,
                                         uint64_t index,
                                         PermutationAssignment active,
                                         std::span<const FixtureInstance> instances)
{
    LibraryVariant variant;
    variant.Index = index;
    variant.Active = std::move(active);
    variant.Canonical = space.CanonicalizeAssignment(variant.Active);
    variant.ResourceListIndex = 0u;
    variant.FootprintListIndex = 0u;
    for (const FixtureInstance& instance : instances)
    {
        variant.SourceIndices.push_back(instance.Source);
        variant.VisibilityIndices.push_back(instance.Visibility);
        variant.RasterIndices.push_back(instance.Raster);
        variant.SamplerPairIndices.push_back(instance.SamplerPairs);
        variant.ConstantsIndices.push_back(instance.Constants);
        variant.Workgroups.push_back(instance.Workgroup);
    }
    return variant;
}

/** Adds the entry 0 that `ComputeInstance` refers to in each instance table that fixtures do not fill themselves:
 * an empty raster state, an empty sampler pair list, and entry point constants with no delivery. */
inline void AddDefaultInstanceTables(CookedModule& module)
{
    module.RasterStates.emplace_back();
    module.SamplerPairLists.emplace_back();
    module.EntryPointConstantsTable.emplace_back();
}

} // namespace lodestone::tests

#endif // !LODESTONE_TESTS_MANIFEST_FIXTURES_HPP
