#include "model/AxisInfluence.hpp"
#include "model/ContentHash.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <numeric>
#include <ranges>
#include <span>
#include <vector>

namespace lodestone
{

namespace
{

    /** The content of every (variant, entry point) instance of one module, resolved once. `Layouts` points into
     * the module's tables, so the module must outlive it. Both vectors are strided by the entry point count. */
    struct InstanceTable
    {
        size_t EntryPointCount{ 0u };
        std::vector<ContentHashValue> Hashes;
        std::vector<ShaderLayoutView> Layouts;

        [[nodiscard]] size_t Slot(size_t variant_index, size_t entry_point_index) const noexcept
        {
            return (variant_index * EntryPointCount) + entry_point_index;
        }
    };

    ContentHashValue HashInstance(const CookedModule& module,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index,
                                  const ShaderLayoutView& layout)
    {
        thread_local StreamingHash hasher;
        hasher.Reset();

        hasher.Append(ResolveSource(module, variant, entry_point_index));

        hasher.Append(static_cast<uint64_t>(layout.size()));
        for (const ResolvedBindingView& binding : layout)
        {
            hasher.Append(HashReflectedBinding(*binding.Resource));
            // a resource with no footprint and a resource with an empty one are different facts
            hasher.Append(static_cast<uint32_t>(binding.Footprint != nullptr));
            if (binding.Footprint != nullptr)
            {
                AppendFootprintHash(hasher, *binding.Footprint);
            }
        }

        hasher.Append(HashReflectedRasterState(module.RasterStates[variant.RasterIndices[entry_point_index]]));

        const WorkgroupSize& workgroup = variant.Workgroups[entry_point_index];
        hasher.Append(workgroup.X);
        hasher.Append(workgroup.Y);
        hasher.Append(workgroup.Z);

        return hasher.Finalize();
    }

    CookResult<InstanceTable> BuildInstanceTable(const CookedModule& module)
    {
        InstanceTable table;
        table.EntryPointCount = module.EntryPoints.size();
        table.Hashes.reserve(module.Variants.size() * table.EntryPointCount);
        table.Layouts.reserve(module.Variants.size() * table.EntryPointCount);

        for (const LibraryVariant& variant : module.Variants)
        {
            for (size_t entryPointIndex = 0u; entryPointIndex < table.EntryPointCount; ++entryPointIndex)
            {
                CookResult<ShaderLayoutView> layout = ResolveLayoutView(module, variant, entryPointIndex);
                if (!layout)
                {
                    return std::unexpected(layout.error());
                }

                table.Hashes.push_back(HashInstance(module, variant, entryPointIndex, layout.value()));
                table.Layouts.push_back(std::move(layout.value()));
            }
        }

        return table;
    }

    /** Compares the content of two instances of one entry point. A hash only picks the candidates, so this
     * decides. */
    bool InstancesEqual(const CookedModule& module,
                        const InstanceTable& instances,
                        uint32_t lhs_variant,
                        uint32_t rhs_variant,
                        size_t entry_point_index)
    {
        const LibraryVariant& lhs = module.Variants[lhs_variant];
        const LibraryVariant& rhs = module.Variants[rhs_variant];

        const WorkgroupSize& lhsWorkgroup = lhs.Workgroups[entry_point_index];
        const WorkgroupSize& rhsWorkgroup = rhs.Workgroups[entry_point_index];
        const bool workgroupsEqual = lhsWorkgroup.X == rhsWorkgroup.X && lhsWorkgroup.Y == rhsWorkgroup.Y &&
                                     lhsWorkgroup.Z == rhsWorkgroup.Z;

        return workgroupsEqual &&
               module.RasterStates[lhs.RasterIndices[entry_point_index]] ==
                   module.RasterStates[rhs.RasterIndices[entry_point_index]] &&
               instances.Layouts[instances.Slot(lhs_variant, entry_point_index)] ==
                   instances.Layouts[instances.Slot(rhs_variant, entry_point_index)] &&
               std::ranges::equal(ResolveSource(module, lhs, entry_point_index),
                                  ResolveSource(module, rhs, entry_point_index));
    }

    bool GroupSharesOneInstance(const CookedModule& module,
                                const InstanceTable& instances,
                                std::span<const uint32_t> group,
                                size_t entry_point_index)
    {
        const ContentHashValue firstHash = instances.Hashes[instances.Slot(group.front(), entry_point_index)];
        const bool hashesAgree = std::ranges::all_of(group.subspan(1u), [&](uint32_t variant_index) {
            return instances.Hashes[instances.Slot(variant_index, entry_point_index)] == firstHash;
        });
        if (!hashesAgree)
        {
            return false;
        }

        // equal hashes prove nothing: the content decides
        return std::ranges::all_of(group.subspan(1u), [&](uint32_t variant_index) {
            return InstancesEqual(module, instances, group.front(), variant_index, entry_point_index);
        });
    }

