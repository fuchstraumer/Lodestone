#include "ShaderManifestIndex.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "Suggest.hpp"
#include "VariantKey.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <iterator>
#include <numeric>
#include <cstdint>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{
    // The nearest accepted name to a mistyped one, or empty when nothing is close enough. The distance
    // budget follows clang and rust: one edit per three characters, and at least one.
    std::string_view NearestName(std::string_view input,
                                 std::span<const std::string_view> candidates) noexcept
    {
        const int32_t maxDistance = std::max<int32_t>(1, static_cast<int32_t>(input.size() / 3u));
        const std::vector<Suggestion> suggestions = FindSuggestions(input, candidates, maxDistance);
        return suggestions.empty() ? std::string_view{} : suggestions.front().Text;
    }
}

ManifestQueryBuilder::ManifestQueryBuilder(const class ManifestIndex& _index) noexcept : index(&_index)
{

}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name, bool value) const noexcept
{
    // A boolean axis is an integral axis with values {0, 1}, so store the boolean as its 0/1 integral
    // and let it share the integral resolution and decode paths.
    QueryAxisValue newValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = value ? 1u : 0u, .TypeName = {} };
    return where(axis_name, newValue);

}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name, uint32_t value) const noexcept
{
    QueryAxisValue newValue{ .Type = AxisValueDomain::Integral, .IntegralValue = value, .TypeName = {} };
    return where(axis_name, newValue);
}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name, std::string_view type_name) const noexcept
{
    QueryAxisValue newValue{ .Type = AxisValueDomain::Type, .IntegralValue=0u, .TypeName = type_name };
    return where(axis_name, newValue);
}

ManifestQueryBuilder ManifestQueryBuilder::where(std::string_view axis_name, QueryAxisValue value) const noexcept
{
    // We copy as we descend, because otherwise modifications to the query would affect the original object.
    // This won't work for cases where we set high-level root constraints, then branch into ones we want
    // to actually bake
    ManifestQueryBuilder result{ *this };
    // verify axis_name is valid, push an error if not
    auto indexIter = index->axisNameToIndex.find(axis_name);
    if (indexIter == index->axisNameToIndex.end())
    {
        const std::vector<std::string_view> axisNames =
            index->axisNameToIndex | std::views::keys | std::ranges::to<std::vector>();
        result.errors.emplace_back(QueryErrorCode::UnknownAxis,
                                   axis_name,
                                   0u,
                                   NearestName(axis_name, axisNames));
        return result;
    }

    const uint32_t axisIndex = indexIter->second;
    const ManifestAxis& axis = index->manifest.Axis(axisIndex);

    // The value's domain must match the axis. An Integral value also satisfies an Enum axis,
    // since enum values are stored as plain integers.
    const bool domainMatches = (value.Type == axis.Domain) ||
                               (value.Type == AxisValueDomain::Integral &&
                                axis.Domain == AxisValueDomain::Enum);
    if (!domainMatches)
    {
        result.errors.emplace_back(QueryErrorCode::IncorrectValueDomain,
                                   axis_name,
                                   static_cast<uint32_t>(axis.Domain));
        return result;
    }

    // Confirm the axis actually declares the requested value. A Boolean axis always holds both
    // 0 and 1, so only Type and Integral/Enum axes need the lookup.
    if (axis.Domain == AxisValueDomain::Type)
    {
        // Type axis values are indices into the string table; resolve them and compare by name.
        auto extractStrView = [&](const int64_t string_index) -> std::string_view
        {
            return index->manifest.String(static_cast<uint32_t>(string_index));
        };
        const std::vector<std::string_view> axisTypeNames = index->manifest.AxisValues(axisIndex) |
                                                            std::views::transform(extractStrView) |
                                                            std::ranges::to<std::vector>();
        if (std::ranges::find(axisTypeNames, value.TypeName) == axisTypeNames.end())
        {
            result.errors.emplace_back(QueryErrorCode::ValueNotInAxis,
                                       axis_name,
                                       0u,
                                       NearestName(value.TypeName, axisTypeNames));
            return result;
        }
    }
    else if (axis.Domain != AxisValueDomain::Boolean)
    {
        std::span<const int64_t> axisValues = index->manifest.AxisValues(axisIndex);
        if (std::ranges::find(axisValues, value.IntegralValue) == axisValues.end())
        {
            result.errors.emplace_back(QueryErrorCode::ValueNotInAxis, axis_name, value.IntegralValue);
            return result;
        }
    }

    // see if axis_name is already in the constraints
    auto constraintIter = std::ranges::find(result.constraints,
                                            axis_name,
                                            &QueryAxisRange::AxisName);
    if (constraintIter != result.constraints.end())
    {
        // add value to existing
        QueryAxisRange& constraint = *constraintIter;
        // if we wanted to validate against duplicate values, this would be where to do it
        // for now, I'm not doing it as it's a real mess and I'm not really sure it warrants an error
        constraint.Values.emplace_back(value);
        return result;
    }

    // create a whole new constraint
    result.constraints.emplace_back(QueryAxisRange{ .AxisName = axis_name, .Values = { value } });
    return result;
}

