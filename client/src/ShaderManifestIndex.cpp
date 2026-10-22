#include "ShaderManifestIndex.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "Suggest.hpp"
#include "VariantKey.hpp"
#include <algorithm>
#include <cassert>
#include <expected>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
// suppress these bc I'm doing them all quite intentionally here
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif

namespace lodestone
{

ManifestIndex::ManifestIndex(manifest::EnvironmentView view) : environment(view)
{
    const manifest::ModuleView& module = environment.Module();
    const uint32_t axisCount = module.AxisCount();
    axisValues.resize(axisCount);
    radices.resize(axisCount);
    placeValues.resize(axisCount);
    axisNameToIndex.reserve(axisCount);

    for (int32_t axisIndex = 0u; std::cmp_less(axisIndex, axisCount); ++axisIndex)
    {
        radices[axisIndex] = module.AxisValueCount(axisIndex);
        axisValues[axisIndex].reserve(radices[axisIndex]);
        for (int32_t digit = 0u; std::cmp_less(digit, radices[axisIndex]); ++digit)
        {
            axisValues[axisIndex].push_back(module.AxisValue(axisIndex, digit));
        }

        const std::string_view axisName = environment.String(module.AxisData(axisIndex).NameString);
        axisNameToIndex[axisName] = axisIndex;
    }

    std::exclusive_scan(radices.rbegin(),
                        radices.rend(),
                        placeValues.rbegin(),
                        static_cast<uint64_t>(1),
                        std::multiplies<uint64_t>{});
    
}

const manifest::EnvironmentView& ManifestIndex::View() const noexcept
{
    return environment;
}

std::vector<QueryAxisValue> ManifestIndex::Decode(VariantKey key) const
{
    std::vector<uint32_t> scratch(radices.size());
    UnpackVariantKey(key, radices, scratch);

    std::vector<QueryAxisValue> result(radices.size());

    for (const auto&& [axisIndex, valueIndex] : std::views::enumerate(scratch))
    {
        result[axisIndex] = decodeAxis(static_cast<uint32_t>(axisIndex), valueIndex);
    }

    if (const std::optional<uint32_t> variantIndex = environment.FindVariant(key); variantIndex.has_value())
    {
        markInactiveAxes(variantIndex.value(), result);
    }

    return result;
}

std::vector<DecodedVariant> ManifestIndex::Enumerate() const
{
    std::span<const VariantKey> variantKeys = environment.VariantKeys();
    std::vector<DecodedVariant> result(variantKeys.size());
    // Decode by hand, to reuse one scratch buffer

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
        markInactiveAxes(static_cast<uint32_t>(index), result[index].Values);
    }

    return result;
}

ManifestQueryBuilder ManifestIndex::Query() const noexcept
{
    return ManifestQueryBuilder{ *this };
}

std::vector<std::string_view> ManifestIndex::AxisNames() const
{
    const manifest::ModuleView& module = environment.Module();
    std::vector<std::string_view> names;
    names.reserve(module.AxisCount());
    for (uint32_t axisIndex = 0u; axisIndex < module.AxisCount(); ++axisIndex)
    {
        names.push_back(environment.String(module.AxisData(axisIndex).NameString));
    }
    return names;
}

QueryResult<std::vector<QueryAxisValue>> ManifestIndex::AxisValues(std::string_view axis_name) const
{
    const auto indexIter = axisNameToIndex.find(axis_name);
    if (indexIter == axisNameToIndex.end())
    {
        return std::unexpected(QueryErrorCode::UnknownAxis);
    }

    const uint32_t axisIndex = indexIter->second;
    std::vector<QueryAxisValue> values;
    values.reserve(radices[axisIndex]);
    for (uint32_t digit = 0u; digit < radices[axisIndex]; ++digit)
    {
        values.push_back(decodeAxis(axisIndex, digit));
    }
    return values;
}

QueryResult<QueryAxisValue> ManifestIndex::AxisDefault(std::string_view axis_name) const
{
    const auto indexIter = axisNameToIndex.find(axis_name);
    if (indexIter == axisNameToIndex.end())
    {
        return std::unexpected(QueryErrorCode::UnknownAxis);
    }

    const uint32_t axisIndex = indexIter->second;
    return decodeAxis(axisIndex, environment.Module().AxisDefaultDigit(axisIndex));
}

std::vector<ManifestIndex::ScanConstraint> ManifestIndex::convertToScanConstraints(std::span<const QueryAxisRange> query) const
{
    std::vector<ScanConstraint> scanConstraints;
    scanConstraints.reserve(query.size());
    for (const auto& range : query)
    {
        const manifest::Axis& axis = environment.Module().AxisData(range.AxisIndex);
        std::vector<uint32_t> valueIndices;
        if (axis.Domain == AxisValueDomain::Type || axis.Domain == AxisValueDomain::Enum)
        {
            valueIndices = stringValueIndices(range.AxisIndex, range);
        }
        else
        {
            valueIndices = integralValueIndices(range.AxisIndex, range);
        }

        // the builder reports errors
        if (valueIndices.empty())
        {
            return {};
        }

        scanConstraints.emplace_back(range.AxisIndex, std::move(valueIndices), range.Mode);
    }

    return scanConstraints;
}

std::vector<VariantKey> ManifestIndex::select(std::span<const QueryAxisRange> constraints) const
{
    assert(std::ranges::is_sorted(constraints, std::less<uint32_t>{}, &QueryAxisRange::AxisIndex));
    const std::vector<ScanConstraint> scanConstraints = convertToScanConstraints(constraints);
    return scan(scanConstraints);
}

