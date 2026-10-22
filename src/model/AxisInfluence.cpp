#include "model/AxisInfluence.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "model/ContentHash.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <numeric>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
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

        // the pairs follow from the code and the layout, and a consumer reads them, so they are part of the instance
        hasher.Append(HashSamplerPairList(module.SamplerPairLists[variant.SamplerPairIndices[entry_point_index]]));
        hasher.Append(
            HashEntryPointConstants(module.EntryPointConstantsTable[variant.ConstantsIndices[entry_point_index]]));

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
        
        const bool workgroupsEqual = lhsWorkgroup.X == rhsWorkgroup.X &&
                                     lhsWorkgroup.Y == rhsWorkgroup.Y &&
                                     lhsWorkgroup.Z == rhsWorkgroup.Z;

        const bool rasterStatesEqual = module.RasterStates[lhs.RasterIndices[entry_point_index]] ==
                                       module.RasterStates[rhs.RasterIndices[entry_point_index]];
        const bool layoutsEqual = instances.Layouts[instances.Slot(lhs_variant, entry_point_index)] ==
                                  instances.Layouts[instances.Slot(rhs_variant, entry_point_index)];
        const bool sourcesEqual = std::ranges::equal(ResolveSource(module, lhs, entry_point_index),
                                                     ResolveSource(module, rhs, entry_point_index));
        const bool pairsEqual = module.SamplerPairLists[lhs.SamplerPairIndices[entry_point_index]] ==
                                module.SamplerPairLists[rhs.SamplerPairIndices[entry_point_index]];
        const bool constantsEqual = module.EntryPointConstantsTable[lhs.ConstantsIndices[entry_point_index]] ==
                                    module.EntryPointConstantsTable[rhs.ConstantsIndices[entry_point_index]];

        return workgroupsEqual &&
               rasterStatesEqual &&
               layoutsEqual &&
               sourcesEqual &&
               pairsEqual &&
               constantsEqual;
    }

    bool GroupSharesOneInstance(const CookedModule& module,
                                const InstanceTable& instances,
                                std::span<const uint32_t> group,
                                size_t entry_point_index)
    {
        // If the hash of the first variant matches all others, we still need to check the actual content.
        // But if we find a mismatch, we can immediately return false.
        const ContentHashValue firstHash = instances.Hashes[instances.Slot(group.front(), entry_point_index)];
        auto hashCheck = [&instances, &entry_point_index, &firstHash](uint32_t variant_index)
        {
            return instances.Hashes[instances.Slot(variant_index, entry_point_index)] == firstHash;
        };

        const bool hashesAgree = std::ranges::all_of(group.subspan(1u), hashCheck);

        if (!hashesAgree)
        {
            return false;
        }

        // equal hashes prove nothing: the content decides. check the actual content, including layouts, states,
        // and workgroup sizes - all things that can be varied by permutation values
        auto predicate = [&](uint32_t variant_index)
        {
            return InstancesEqual(module, instances, group.front(), variant_index, entry_point_index);
        };

        return std::ranges::all_of(group.subspan(1u), predicate);
    }

    bool AssignmentComparatorExcludingAxis(const PermutationAssignment& lhs,
                                           const PermutationAssignment& rhs,
                                           size_t excluded_axis_index) noexcept
    {
        for (std::ptrdiff_t k = 0; k < std::ssize(lhs); ++k)
        {
            if (std::cmp_not_equal(k , excluded_axis_index) &&
                lhs[k].Value != rhs[k].Value)
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

    /** Closes the relevance of one subject. The per-axis pass compares only variants that differ in one axis, so
     * it proves that variants which agree on every relevant axis hold one value only along chains of cooked
     * variants, and a sparse cook can break a chain. This groups the variants by their relevant axes, and in each
     * group that holds more than one value it marks every axis that varies within the group, until no group holds
     * more than one value.
     *
     * Every axis that varies within such a group separates two variants with different values, so the rule marks
     * a set and picks no pair, which keeps the result independent of the variant order. A smaller set can exist,
     * but it is not unique. Each round marks at least one axis, because two variants with different values that
     * agree on every relevant axis differ in some other axis. */
    template<typename SharesOneValue>
    void CloseRelevance(const CookedModule& module,
                        const SharesOneValue& shares_one_value,
                        size_t subject_index,
                        std::vector<AxisInfluence>& axes)
    {
        const size_t axisCount = axes.size();
        std::vector<uint32_t> order(module.Variants.size());
        std::vector<uint8_t> marked(axisCount, uint8_t{ 0u });
        std::vector<std::ptrdiff_t> active_indices;
        active_indices.reserve(axisCount);
        std::vector<std::ptrdiff_t> inactive_indices;
        inactive_indices.reserve(axisCount);

        while (true)
        {
            // reset marked per iteration
            std::ranges::fill(marked, 0u);
            active_indices.clear(); // just reset using clear(), can't fill
            inactive_indices.clear(); // just reset using clear(), can't fill

            // pack the actually active (relevant) axes into active_indices
            // means one less branch in the hot lessOnRelevant comparison
            for (std::ptrdiff_t k = 0u; k < std::ssize(axes); ++k)
            {
                if (IsAxisRelevant(axes[k]))
                {
                    active_indices.push_back(k);
                }
                else
                {
                    inactive_indices.push_back(k);
                }
            }

            auto lessOnRelevant = [&](uint32_t lhs, uint32_t rhs)
            {
                const CanonicalAssignment& lhsValues = module.Variants[lhs].Canonical;
                const CanonicalAssignment& rhsValues = module.Variants[rhs].Canonical;
                for (std::ptrdiff_t k : active_indices)
                {
                    if (lhsValues[k].Value != rhsValues[k].Value)
                    {
                        return lhsValues[k].Value < rhsValues[k].Value;
                    }
                }
                return false;
            };

            std::ranges::iota(order, 0u);
            std::ranges::stable_sort(order, lessOnRelevant);

            size_t begin = 0u;
            while (begin < order.size())
            {
                size_t end = begin + 1u;

                while (end < order.size() && !lessOnRelevant(order[begin], order[end]))
                {
                    ++end;
                }

                const std::span<const uint32_t> group{ order.data() + begin, end - begin };
                begin = end;

                if (group.size() < 2u || shares_one_value(group, subject_index))
                {
                    continue;
                }

                const CanonicalAssignment& first = module.Variants[group.front()].Canonical;
                // inverted loop: load each variant once, then check all of it's axes for differences
                // should hopefully be more cache-friendly than the previous approach
                for (uint32_t variant_index : group.subspan(1u))
                {
                    const CanonicalAssignment& current = module.Variants[variant_index].Canonical;
                    for (std::ptrdiff_t k : inactive_indices)
                    {
                        if (marked[k] == 0u &&
                            (current[k].Value != first[k].Value))
                        {
                            marked[k] = 1u;
                        }
                    }
                }
            }

            if (std::ranges::none_of(marked, [](uint8_t bit) { return bit != 0u; }))
            {
                return;
            }

            for (std::ptrdiff_t k = 0u; k < std::ssize(axes); ++k)
            {
                if (marked[k] != 0u)
                {
                    axes[k] = AxisInfluence::Active;
                }
            }
        }
    }

    /** The first pass over each of `subject_count` subjects. A subject is one thing that a variant gives a value
     * for, such as one entry point instance or one resource footprint. `shares_one_value(group, subject)` returns true
     * when every variant of `group` gives one value for the subject, and it decides by content. Returns one axis list
     * for each subject: Active where a one-axis pair differs, and Undetermined for an axis with no such pair. */
    template<typename SharesOneValue>
    std::vector<std::vector<AxisInfluence>> MeasureAxisPairs(const CookedModule& module,
                                                             size_t subject_count,
                                                             const SharesOneValue& shares_one_value)
    {
        const size_t axisCount = module.Space->AxisCount();
        std::vector<std::vector<AxisInfluence>> subjects(subject_count,
                                                         std::vector<AxisInfluence>(axisCount, AxisInfluence::Inert));

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

                for (size_t subjectIndex = 0u; subjectIndex < subject_count; ++subjectIndex)
                {
                    if (subjects[subjectIndex][k] != AxisInfluence::Active &&
                        !shares_one_value(group, subjectIndex))
                    {
                        subjects[subjectIndex][k] = AxisInfluence::Active;
                    }
                }
            }

            if (!foundPair)
            {
                for (std::vector<AxisInfluence>& axes : subjects)
                {
                    axes[k] = AxisInfluence::Undetermined;
                }
            }
        }

        return subjects;
    }

    /** The resources of one module by scope and name, and the footprint each variant gives each of them. */
    struct FootprintTable
    {
        /** Sorted by scope and then by name, with no repeats. */
        std::vector<std::pair<std::string_view, std::string_view>> Identities;
        /** Strided by the identity count. Null where the variant does not declare the resource. */
        std::vector<const ResourceFootprint*> Footprints;

        [[nodiscard]] const ResourceFootprint* At(size_t variant_index, size_t identity_index) const noexcept
        {
            return Footprints[(variant_index * Identities.size()) + identity_index];
        }
    };

    FootprintTable BuildFootprintTable(const CookedModule& module)
    {
        FootprintTable table;
        table.Identities.reserve(module.Resources.size());
        for (const ReflectedBinding& resource : module.Resources)
        {
            table.Identities.emplace_back(resource.ScopeName, resource.Name);
        }
        std::ranges::sort(table.Identities);
        const auto [uniqueEnd, rangeEnd] = std::ranges::unique(table.Identities);
        table.Identities.erase(uniqueEnd, rangeEnd);

        // the identity of each resource record, found once, so the variant walk below searches nothing
        std::vector<uint32_t> identityOfResource;
        identityOfResource.reserve(module.Resources.size());
        for (const ReflectedBinding& resource : module.Resources)
        {
            const auto found = std::ranges::lower_bound(table.Identities,
                                                        std::pair<std::string_view, std::string_view>{
                                                            resource.ScopeName, resource.Name });
            identityOfResource.push_back(static_cast<uint32_t>(found - table.Identities.begin()));
        }

        table.Footprints.assign(module.Variants.size() * table.Identities.size(), nullptr);
        for (auto&& [variantIndex, variant] : std::views::enumerate(module.Variants))
        {
            const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
            const FootprintList& footprints = module.FootprintLists[variant.FootprintListIndex];
            for (size_t position = 0u; position < resources.size(); ++position)
            {
                const size_t slot = (static_cast<size_t>(variantIndex) * table.Identities.size()) +
                                    identityOfResource[resources[position]];
                table.Footprints[slot] = &footprints[position];
            }
        }

        return table;
    }

    /** Returns true when every variant of `group` that declares the resource gives it one footprint. */
    bool GroupSharesOneFootprint(const FootprintTable& table, std::span<const uint32_t> group, size_t identity_index)
    {
        const ResourceFootprint* first = nullptr;
        for (const uint32_t variantIndex : group)
        {
            const ResourceFootprint* footprint = table.At(variantIndex, identity_index);
            if (footprint == nullptr)
            {
                continue;
            }

            if (first == nullptr)
            {
                first = footprint;
            }
            else if (!(*footprint == *first))
            {
                return false;
            }

        }

        return true;
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
    // every pass reads one canonical value for each axis, and only CanonicalizeAssignment builds a canonical
    // assignment, so a variant with a shorter one was built by hand and skipped it
    auto checkAxisCount = [&axisCount](const LibraryVariant& variant)
    {
        return variant.Canonical.size() == axisCount;
    };
    assert(std::ranges::all_of(module.Variants, checkAxisCount));

    const CookResult<InstanceTable> instances = BuildInstanceTable(module);
    if (!instances)
    {
        return std::unexpected(instances.error());
    }

    auto sharesOneInstance = [&](std::span<const uint32_t> group, size_t entry_point_index)
    {
        return GroupSharesOneInstance(module, instances.value(), group, entry_point_index);
    };
    std::vector<std::vector<AxisInfluence>> measured =
        MeasureAxisPairs(module, module.EntryPoints.size(), sharesOneInstance);

    for (size_t entryPointIndex = 0u; entryPointIndex < measured.size(); ++entryPointIndex)
    {
        CloseRelevance(module, sharesOneInstance, entryPointIndex, measured[entryPointIndex]);
    }

    influence.EntryPoints.reserve(module.EntryPoints.size());
    for (auto&& [entryPoint, axes] : std::views::zip(module.EntryPoints, measured))
    {
        influence.EntryPoints.emplace_back(entryPoint.Name, std::move(axes));
    }

    return influence;
}