ManifestIndex::ManifestIndex(ShaderManifestView view) : manifest(view)
{
    std::span<const ManifestAxis> axes = manifest.Axes();
    radices.resize(axes.size());
    placeValues.resize(axes.size());
    axisNameToIndex.reserve(axes.size());

    for (const auto&& [index, axis] : std::views::enumerate(axes))
    {
        radices[index] = axis.ValueCount;
        std::string_view axisName = manifest.String(axis.NameString);
        axisNameToIndex[axisName] = static_cast<uint32_t>(index);
    }

    // use a backwards exclusive scan to fill the placeValues array with less gross indexing logic
    std::exclusive_scan(radices.rbegin(),
                        radices.rend(),
                        placeValues.rbegin(),
                        static_cast<uint64_t>(1),
                        std::multiplies<uint64_t>{});
    
}

const ShaderManifestView& ManifestIndex::View() const noexcept
{
    return manifest;
}

std::vector<QueryAxisValue> ManifestIndex::Decode(VariantKey key) const
{
    // scratch holds the indices for each value in the manifest, sized to the 
    // count of axes (so, radices.size())
    std::vector<uint32_t> scratch(radices.size());
    UnpackVariantKey(key, radices, scratch);

    std::vector<QueryAxisValue> result(radices.size());

    for (const auto&& [axisIndex, valueIndex] : std::views::enumerate(scratch))
    {
        result[axisIndex] = decodeAxis(static_cast<uint32_t>(axisIndex), valueIndex);
    }

    return result;
}

std::vector<DecodedVariant> ManifestIndex::Enumerate() const
{
    std::span<const VariantKey> variantKeys = manifest.VariantKeys();
    std::vector<DecodedVariant> result(variantKeys.size());
    // we effectively copy decode, but I'm doing it manually here to hoist
    // out the scratch buffer and avoid reallocating that for each call to Decode()

    auto decodeAxisFn = [this](const auto& pair) -> QueryAxisValue
    {
        const auto& [axisIndex, valueIndex] = pair;
        return decodeAxis(static_cast<uint32_t>(axisIndex), valueIndex);
    };
    
    std::vector<uint32_t> scratch(radices.size());
    for (const auto [index, key] : std::views::enumerate(variantKeys))
    {
        result[index].Key = key;
        UnpackVariantKey(key, radices, scratch);
        result[index].Values = scratch |
                               std::views::enumerate |
                               std::views::transform(decodeAxisFn) |
                               std::ranges::to<std::vector<QueryAxisValue>>();
    }

    return result;
}

ManifestQueryBuilder ManifestIndex::Query() const noexcept
{
    return ManifestQueryBuilder{ *this };
}

std::vector<VariantKey> ManifestIndex::Select(std::span<const QueryAxisRange> constraints) const
{
    // first need to construct the scan constraints from the provided axis assignment ranges
    std::vector<ScanConstraint> scanConstraints;
    scanConstraints.reserve(constraints.size());
    for (const auto& range : constraints)
    {
        const auto axisIter = axisNameToIndex.find(range.AxisName);
        if (axisIter == axisNameToIndex.end())
        {
            // if axis name not found, return empty: builder does erroring path, Select
            // doesn't handle errors at all.
            return {};
        }
        const uint32_t axisIndex = axisIter->second;
        const ManifestAxis& axis = manifest.Axis(axisIndex);
        // map input constraint values (given as actual concrete values) to the indices
        // of that value in axisValues space
        std::vector<uint32_t> valueIndices;
        if (axis.Domain != AxisValueDomain::Type)
        {
            valueIndices = integralValueIndices(axisIndex, range);
        }
        else
        {
            valueIndices = stringValueIndices(axisIndex, range);
        }

        // there's no error handling here, the QueryBuilder is the one that gives you that
        if (valueIndices.empty())
        {
            return {};
        }

        scanConstraints.emplace_back(axisIndex, std::move(valueIndices));
    }

    // sorting scanConstraints makes matching from constraints to axes a little more efficient
    // less important than the keys being sorted, and the subspan construction that happens later
    // range much not contain any duplicate axes, as this would violate the uniqueness assumption in the scan logic
    std::ranges::sort(scanConstraints, std::less<uint32_t>{}, &ScanConstraint::AxisIndex);
    return scan(scanConstraints);
}

QueryAxisValue ManifestIndex::decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept
{
    const ManifestAxis& axis = manifest.Axis(axis_index);
    const int64_t currValue = manifest.AxisValue(axis_index, value_index);
    QueryAxisValue result{};
    result.Type = axis.Domain;
    switch (axis.Domain)
    {
    case AxisValueDomain::Boolean:
        [[fallthrough]];
    case AxisValueDomain::Integral:
        [[fallthrough]];
    case AxisValueDomain::Enum:
        result.IntegralValue = static_cast<uint32_t>(currValue);
        break;
    case AxisValueDomain::Type:
        // read type name from string table
        result.TypeName = manifest.String(static_cast<uint32_t>(currValue));
        break;
    case AxisValueDomain::None:
        std::unreachable();
    }
    return result;
}

