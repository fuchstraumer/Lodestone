#include "ShaderManifestIndex.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "VariantKey.hpp"
#include <algorithm>
#include <cassert>
#include <functional>
#include <iterator>
#include <numeric>
#include <cstdint>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

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
                        1,
                        std::multiplies<uint64_t>{});
    
}

const ShaderManifestView& ManifestIndex::View() const noexcept
{
    return manifest;
}

std::vector<ManifestAxisValue> ManifestIndex::Decode(VariantKey key) const
{
    // scratch holds the indices for each value in the manifest, sized to the 
    // count of axes (so, radices.size())
    std::vector<uint32_t> scratch(radices.size());
    UnpackVariantKey(key, radices, scratch);

    std::vector<ManifestAxisValue> result(radices.size());

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

    auto decodeAxisFn = [this](const auto& pair) -> ManifestAxisValue
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
                               std::ranges::to<std::vector<ManifestAxisValue>>();
    }

    return result;
}

ManifestQueryBuilder ManifestIndex::Query() const noexcept
{
    return ManifestQueryBuilder{ *this };
}

std::vector<VariantKey> ManifestIndex::Select(std::span<const ManifestAxisAssignmentRange> constraints) const
{
    // first need to construct the scan constraints from the provided axis assignment ranges
    std::vector<ScanConstraint> scanConstraints;
    scanConstraints.reserve(constraints.size());
    for (const auto& range : constraints)
    {
        const uint32_t axisIndex = axisNameToIndex.at(range.AxisName);
        const ManifestAxis& axis = manifest.Axis(axisIndex);
        // don't like my syntax here? think this is ugly? then you hate women
        // (each function maps concrete values to their corresponding indices in the axis values array)
        // (this just constructs the result vector right in ScanConstraint succinctly thats all)
        scanConstraints.emplace_back(axisIndex,
                                     axis.Domain != AxisValueDomain::Type ? integralValueIndices(axisIndex, range) :
                                                                            stringValueIndices(axisIndex, range));
    }
    // sorting scanConstraints makes matching from constraints to axes a little more efficient
    // less important than the keys being sorted, and the subspan construction that happens later
    // range much not contain any duplicate axes, as this would violate the uniqueness assumption in the scan logic
    std::ranges::sort(scanConstraints, std::less<uint32_t>{}, &ScanConstraint::AxisIndex);
    return scan(scanConstraints);
}

ManifestAxisValue ManifestIndex::decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept
{
    const ManifestAxis& axis = manifest.Axis(axis_index);
    const int64_t currValue = manifest.AxisValue(axis_index, value_index);
    ManifestAxisValue result{};
    result.Type = axis.Domain;
    switch (axis.Domain)
    {
    case AxisValueDomain::Boolean:
        result.BoolValue = static_cast<bool>(currValue);
        break;
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

std::vector<uint32_t> ManifestIndex::integralValueIndices(const uint32_t axis_index, const ManifestAxisAssignmentRange& range) const
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
            // TODO TODO TODO: our error handling state
            // leaving this stubbed for now as just an exception
            throw std::runtime_error("Constraint value not found in axis values.");
        }
        const uint32_t valueIndex = static_cast<uint32_t>(std::distance(axisValues.begin(), iter));
        constraintValueIndices[index] = valueIndex;
    }
    return constraintValueIndices;
}

std::vector<uint32_t> ManifestIndex::stringValueIndices(const uint32_t axis_index, const ManifestAxisAssignmentRange& range) const
{
    std::vector<uint32_t> constraintValueIndices(range.Values.size());
    // axisValues now gives indices into the Strings() table: extract the strings,
    // and do a lexicographical comparison to back that out into an index. position
    // of the matching string in this local table gives the index in axisValues
    auto extractStrView = [&](const int64_t value) -> std::string_view
    {
        return manifest.String(static_cast<uint32_t>(value));
    };
    std::span<const int64_t> axisValues = manifest.AxisValues(axis_index);
    std::vector<std::string_view> constraintStrings(axisValues.size());
    std::ranges::transform(axisValues, constraintStrings.begin(), extractStrView);
    // get iterators that match from axis.Values to constraintStrings, and use std::distance to convert to indices
    for (const auto&& [index, val] : std::views::enumerate(range.Values))
    {
        auto iter = std::ranges::find(constraintStrings, val.TypeName);
        if (iter == constraintStrings.end())
        {
            // TODO TODO TODO: our error handling state
            // leaving this stubbed for now as just an exception
            throw std::runtime_error("Constraint value not found in axis values.");
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
    for (const auto&& [axisIndex, radix] : std::views::enumerate(radices))
    {
        uint32_t minDigit = 0u;
        uint32_t maxDigit = radix - 1;
        // todo-ship: input constraints are sorted by axis index before calling this function
        const auto constraintIter = std::ranges::lower_bound(constraints,
                                                             axisIndex,
                                                             std::less<uint32_t>{},
                                                             &ScanConstraint::AxisIndex);
        if (constraintIter != constraints.end())
        {
            // std::ranges::minmax is pretty cool, neat!
            const auto [minValue, maxValue] = std::ranges::minmax(constraintIter->AllowedValueIndices);
            minDigit = minValue;
            maxDigit = maxValue;
        }

        minKey += static_cast<uint64_t>(minDigit) * placeValues[axisIndex];
        maxKey += static_cast<uint64_t>(maxDigit) * placeValues[axisIndex];
    }

    // now use lower_bound and upper_bound (since keys are already sorted) to create the span
    // we actually filter on (as a subspan of the original)
    std::span<const VariantKey> candidates = manifest.VariantKeys();
    const auto first = std::ranges::lower_bound(candidates, static_cast<VariantKey>(minKey));
    const auto last = std::ranges::upper_bound(candidates, static_cast<VariantKey>(maxKey));
    assert((first < last) && (first != std::end(candidates)) && (last != std::end(candidates)));
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
