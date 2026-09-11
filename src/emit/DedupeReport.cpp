#include "emit/DedupeReport.hpp"
#include "model/ContentHash.hpp"
#include "model/ContentInterner.hpp"
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationRegistry.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PolicyDocument.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <iterator>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    struct SourceHashTable
    {
        SourceHashTable(size_t num_sources, size_t num_entry_points);
        void Add(ContentHashValue hash) noexcept;
        [[nodiscard]] ContentHashValue At(size_t variant_index, size_t entry_point_index) const noexcept;
    private:
        std::vector<ContentHashValue> sourceStrHashes;
        size_t numEntryPoints;
    };

     /**@brief `SourceIndex` gives the unique index of a source string, and `MappedCount` tells how many
       * times something mapped to that index instead of duplicating into another source blob string */
    struct SourceCollapse
    {
        uint32_t SourceIndex{ 0u };
        uint32_t MappedCount{ 0u };
        std::string_view FirstDescription;
    };

    SourceHashTable BuildSourceHashTable(const CookedModule& module);
    bool AssignmentComparatorExcludingAxis(const PermutationAssignment& lhs,
                                           const PermutationAssignment& rhs,
                                           size_t excluded_axis_index) noexcept;
    std::vector<uint32_t> OrderByOtherAxes(const CookedModule& module, const size_t axis_index);
    /**@brief Checks if all variants in the given group share the same source for the specified entry point. */
    bool GroupSharesOneSource(const CookedModule& module,
                              const SourceHashTable& hashes,
                              std::span<const uint32_t> group,
                              size_t entry_point);
    /**@brief Printed marker specifying what influence an axis had (if any) in the influence table we print. */
    char InfluenceMarker(AxisInfluence influence) noexcept;
    std::string EmitInfluenceTable(const CookedModule& module, const ModuleInfluence& influence);
    /** One pass over the variants. The earlier form searched the whole variant list again for each
     * distinct source, which is quadratic and gives the same answer. */
    std::vector<SourceCollapse> CollectSourceCollapses(const CookedModule& module, size_t entry_point_index);
    std::string EmitProvenance(const CookedModule& module);

} // namespace

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

ModuleInfluence ComputeActualInfluence(const CookedModule& module)
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

    const SourceHashTable hashes = BuildSourceHashTable(module);

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
                    !GroupSharesOneSource(module, hashes, group, entryPointIndex))
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

CookError EnforceModulePolicy(const CookedModule& module,
                              const ModulePolicyEntry& policy,
                              DiagnosticSink& diagnostics) noexcept
{
    if (policy.InertAxesForEntryPoints.empty())
    {
        return CookError::Success;
    }

    const StringMap<std::vector<std::string>>& expectedInfluence = policy.InertAxesForEntryPoints;
    // pull out the keys: we only want to iterate over the entry points we actually have policy data on
    auto inertEntryPoints = expectedInfluence |
                            std::views::keys |
                            std::views::transform([](const auto& key) { return std::string_view{ key }; }) |
                            std::ranges::to<std::vector<std::string_view>>();
    std::ranges::sort(inertEntryPoints);

    // preconstruct a map of axis names to axis indices for quick lookup
    std::unordered_map<std::string_view, std::ptrdiff_t> axisNameToIndex;
    for (auto&& [idx, axis] : std::views::enumerate(module.Space->Axes()))
    {
        axisNameToIndex[axis.Name] = idx;
    }
    
    // get the influence information for every entry point in the module
    // todo-ship: simplify ComputeActualInfluence's work by using inertEntrryPoints as a filter there as well
    ModuleInfluence actualInfluence = ComputeActualInfluence(module);
    auto relevantEntryPoints = actualInfluence.EntryPoints |
                               std::views::filter([&](const EntryPointInfluence& enpt)
                               {
                                   return std::ranges::binary_search(inertEntryPoints, std::string_view{ enpt.EntryPointName });
                               });
    
    for (const EntryPointInfluence& entryPointInfluence : relevantEntryPoints)
    {
        const std::vector<std::string>& expectedInertAxes = expectedInfluence.at(entryPointInfluence.EntryPointName);
        // extract axes that should be inert but aren't
        auto filterFailingAxis = [&axisNameToIndex, &entryPointInfluence](const std::string_view& axis_name)
        {
            const std::ptrdiff_t axisIndex = axisNameToIndex.at(axis_name);
            return entryPointInfluence.Axes[static_cast<size_t>(axisIndex)] != AxisInfluence::Inert;
        };

        auto failingAxes = expectedInertAxes |
                           std::views::filter(filterFailingAxis);

        if (!std::ranges::empty(failingAxes))
        {
            CookError runningErr = CookError::Invalid;
            for (std::string_view failingAxis : failingAxes)
            {
                const std::string errorStr = std::format("Entry point '{}' has axis '{}' expected to be inert but is not.",
                                                         entryPointInfluence.EntryPointName,
                                                         failingAxis);
                runningErr = ReportError(diagnostics, CookError::PolicyInertAxisNotInertWhenCooked, errorStr);
            }
            return runningErr;
        }
    }

    return CookError::Success;
}