std::vector<uint32_t> ManifestIndex::integralValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const
{
    std::vector<uint32_t> constraintValueIndices(range.Values.size());
    std::span<const int64_t> axisValues = manifest.AxisValues(axis_index);
    // flatten input constraint values (actual values) into the index of that value in the
    // axisValues array (i.e, get the digit in the radix of this axis)
    for (const auto&& [index, val] : std::views::enumerate(range.Values))
    {
        auto iter = std::ranges::find(axisValues, val.IntegralValue);
        if (iter == axisValues.end())
        {
            return {};
        }
        const uint32_t valueIndex = static_cast<uint32_t>(std::distance(axisValues.begin(), iter));
        constraintValueIndices[index] = valueIndex;
    }
    return constraintValueIndices;
}

std::vector<uint32_t> ManifestIndex::stringValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const
{
    std::vector<uint32_t> constraintValueIndices(range.Values.size());
    // axisValues now gives indices into the Strings() table: extract the strings,
    // and do a lexicographical comparison to back that out into an index. position
    // of the matching string in this local table gives the index in axisValues
    auto extractStrView = [&](const int64_t value) -> std::string_view
    {
        return manifest.String(static_cast<uint32_t>(value));
    };
    const std::vector<std::string_view> constraintStrings = manifest.AxisValues(axis_index) |
                                                            std::views::transform(extractStrView) |
                                                            std::ranges::to<std::vector>();
    // get iterators that match from axis.Values to constraintStrings, and use std::distance to convert to indices
    for (const auto&& [index, val] : std::views::enumerate(range.Values))
    {
        auto iter = std::ranges::find(constraintStrings, val.TypeName);
        if (iter == constraintStrings.end())
        {
            return {};
        }
        const uint32_t strIndex = static_cast<uint32_t>(std::distance(constraintStrings.begin(), iter));
        constraintValueIndices[index] = strIndex;
    }
    
    return constraintValueIndices;
}

std::vector<VariantKey> ManifestIndex::scan(std::span<const ScanConstraint> constraints) const
{
    // pre-narrow the above span down to [minKey, maxKey] band that any match must fit in.
    // this subspan still contains many non-matches, but it reduces the number of candidates
    // in a robust and repeatable manner that scales well as constraints grow
    // so for each axis, we use the mixed-radix math to evaluate the minimum and maximum
    // value for that axis, and add it to the min and max key. for unconstrainted axes,
    // the range is just [0, radix - 1] for that axis.
    uint64_t minKey = 0u;
    uint64_t maxKey = 0u;
    uint32_t constraintCursor = 0u;
    for (int64_t axisIndex = 0; std::cmp_less(axisIndex, radices.size()); ++axisIndex)
    {
        uint32_t minDigit = 0u;
        uint32_t maxDigit = radices[axisIndex] - 1;
        // simpler logic now that we have sorted constraints... just walk it alongside the axis walk,
        // no searching with lower_bound needed
        if (std::cmp_less(constraintCursor, constraints.size()) &&
            std::cmp_equal(constraints[constraintCursor].AxisIndex , axisIndex))
        {
            const auto [minValue, maxValue] =
                std::ranges::minmax(constraints[constraintCursor].AllowedValueIndices);
            minDigit = minValue;
            maxDigit = maxValue;
            ++constraintCursor;
        }

        minKey += static_cast<uint64_t>(minDigit) * placeValues[axisIndex];
        maxKey += static_cast<uint64_t>(maxDigit) * placeValues[axisIndex];
    }

    // now use lower_bound and upper_bound (since keys are already sorted) to create the span
    // we actually filter on (as a subspan of the original)
    std::span<const VariantKey> candidates = manifest.VariantKeys();
    const auto first = std::ranges::lower_bound(candidates, static_cast<VariantKey>(minKey));
    const auto last = std::ranges::upper_bound(candidates, static_cast<VariantKey>(maxKey));
    // upper_bound returns end() whenever the largest match is the last key, and first == last is a
    // valid empty result, so the only real invariant is that the band is not inverted.
    assert(first <= last);
    candidates = std::span<const VariantKey>(first, last);

    // now that we have our narrowed band of candidates, we can filter them according to the constraints
    std::vector<VariantKey> result;

    for (const VariantKey key : candidates)
    {
        const uint64_t keyValue = std::to_underlying(key);
        
        auto matchFn = [&](const ScanConstraint& constraint) -> bool
        {
            // for each constraint, extract the digit (at the current radix/axis)
            // (aka, just it's value in that mixed radix space)
            const uint32_t digit =
                static_cast<uint32_t>((keyValue / placeValues[constraint.AxisIndex]) % radices[constraint.AxisIndex]);
            // now check if digit is in range of the current constraint
            return std::ranges::contains(constraint.AllowedValueIndices, digit);
        };

        if (std::ranges::all_of(constraints, matchFn))
        {
            result.emplace_back(key);
        }
    }

    return result;
}

}