QueryAxisValue ManifestIndex::decodeAxis(uint32_t axis_index, uint32_t value_index) const noexcept
{
    const manifest::Axis& axis = environment.Module().AxisData(axis_index);
    const AxisValueType currValue = axisValues[axis_index][value_index];
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
        [[fallthrough]];
    case AxisValueDomain::Type:
        result.Name = environment.String(currValue);
        break;
    case AxisValueDomain::None:
        std::unreachable();
    }
    return result;
}

std::vector<uint32_t> ManifestIndex::integralValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const
{
    std::vector<uint32_t> constraintValueIndices(range.Values.size());
    std::span<const AxisValueType> localValues = axisValues[axis_index];
    for (const auto&& [index, val] : std::views::enumerate(range.Values))
    {
        auto iter = std::ranges::find(localValues, val.IntegralValue);
        if (iter == localValues.end())
        {
            return {};
        }
        const uint32_t valueIndex = static_cast<uint32_t>(std::distance(localValues.begin(), iter));
        constraintValueIndices[index] = valueIndex;
    }
    
    return constraintValueIndices;
}

std::vector<uint32_t> ManifestIndex::stringValueIndices(const uint32_t axis_index, const QueryAxisRange& range) const
{
    std::vector<uint32_t> constraintValueIndices(range.Values.size());
    const std::vector<std::string_view> constraintStrings = stringTableForAxis(axis_index);
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

std::span<const VariantKey> ManifestIndex::filterKeys(std::span<const ScanConstraint> constraints) const
{
    // narrow to the [minKey, maxKey] band any match fits in, from each axis's smallest and largest allowed digit
    uint64_t minKey = 0u;
    uint64_t maxKey = 0u;
    uint32_t constraintCursor = 0u;
    for (int64_t axisIndex = 0; std::cmp_less(axisIndex, radices.size()); ++axisIndex)
    {
        uint32_t minDigit = 0u;
        uint32_t maxDigit = radices[axisIndex] - 1;
        if (std::cmp_less(constraintCursor, constraints.size()) &&
            std::cmp_equal(constraints[constraintCursor].AxisIndex , axisIndex))
        {
            const ScanConstraint& constraint = constraints[constraintCursor];
            const auto [minValue, maxValue] = std::ranges::minmax(constraint.AllowedValueIndices);
            // inactive axes store the canonical digit 0, which a when-active constraint keeps
            minDigit = constraint.Mode == PinMode::WhenActive ? 0u : minValue;
            maxDigit = maxValue;
            ++constraintCursor;
        }

        minKey += static_cast<uint64_t>(minDigit) * placeValues[axisIndex];
        maxKey += static_cast<uint64_t>(maxDigit) * placeValues[axisIndex];
    }

    std::span<const VariantKey> candidates = environment.VariantKeys();
    const auto first = std::ranges::lower_bound(candidates, static_cast<VariantKey>(minKey));
    const auto last = std::ranges::upper_bound(candidates, static_cast<VariantKey>(maxKey));
    assert(first <= last);
    return std::span<const VariantKey>{ first, last };
}

std::vector<VariantKey> ManifestIndex::scan(std::span<const ScanConstraint> constraints) const
{
    std::span<const VariantKey> candidates = filterKeys(constraints);
    std::vector<VariantKey> result;

    const uint32_t firstIndex = static_cast<uint32_t>(candidates.data() - environment.VariantKeys().data());
    for (const auto&& [offset, key] : std::views::enumerate(candidates))
    {
        if (matches(firstIndex + static_cast<uint32_t>(offset), key, constraints))
        {
            result.emplace_back(key);
        }
    }

    return result;
}

bool ManifestIndex::matches(uint32_t variant_index,
                            VariantKey key,
                            std::span<const ScanConstraint> constraints) const noexcept
{
    auto matchFn = [&](const ScanConstraint& constraint) -> bool
    {
        const bool active = environment.IsAxisActive(variant_index, constraint.AxisIndex);
        if (!active)
        {
            return constraint.Mode == PinMode::WhenActive;
        }
        const uint32_t digit = digitOf(key, constraint.AxisIndex);
        return std::ranges::contains(constraint.AllowedValueIndices, digit);
    };

    return std::ranges::all_of(constraints, matchFn);
}

uint32_t ManifestIndex::digitOf(VariantKey key, uint32_t axis_index) const noexcept
{
    return static_cast<uint32_t>((std::to_underlying(key) / placeValues[axis_index]) % radices[axis_index]);
}

std::vector<uint32_t> ManifestIndex::varyingAxes(std::span<const VariantKey> keys) const
{
    std::vector<uint32_t> result;
    for (uint32_t axisIndex = 0u; std::cmp_less(axisIndex, radices.size()); ++axisIndex)
    {
        const uint32_t firstDigit = digitOf(keys.front(), axisIndex);
        auto differsFromFirst = [&](VariantKey key) -> bool
        {
            return digitOf(key, axisIndex) != firstDigit;
        };
        if (std::ranges::any_of(keys, differsFromFirst))
        {
            result.push_back(axisIndex);
        }
    }
    return result;
}

void ManifestIndex::markInactiveAxes(uint32_t variant_index, std::span<QueryAxisValue> values) const noexcept
{
    for (const auto&& [axisIndex, value] : std::views::enumerate(values))
    {
        value.Active = environment.IsAxisActive(variant_index, static_cast<uint32_t>(axisIndex));
    }
}

std::vector<std::string_view> ManifestIndex::stringTableForAxis(uint32_t axis_index) const
{
    auto getStringView = [&](const AxisValueType& value) -> std::string_view
    {
        return environment.String(value);
    };
    return axisValues[axis_index] |
           std::views::transform(getStringView) |
           std::ranges::to<std::vector>();
}

}
