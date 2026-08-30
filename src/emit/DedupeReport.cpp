#include "emit/DedupeReport.hpp"
#include "model/ContentHash.hpp"
#include "model/ContentInterner.hpp"
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationPolicy.hpp"
#include "permute/PermutationRegistry.hpp"
#include "permute/PermutationSpace.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <numeric>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    struct SourceHashTable
    {

        SourceHashTable(size_t num_sources, size_t num_entry_points)
            : numEntryPoints(num_entry_points)
        {
            // its semantically easier to still emplace_back new entries,
            // rather than pre-allocating and indexing into the vector.
            // but reserving here exactly should make that cost the same anyways
            sourceStrHashes.reserve(num_sources * num_entry_points);
        }

        void Add(ContentHashValue hash) noexcept
        {
            sourceStrHashes.emplace_back(hash);
        }

        [[nodiscard]] ContentHashValue At(size_t variant_index, size_t entry_point_index) const noexcept
        {
            // indices are strided by numEntryPoints
            return sourceStrHashes[(variant_index * numEntryPoints) + entry_point_index];
        }

    private:
        std::vector<ContentHashValue> sourceStrHashes;
        size_t numEntryPoints;
    };

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

    // for each group of variants, check if they all share the same source for the given entry point.
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

    /** The single character the influence table prints for one axis. The heading above the table
     * states what each one means, so the two must stay together. */
    char InfluenceMarker(AxisInfluence influence) noexcept
    {
        switch (influence)
        {
        case AxisInfluence::Active:
            return 'x';
        case AxisInfluence::Inert:
            return '.';
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

    /** How many variants mapped onto one interned source, and which one arrived first.
     *
     * The order of first arrival decides the report text, so this keeps the sources in that order
     * rather than in index order. */
    struct SourceCollapse
    {
        uint32_t SourceIndex{ 0u };
        uint32_t MappedCount{ 0u };
        std::string_view FirstDescription;
    };

    /** One pass over the variants. The earlier form searched the whole variant list again for each
     * distinct source, which is quadratic and gives the same answer. */
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

ModuleInfluence ComputeAxisInfluence(const CookedModule& module)
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

// I'm keeping this around in case we want to use it, but this isn't really a useful
// query in 99.9% of cases: it was an interesting special case for our first ever
// test content since we could use this to verify collapsing worked as expected.
bool AllVariantsShareOneLayout(const CookedModule& module)
{
    bool seenOne = false;
    ShaderLayoutView first;

    for (const LibraryVariant& variant : module.Variants)
    {
        for (size_t i = 0u; i < variant.VisibilityIndices.size(); ++i)
        {
            ShaderLayoutView layout = ResolveLayoutView(module, variant, i);

            if (!seenOne)
            {
                first = layout;
                seenOne = true;
            }
            else if (layout != first)
            {
                return false;
            }
        }
    }

    return true;
}

namespace
{

    const EntryPointInfluence* FindEntryPointInfluence(const ModuleInfluence& influence,
                                                       std::string_view entry_point_name) noexcept
    {
        for (const EntryPointInfluence& candidate : influence.EntryPoints)
        {
            if (candidate.EntryPointName == entry_point_name)
            {
                return &candidate;
            }
        }

        return nullptr;
    }

    /** Position of an axis in the space. The influence vector runs parallel to it. */
    std::optional<size_t> FindAxisIndex(const PermutationSpace& space, std::string_view axis_name) noexcept
    {
        for (size_t i = 0u; i < space.AxisCount(); ++i)
        {
            if (space.Axes()[i].Name == axis_name)
            {
                return i;
            }
        }

        return std::nullopt;
    }

    /** One declared expectation, checked against what the cook measured. Returns the violation count,
     * which is zero or one. */
    uint32_t CheckExpectedInfluence(const CookedModule& module,
                                    const ModuleInfluence& influence,
                                    const ExpectedAxisInfluence& expected)
    {
        const EntryPointInfluence* entry = FindEntryPointInfluence(influence, expected.EntryPointName);
        if (entry == nullptr)
        {
            std::println(stderr,
                         "[shader_cooker] module {} declares an expectation for entrypoint '{}', which "
                         "does not exist",
                         module.Name,
                         expected.EntryPointName);
            return 1u;
        }

        const std::optional<size_t> axisIndex = FindAxisIndex(*module.Space, expected.AxisName);
        if (!axisIndex.has_value() || axisIndex.value() >= entry->Axes.size())
        {
            std::println(stderr,
                         "[shader_cooker] module {} declares an expectation for axis '{}', which is not "
                         "in its permutation space",
                         module.Name,
                         expected.AxisName);
            return 1u;
        }

        const AxisInfluence measured = entry->Axes[axisIndex.value()];
        const bool measuredInert = measured == AxisInfluence::Inert;

        if (measured != AxisInfluence::Undetermined && measuredInert != expected.IsInert)
        {
            std::println(stderr,
                         "[shader_cooker] INFLUENCE CHANGED: axis '{}' is {} for {}, but the module "
                         "declares it {}. The permutation space now costs something different.",
                         expected.AxisName,
                         ToString(measured),
                         expected.EntryPointName,
                         expected.IsInert ? "Inert" : "Active");
            return 1u;
        }

        return 0u;
    }

    uint32_t CheckVariantBudget(const CookedModule& module, const ModulePolicy& policy)
    {
        if (policy.MaxVariants == 0u || module.Variants.size() <= policy.MaxVariants)
        {
            return 0u;
        }

        std::println(stderr,
                     "[shader_cooker] module {} expands to {} variants, over its budget of {}. Raise "
                     "the budget on purpose, or take an axis out.",
                     module.Name,
                     module.Variants.size(),
                     policy.MaxVariants);
        return 1u;
    }

} // namespace

CookError EnforceModulePolicy(const CookedModule& module, const ModuleInfluence& influence)
{
    const ModulePolicy* policy = FindPolicyForModule(module.Name);
    if (policy == nullptr)
    {
        return CookError::Success;
    }

    uint32_t violations = CheckVariantBudget(module, *policy);

    for (const ExpectedAxisInfluence& expected : policy->ExpectedInfluence)
    {
        violations += CheckExpectedInfluence(module, influence, expected);
    }

    if (violations != 0u)
    {
        return CookError::ModulePolicyViolated;
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

        const ModuleInfluence influence = ComputeAxisInfluence(module);
        report += EmitInfluenceTable(module, influence);
        report += "\n";
    }

    return report;
}
// ttb: 764.5ms
} // namespace lodestone