std::string GenerateDedupeReport(const CookedLibrary& library)
{
    std::string report;
    report.reserve(1u << 13);

    report += "Shader cooker dedup report\n";
    report += "Generated by tools/shader_cooker. Do not edit by hand.\n\n";
    report += "Note: The dedupe ratio indicates how many artifacts were seen for each unique entry. A higher "
              "ratio means more effective deduplication.\n\n";

    for (const CookedModule& module : library.Modules)
    {
        const InternerStatistics& sourceStatistics = module.SourceTable.Interning;

        report += std::format("{}  {} variants x {} entrypoints = {} artifacts\n\n",
                              module.Name,
                              module.Variants.size(),
                              module.EntryPoints.size(),
                              sourceStatistics.ArtifactsSeen);

        report += EmitProvenance(module);
        report += "\n";

        // One line for each table. Placement, footprint, and visibility collapse at different rates,
        // and one number for all three would hide which one grows.
        const std::array<std::pair<std::string_view, const TableStatistics*>, 5u> tables{
            std::pair{ std::string_view{ "sources" }, &module.SourceTable },
            std::pair{ std::string_view{ "resources" }, &module.ResourceTable },
            std::pair{ std::string_view{ "resource lists" }, &module.ResourceListTable },
            std::pair{ std::string_view{ "footprints" }, &module.FootprintListTable },
            std::pair{ std::string_view{ "visibility" }, &module.VisibilityTable }
        };

        uint32_t collisions = 0u;
        uint32_t comparisons = 0u;

        for (const auto& [name, table] : tables)
        {
            const float dedupeRatio = static_cast<float>(table->Interning.ArtifactsSeen) /
                                      static_cast<float>(table->Interning.UniqueEntries);
            report += std::format("  {}: Artifacts seen: {} -> Unique Entries: {} (Dedupe Ratio: {:.2f}:1)\n",
                                  name,
                                  table->Interning.ArtifactsSeen,
                                  table->Interning.UniqueEntries,
                                  dedupeRatio);
            collisions += table->Interning.HashCollisions;
            comparisons += table->Interning.ByteComparisons;
        }

        report += std::format("  dedup enabled: {}\n", module.SourceTable.DedupeEnabled ? "yes" : "no");
        report += std::format("  hash function: {}\n", module.SourceTable.HashName);
        report += std::format("  hash collisions resolved by byte compare: {}\n", collisions);
        report += std::format("  byte comparisons forced by a hash hit: {}\n", comparisons);
        report += "  normalization passes active: (none)\n\n";

        const ModuleInfluence influence = ComputeActualInfluence(module);
        report += EmitInfluenceTable(module, influence);
        report += "\n";
    }

    return report;
}

namespace
{
    SourceHashTable::SourceHashTable(size_t num_sources, size_t num_entry_points) : numEntryPoints(num_entry_points)
    {
        // its semantically easier to still emplace_back new entries,
        // rather than pre-allocating and indexing into the vector.
        // but reserving here exactly should make that cost the same anyways
        sourceStrHashes.reserve(num_sources * num_entry_points);
    }

    void SourceHashTable::Add(ContentHashValue hash) noexcept
    {
        sourceStrHashes.emplace_back(hash);
    }

    ContentHashValue SourceHashTable::At(size_t variant_index, size_t entry_point_index) const noexcept
    {
        // indices are strided by numEntryPoints
        return sourceStrHashes[(variant_index * numEntryPoints) + entry_point_index];
    }

