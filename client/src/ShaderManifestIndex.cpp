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

#ifdef __clang__
#pragma clang diagnostic push
// suppress these bc I'm doing them all quite intentionally here
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif

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
    QueryAxisValue newValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = value ? 1u : 0u, .Name = {} };
    return whereAnyOf(axis_name, { newValue });

}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name, uint32_t value) const noexcept
{
    QueryAxisValue newValue{ .Type = AxisValueDomain::Integral, .IntegralValue = value, .Name = {} };
    return whereAnyOf(axis_name, { newValue });
}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name,
                                                 AxisValueDomain domain,
                                                 std::string_view type_or_enum_name) const noexcept
{
    QueryAxisValue newValue{ .Type = domain, .IntegralValue=0u, .Name = type_or_enum_name };
    return whereAnyOf(axis_name, { newValue });
}

ManifestQueryBuilder ManifestQueryBuilder::WhereAnyOfBoolean(std::string_view axis_name) const
{
    std::vector<QueryAxisValue> values
    {
        QueryAxisValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = 0u, .Name = axis_name },
        QueryAxisValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = 1u, .Name = axis_name }
    };
    return whereAnyOf(axis_name, std::move(values));
}

ManifestQueryBuilder ManifestQueryBuilder::WhereAnyOf(std::string_view axis_name, std::span<const uint32_t> values) const
{
    auto buildAxisValue = [](uint32_t value) -> QueryAxisValue
    {
        return QueryAxisValue{ .Type = AxisValueDomain::Integral, .IntegralValue = value, .Name = {} };
    };
    std::vector<QueryAxisValue> queryValues = values |
                                              std::views::transform(buildAxisValue) |
                                              std::ranges::to<std::vector>();
    return whereAnyOf(axis_name, std::move(queryValues));
}

ManifestQueryBuilder ManifestQueryBuilder::WhereAnyOf(std::string_view axis_name,
                                                      AxisValueDomain domain,
                                                      std::span<const std::string_view> values) const
{
    auto buildAxisValue = [domain](std::string_view value) -> QueryAxisValue
    {
        return QueryAxisValue{ .Type = domain, .IntegralValue = 0u, .Name = value };
    };
    std::vector<QueryAxisValue> queryValues = values |
                                              std::views::transform(buildAxisValue) |
                                              std::ranges::to<std::vector>();
    return whereAnyOf(axis_name, std::move(queryValues));
}

QueryResult<std::vector<VariantKey>> ManifestQueryBuilder::Keys() const noexcept
{
    if (!errors.empty())
    {
        // return the last error code, since that's the most recent one
        return std::unexpected(errors.back().Code);
    }

    // now validate the constraints
}

ManifestQueryBuilder ManifestQueryBuilder::whereAnyOf(std::string_view axis_name, std::vector<QueryAxisValue> values) const noexcept
{
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
    // we can just check front(), as we always make sure the vector is contiguous types
    if (values.front().Type != axis.Domain)
    {
        result.errors.emplace_back(QueryErrorCode::IncorrectValueDomain,
                                   axis_name,
                                   static_cast<uint32_t>(axis.Domain));
        return result;
    }

    // Confirm the axis actually declares the requested value. A Boolean axis always holds both
    // 0 and 1, so only Type and Integral/Enum axes need the lookup.
    if (axis.Domain == AxisValueDomain::Type || axis.Domain == AxisValueDomain::Enum)
    {
        // Type + Enum axis values are indices into the string table; resolve them and compare by name.
        auto extractStrView = [&](const AxisValueType string_index) -> std::string_view
        {
            return index->manifest.String(string_index);
        };
        const std::vector<std::string_view> axisValueNames = index->manifest.AxisValues(axisIndex) |
                                                             std::views::transform(extractStrView) |
                                                             std::ranges::to<std::vector>();
        // create just a view, not vector, to avoid allocating a new container
        auto queryValueNames = values | std::views::transform(&QueryAxisValue::Name);
        for (const auto& queryValueName : queryValueNames)
        {
            if (std::ranges::find(axisValueNames, queryValueName) == axisValueNames.end())
            {
                result.errors.emplace_back(QueryErrorCode::ValueNotInAxis,
                                           axis_name,
                                           0u,
                                           NearestName(queryValueName, axisValueNames));
                return result;
            }
        }
    }
    else if (axis.Domain == AxisValueDomain::Integral)
    {
        std::span<const AxisValueType> axisValues = index->manifest.AxisValues(axisIndex);
        // same as above, extract a view of all values
        auto queryIntegralValues = values | std::views::transform(&QueryAxisValue::IntegralValue);
        // make sure all values are in the axis values. both containers are sorted, use set_intersection logic
        for (const auto& queryValue : queryIntegralValues)
        {
            if (std::ranges::find(axisValues, queryValue) == axisValues.end())
            {
                result.errors.emplace_back(QueryErrorCode::ValueNotInAxis,
                                           axis_name,
                                           queryValue); // empty suggestion 
                return result;
            }
        }
    }

    // see if axis_name is already in the constraints
    auto constraintIter = std::ranges::lower_bound(result.constraints,
                                                   axisIndex,
                                                   std::less<uint32_t>{},
                                                   &QueryAxisRange::AxisIndex);
    // only quirk for lower_bound: it will give location where AxisIndex *could* be inserted
    if (constraintIter != result.constraints.end() &&
        constraintIter->AxisIndex == axisIndex)
    {
        // add value to existing
        QueryAxisRange& constraint = *constraintIter;
        // if we wanted to validate against duplicate values, this would be where to do it
        // for now, I'm not doing it as it's a real mess and I'm not really sure it warrants an error
        constraint.Values.append_range(values);
        return result;
    }
    else
    {
        // insert a new constraint at the position indicated by lower_bound, which keeps it sorted
        result.constraints.insert(constraintIter,
            QueryAxisRange{ axis_name, std::move(values), axisIndex });
        return result;
    }
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
        if (axis.Domain == AxisValueDomain::Type || axis.Domain == AxisValueDomain::Enum)
        {
            valueIndices = stringValueIndices(axisIndex, range);
        }
        else
        {
            valueIndices = integralValueIndices(axisIndex, range);
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
    const AxisValueType currValue = manifest.AxisValue(axis_index, value_index);
    QueryAxisValue result{};
    result.Type = axis.Domain;
    switch (axis.Domain)
    {
    case AxisValueDomain::Boolean:
        [[fallthrough]];
    case AxisValueDomain::Integral:
        result.IntegralValue = currValue;
        break;
    case AxisValueDomain::Enum:
        // enums handled like interface strings, it's the case name
        [[fallthrough]];
    case AxisValueDomain::Type:
        // read type name from string table
        result.Name = manifest.String(currValue);
        break;
    case AxisValueDomain::None:
        std::unreachable();
    }
    return result;
}

std::vector<uint32_t> ManifestIndex::integralValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const
{
    std::vector<uint32_t> constraintValueIndices(range.Values.size());
    std::span<const AxisValueType> axisValues = manifest.AxisValues(axis_index);
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
    auto extractStrView = [&](const AxisValueType value) -> std::string_view
    {
        return manifest.String(value);
    };
    const std::vector<std::string_view> constraintStrings = manifest.AxisValues(axis_index) |
                                                            std::views::transform(extractStrView) |
                                                            std::ranges::to<std::vector>();
    // get iterators that match from axis.Values to constraintStrings, and use std::distance to convert to indices
    for (const auto&& [index, val] : std::views::enumerate(range.Values))
    {
        auto iter = std::ranges::find(constraintStrings, val.Name);
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