std::vector<ResourceFootprintInfluence> ComputeFootprintInfluence(const CookedModule& module)
{
    const FootprintTable table = BuildFootprintTable(module);
    std::vector<ResourceFootprintInfluence> influence;
    influence.reserve(table.Identities.size());
    for (const auto& [scopeName, name] : table.Identities)
    {
        influence.push_back(ResourceFootprintInfluence{ .ScopeName = std::string{ scopeName },
                                                        .Name = std::string{ name },
                                                        .Axes = {} });
    }

    if (module.Space == nullptr)
    {
        return influence;
    }

    auto sharesOneFootprint = [&table](std::span<const uint32_t> group, size_t identity_index)
    {
        return GroupSharesOneFootprint(table, group, identity_index);
    };
    std::vector<std::vector<AxisInfluence>> measured = MeasureAxisPairs(module, table.Identities.size(), sharesOneFootprint);
    for (auto&& [identityIndex, resource] : std::views::enumerate(influence))
    {
        resource.PairAxes = std::move(measured[static_cast<size_t>(identityIndex)]);
        // the closing pass treats an Undetermined axis as relevant, which suits a pin and not a footprint: here it
        // must mark the axes that a sparse cook left with no one-axis pair
        resource.Axes = resource.PairAxes;
        std::ranges::replace(resource.Axes, AxisInfluence::Undetermined, AxisInfluence::Inert);
        CloseRelevance(module, sharesOneFootprint, static_cast<size_t>(identityIndex), resource.Axes);
    }

    return influence;
}