    bool AssignmentComparatorExcludingAxis(const PermutationAssignment& lhs,
                                           const PermutationAssignment& rhs,
                                           size_t excluded_axis_index) noexcept
    {
        for (size_t k = 0; k < lhs.size(); ++k)
        {
            if (k != excluded_axis_index && lhs[k].Value != rhs[k].Value)
            {
                return lhs[k].Value < rhs[k].Value;
            }
        }
        return false;
    }

    /** Orders the variants so that the ones that agree on every axis except `axis_index` are adjacent. */
    std::vector<uint32_t> OrderByOtherAxes(const CookedModule& module, const size_t axis_index)
    {
        std::vector<uint32_t> order(module.Variants.size());
        std::ranges::iota(order, 0u);
        std::ranges::stable_sort(order,
                                 [&](uint32_t lhs, uint32_t rhs)
                                 {
                                     return AssignmentComparatorExcludingAxis(module.Variants[lhs].Canonical,
                                                                              module.Variants[rhs].Canonical,
                                                                              axis_index);
                                 });
        return order;
    }

} // namespace

CookResult<ModuleInfluence> ComputeActualInfluence(const CookedModule& module)
{
    ModuleInfluence influence;
    influence.ModuleName = module.Name;

    if (module.Space == nullptr)
    {
        return influence;
    }

    const size_t axisCount = module.Space->AxisCount();
    const size_t entryPointCount = module.EntryPoints.size();

    influence.EntryPoints.reserve(entryPointCount);
    for (const LibraryEntryPoint& entryPoint : module.EntryPoints)
    {
        influence.EntryPoints.emplace_back(entryPoint.Name,
                                           std::vector<AxisInfluence>(axisCount, AxisInfluence::Inert));
    }

    const CookResult<InstanceTable> instances = BuildInstanceTable(module);
    if (!instances)
    {
        return std::unexpected(instances.error());
    }

    for (size_t k = 0u; k < axisCount; ++k)
    {
        const std::vector<uint32_t> orderedIndices = OrderByOtherAxes(module, k);
        bool foundPair = false;
        // A while, not a for: `begin` already moves to the next group, and a for header would then
        // increment past that group's first element and drop the group.
        size_t begin = 0u;
        while (begin < orderedIndices.size())
        {
            const PermutationAssignment& beginCanonical = module.Variants[orderedIndices[begin]].Canonical;

            // The order puts variants that agree on every other axis next to each other, so a group
            // ends at the first one that disagrees. Read `end` inside the condition: on the last
            // group it is out of range until the range test runs.
            size_t end = begin + 1u;
            while (end < orderedIndices.size() &&
                   !AssignmentComparatorExcludingAxis(beginCanonical,
                                                      module.Variants[orderedIndices[end]].Canonical,
                                                      k))
            {
                ++end;
            }

            const std::span<const uint32_t> group{ orderedIndices.data() + begin, end - begin };
            begin = end;

            if (group.size() < 2u)
            {
                continue;
            }

            foundPair = true;

            for (size_t entryPointIndex = 0u; entryPointIndex < entryPointCount; ++entryPointIndex)
            {
                if (influence.EntryPoints[entryPointIndex].Axes[k] != AxisInfluence::Active &&
                    !GroupSharesOneInstance(module, instances.value(), group, entryPointIndex))
                {
                    influence.EntryPoints[entryPointIndex].Axes[k] = AxisInfluence::Active;
                }
            }
        }

        if (!foundPair)
        {
            for (EntryPointInfluence& epInfluence : influence.EntryPoints)
            {
                epInfluence.Axes[k] = AxisInfluence::Undetermined;
            }
        }
    }

    return influence;
}

bool IsAxisRelevant(AxisInfluence influence) noexcept
{
    return influence == AxisInfluence::Active || influence == AxisInfluence::Undetermined;
}

std::string_view ToString(AxisInfluence influence) noexcept
{
    switch (influence)
    {
    case AxisInfluence::Inert:
        return "Inert";
    case AxisInfluence::Active:
        return "Active";
    case AxisInfluence::Undetermined:
        return "Undetermined";
    case AxisInfluence::Invalid:
        return "Invalid";
    }

    return "Invalid";
}

} // namespace lodestone