    SourceHashTable BuildSourceHashTable(const CookedModule& module)
    {
        SourceHashTable table(module.Variants.size(), module.EntryPoints.size());
        for (const LibraryVariant& variant : module.Variants)
        {
            for (size_t i = 0; i < module.EntryPoints.size(); ++i)
            {
                const std::string_view sourceStr = ResolveSource(module, variant, i);
                const ContentHashValue hash =
                    HashBytes(std::as_bytes(std::span{ sourceStr.data(), sourceStr.size() }));
                table.Add(hash);
            }
        }
        return table;
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

    bool GroupSharesOneSource(const CookedModule& module,
                              const SourceHashTable& hashes,
                              std::span<const uint32_t> group,
                              size_t entry_point)
    {
        const ContentHashValue firstHash = hashes.At(group.front(), entry_point);
        for (size_t i = 1; i < group.size(); ++i)
        {
            if (hashes.At(group[i], entry_point) != firstHash)
            {
                return false;
            }
        }
        // as with the rest of our library: equal hashes don't prove anything. now we will fallback
        // to actual string comparisons. with xxhash3 though, our chance of a collision is miniscule.
        // like something on the order of 1 in 2^128 for xxhash3.
        // (again, we shouldn't hit this, and this is for a statistical tool, but it's still important to be
        // thorough)
        const std::string_view text = ResolveSource(module, module.Variants[group.front()], entry_point);
        return std::ranges::all_of(
            group.subspan(1u),
            [&module, &entry_point, &text](uint32_t variant_index)
            {
                return ResolveSource(module, module.Variants[variant_index], entry_point) == text;
            });
    }

    char InfluenceMarker(AxisInfluence influence) noexcept
    {
        switch (influence)
        {
        case AxisInfluence::Active:
            return 'x';
        case AxisInfluence::Inert:
            return '.';
        case AxisInfluence::Invalid:
            [[fallthrough]];
        case AxisInfluence::Undetermined:
            [[fallthrough]];
        default:
            return '?';
        }
    }

    std::string EmitInfluenceTable(const CookedModule& module, const ModuleInfluence& influence)
    {
        std::string table = "  axis influence (x = changes output, . = inert, ? = undetermined)\n\n";

        size_t nameWidth = 16u;
        for (const EntryPointInfluence& entry : influence.EntryPoints)
        {
            nameWidth = std::max(nameWidth, entry.EntryPointName.size() + 2u);
        }

        table += std::format("  {:<{}}", "", nameWidth);
        for (const PermutationAxis& axis : module.Space->Axes())
        {
            table += std::format("{:<24}", axis.Name);
        }
        table += "\n";

        for (const EntryPointInfluence& entry : influence.EntryPoints)
        {
            table += std::format("  {:<{}}", entry.EntryPointName, nameWidth);
            for (const AxisInfluence value : entry.Axes)
            {
                table += std::format("{:<24}", std::string(1u, InfluenceMarker(value)));
            }
            table += "\n";
        }

        return table;
    }

    std::vector<SourceCollapse> CollectSourceCollapses(const CookedModule& module, size_t entry_point_index)
    {
        std::vector<SourceCollapse> collapses;

        for (const LibraryVariant& variant : module.Variants)
        {
            const uint32_t sourceIndex = variant.SourceIndices[entry_point_index];

            const auto found = std::ranges::find(collapses, sourceIndex, &SourceCollapse::SourceIndex);

            if (found != collapses.end())
            {
                ++found->MappedCount;
                continue;
            }

            collapses.emplace_back(sourceIndex, 1u, variant.Description);
        }

        return collapses;
    }

    std::string EmitProvenance(const CookedModule& module)
    {
        std::string emitted;

        for (size_t entryPointIndex = 0u; entryPointIndex < module.EntryPoints.size(); ++entryPointIndex)
        {
            const std::string& name = module.EntryPoints[entryPointIndex].Name;
            const std::vector<SourceCollapse> collapses = CollectSourceCollapses(module, entryPointIndex);
            const size_t artifactCount = module.Variants.size();

            emitted += std::format("  {:<20} {} variants -> {} unique sources{}\n",
                                   name,
                                   artifactCount,
                                   collapses.size(),
                                   collapses.size() == artifactCount ? "   (no collapse)" : "");

            for (const SourceCollapse& collapse : collapses)
            {
                if (collapse.MappedCount > 1u)
                {
                    emitted += std::format("      source #{} <- {} assignments, first [{}]\n",
                                           collapse.SourceIndex,
                                           collapse.MappedCount,
                                           collapse.FirstDescription);
                }
            }
        }

        return emitted;
    }

}

} // namespace lodestone