std::vector<uint64_t> BuildFootprintAxisMasks(const CookedModule& module, size_t word_count)
{
    std::vector<uint64_t> masks(module.Resources.size() * word_count, 0u);
    if (word_count == 0u)
    {
        return masks;
    }

    const std::vector<ResourceFootprintInfluence> influence = ComputeFootprintInfluence(module);
    for (auto&& [resourceIndex, resource] : std::views::enumerate(module.Resources))
    {
        // every record has an identity in the result, because the result is built from these records
        const auto measured = std::ranges::lower_bound(influence,
                                                       std::tie(resource.ScopeName, resource.Name),
                                                       std::less{},
                                                       [](const ResourceFootprintInfluence& entry)
                                                       { return std::tie(entry.ScopeName, entry.Name); });
        for (auto&& [axisIndex, axisInfluence] : std::views::enumerate(measured->Axes))
        {
            if (axisInfluence == AxisInfluence::Active)
            {
                const size_t wordIndex = (static_cast<size_t>(resourceIndex) * word_count) +
                                         (static_cast<size_t>(axisIndex) / 64u);
                masks[wordIndex] |= uint64_t{ 1u } << (static_cast<size_t>(axisIndex) % 64u);
            }
        }
    }

    return masks;
}

CookError VerifyFootprintAxes(const CookedModule& module, DiagnosticSink& sink)
{
    if (module.Space == nullptr)
    {
        return CookError::Success;
    }

    const std::span<const PermutationAxis> axes = module.Space->Axes();
    const std::vector<ResourceFootprintInfluence> measured = ComputeFootprintInfluence(module);
    CookError result = CookError::Success;
    for (const ResourceFootprintInfluence& resource : measured)
    {
        // both lists are sorted by scope and then by name, and a resource with no size attribute has no declaration
        const auto declaration = std::ranges::lower_bound(module.FootprintDeclarations,
                                                          std::tie(resource.ScopeName, resource.Name),
                                                          std::less{},
                                                          [](const FootprintAxisDeclaration& entry)
                                                          { return std::tie(entry.ScopeName, entry.Name); });
        const bool declared = declaration != module.FootprintDeclarations.end() &&
                              declaration->ScopeName == resource.ScopeName && declaration->Name == resource.Name;

        // only a one-axis pair proves that one axis changes the footprint. The closed set can hold an axis that a
        // sparse cook could not separate from the one that does
        for (auto&& [axisIndex, axisInfluence] : std::views::enumerate(resource.PairAxes))
        {

            if (axisInfluence != AxisInfluence::Active)
            {
                continue;
            }
            
            const bool named = declared && std::ranges::binary_search(declaration->Axes, static_cast<uint32_t>(axisIndex));
            if (!named)
            {
                result = ReportError(sink,
                                     CookError::FootprintAxisUndeclared,
                                     std::format("module {}: axis '{}' changes the footprint of '{}', and no size "
                                                 "attribute of that resource names it",
                                                 module.Name,
                                                 axes[static_cast<size_t>(axisIndex)].Name,
                                                 resource.ScopeName.empty()
                                                     ? resource.Name
                                                     : std::format("{}.{}", resource.ScopeName, resource.Name)));
            }
        }
    }

    return result;
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
