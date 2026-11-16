#include "ShaderManifestDevice.hpp"
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestIndex.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestLayout.hpp"
#include "Suggest.hpp"
#include "VariantKey.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
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

namespace
{
    std::vector<QueryAxisValue> AllBooleanValues(std::string_view axis_name)
    {
        return
        {
            QueryAxisValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = 0u, .Name = axis_name },
            QueryAxisValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = 1u, .Name = axis_name }
        };
    }

    /** Both spans have the same length. */
    bool IsCapabilitySubset(std::span<const uint64_t> subset, std::span<const uint64_t> superset) noexcept;
}

std::string_view NearestName(std::string_view input, std::span<const std::string_view> candidates) noexcept
{
    // the budget clang and rust use
    const int32_t maxDistance = std::max<int32_t>(1, static_cast<int32_t>(input.size() / 3u));
    const std::vector<Suggestion> suggestions = FindSuggestions(input, candidates, maxDistance);
    return suggestions.empty() ? std::string_view{} : suggestions.front().Text;
}

ManifestQueryBuilder::ManifestQueryBuilder(const class ManifestIndex& _index) noexcept : index(&_index)
{
}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name, bool value) const noexcept
{
    // a boolean axis shares the integral paths, with values {0, 1}
    QueryAxisValue newValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = value ? 1u : 0u, .Name = {} };
    return whereAnyOf(axis_name, { newValue }, PinMode::Required);

}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name, uint32_t value) const noexcept
{
    QueryAxisValue newValue{ .Type = AxisValueDomain::Integral, .IntegralValue = value, .Name = {} };
    return whereAnyOf(axis_name, { newValue }, PinMode::Required);
}

ManifestQueryBuilder ManifestQueryBuilder::Where(std::string_view axis_name,
                                                 AxisValueDomain domain,
                                                 std::string_view type_or_enum_name) const noexcept
{
    QueryAxisValue newValue{ .Type = domain, .IntegralValue=0u, .Name = type_or_enum_name };
    return whereAnyOf(axis_name, { newValue }, PinMode::Required);
}

ManifestQueryBuilder ManifestQueryBuilder::WhereWhenActive(std::string_view axis_name, bool value) const noexcept
{
    QueryAxisValue newValue{ .Type = AxisValueDomain::Boolean, .IntegralValue = value ? 1u : 0u, .Name = {} };
    return whereAnyOf(axis_name, { newValue }, PinMode::WhenActive);
}

ManifestQueryBuilder ManifestQueryBuilder::WhereWhenActive(std::string_view axis_name, uint32_t value) const noexcept
{
    QueryAxisValue newValue{ .Type = AxisValueDomain::Integral, .IntegralValue = value, .Name = {} };
    return whereAnyOf(axis_name, { newValue }, PinMode::WhenActive);
}

ManifestQueryBuilder ManifestQueryBuilder::WhereWhenActive(std::string_view axis_name,
                                                           AxisValueDomain domain,
                                                           std::string_view type_or_enum_name) const noexcept
{
    QueryAxisValue newValue{ .Type = domain, .IntegralValue = 0u, .Name = type_or_enum_name };
    return whereAnyOf(axis_name, { newValue }, PinMode::WhenActive);
}

ManifestQueryBuilder ManifestQueryBuilder::Apply(const QueryPreset& preset) const
{
    ManifestQueryBuilder result{ *this };
    for (const QueryPresetPin& pin : preset.Pins())
    {
        if (!index->axisNameToIndex.contains(pin.AxisName))
        {
            result.notes.emplace_back(QueryNoteCode::PinAxisNotDeclared, pin.AxisName);
            continue;
        }

        std::vector<QueryAxisValue> values;
        if (pin.Domain == AxisValueDomain::Type || pin.Domain == AxisValueDomain::Enum)
        {
            values.reserve(pin.NamedValues.size());
            for (const std::string& name : pin.NamedValues)
            {
                values.emplace_back(QueryAxisValue{ .Type = pin.Domain, .IntegralValue = 0u, .Name = name });
            }
        }
        else
        {
            values.reserve(pin.IntegralValues.size());
            for (const AxisValueType value : pin.IntegralValues)
            {
                values.emplace_back(QueryAxisValue{ .Type = pin.Domain, .IntegralValue = value, .Name = {} });
            }
        }

        const std::optional<uint32_t> axisIndex = resolveAndValidate(result, pin.AxisName, values);
        if (axisIndex)
        {
            result.noteDuplicateValues(pin.AxisName, *axisIndex, values);
            result.insertConstraint(pin.AxisName, *axisIndex, std::move(values), pin.Mode);
        }
    }
    return result;
}

void QueryPreset::Pin(std::string_view axis_name, bool value, PinMode mode)
{
    pins.emplace_back(QueryPresetPin{ .AxisName = std::string(axis_name),
                                      .Domain = AxisValueDomain::Boolean,
                                      .IntegralValues = { value ? 1u : 0u },
                                      .NamedValues = {},
                                      .Mode = mode });
}

void QueryPreset::Pin(std::string_view axis_name, uint32_t value, PinMode mode)
{
    pins.emplace_back(QueryPresetPin{ .AxisName = std::string(axis_name),
                                      .Domain = AxisValueDomain::Integral,
                                      .IntegralValues = { value },
                                      .NamedValues = {},
                                      .Mode = mode });
}

void QueryPreset::Pin(std::string_view axis_name,
                      AxisValueDomain domain,
                      std::string_view type_or_enum_name,
                      PinMode mode)
{
    pins.emplace_back(QueryPresetPin{ .AxisName = std::string(axis_name),
                                      .Domain = domain,
                                      .IntegralValues = {},
                                      .NamedValues = { std::string(type_or_enum_name) },
                                      .Mode = mode });
}

void QueryPreset::PinAnyOf(std::string_view axis_name, std::span<const uint32_t> values, PinMode mode)
{
    pins.emplace_back(QueryPresetPin{ .AxisName = std::string(axis_name),
                                      .Domain = AxisValueDomain::Integral,
                                      .IntegralValues = values | std::ranges::to<std::vector<AxisValueType>>(),
                                      .NamedValues = {},
                                      .Mode = mode });
}

void QueryPreset::PinAnyOf(std::string_view axis_name,
                           AxisValueDomain domain,
                           std::span<const std::string_view> values,
                           PinMode mode)
{
    pins.emplace_back(QueryPresetPin{ .AxisName = std::string(axis_name),
                                      .Domain = domain,
                                      .IntegralValues = {},
                                      .NamedValues = values | std::ranges::to<std::vector<std::string>>(),
                                      .Mode = mode });
}

std::span<const QueryPresetPin> QueryPreset::Pins() const noexcept
{
    return pins;
}

ManifestQueryBuilder ManifestQueryBuilder::WhereAnyOfBoolean(std::string_view axis_name) const
{
    return whereAnyOf(axis_name, AllBooleanValues(axis_name), PinMode::Required);
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
    return whereAnyOf(axis_name, std::move(queryValues), PinMode::Required);
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
    return whereAnyOf(axis_name, std::move(queryValues), PinMode::Required);
}

ManifestQueryBuilder ManifestQueryBuilder::WhereNoneOf(std::string_view axis_name, bool value) const noexcept
{
    return Where(axis_name, !value);
}

ManifestQueryBuilder ManifestQueryBuilder::WhereNoneOf(std::string_view axis_name, std::span<const uint32_t> values) const noexcept
{
    ManifestQueryBuilder result{ *this };
    auto buildAxisValue = [](uint32_t value) -> QueryAxisValue
    {
        return QueryAxisValue{ .Type = AxisValueDomain::Integral, .IntegralValue = value, .Name = {} };
    };
    std::vector<QueryAxisValue> queryValues = values |
                                              std::views::transform(buildAxisValue) |
                                              std::ranges::to<std::vector>();
    std::optional<uint32_t> axisIndexOpt = resolveAndValidate(result, axis_name, queryValues);
    if (!axisIndexOpt)
    {
        return result;
    }

    const uint32_t axisIndex = *axisIndexOpt;
    std::span<const AxisValueType> axisValues = index->axisValues[axisIndex];
    std::vector<QueryAxisValue> complement = axisValues |
                                             std::views::transform(buildAxisValue) |
                                             std::ranges::to<std::vector>();
    
    auto isInQueryValues = [&queryValues](const QueryAxisValue& value) -> bool
    {
        return std::ranges::find(queryValues, value) != queryValues.end();
    };

    std::erase_if(complement, isInQueryValues);

    result.insertConstraint(axis_name, axisIndex, std::move(complement), PinMode::Required);
    return result;
}

ManifestQueryBuilder ManifestQueryBuilder::WhereNoneOf(std::string_view axis_name,
                                                       AxisValueDomain domain,
                                                       std::span<const std::string_view> input_values) const noexcept
{
    ManifestQueryBuilder result{ *this };
    
    auto buildAxisValue = [domain](std::string_view value) -> QueryAxisValue
    {
        return QueryAxisValue{ .Type = domain, .IntegralValue = 0u, .Name = value };
    };

    std::vector<QueryAxisValue> queryValues = input_values |
                                              std::views::transform(buildAxisValue) |
                                              std::ranges::to<std::vector>();

    std::optional<uint32_t> axisIndexOpt = resolveAndValidate(result, axis_name, queryValues);
    if (!axisIndexOpt)
    {
        return result;
    }

    const uint32_t axisIndex = *axisIndexOpt;
    std::span<const AxisValueType> axisValues = index->axisValues[axisIndex];
    auto readStrTable = [&](const AxisValueType& axis_value) -> std::string_view
    {
        return index->environment.String(axis_value);
    };
    std::vector<QueryAxisValue> complement = axisValues |
                                             std::views::transform(readStrTable) |
                                             std::views::transform(buildAxisValue) |
                                             std::ranges::to<std::vector>();
    auto isInQueryValues = [&](const QueryAxisValue& value) -> bool
    {
        return std::ranges::find(queryValues, value) != queryValues.end();
    };

    // do not ask the standards committee why this overload of erase_if is needed vs remove_if lmao
    std::erase_if(complement, isInQueryValues);
    
    result.insertConstraint(axis_name, axisIndex, std::move(complement), PinMode::Required);
    return result;
}

QueryResult<std::vector<VariantKey>> ManifestQueryBuilder::Keys() const noexcept
{
    if (!errors.empty())
    {
        return std::unexpected(errors.front().Code);
    }

    if (preferCapable && !device.has_value())
    {
        return std::unexpected(QueryErrorCode::PreferCapableNeedsDevice);
    }

    manifest::DeviceFit rejection;
    return selectFitting(allEntryPoints(), rejection);
}

ManifestQueryBuilder ManifestQueryBuilder::ForDevice(manifest::CapabilitySupportFn supports,
                                                     void* user,
                                                     const manifest::DeviceLimits& limits) const
{
    ManifestQueryBuilder result = *this;
    if (!index->environment.HasCodeFacts())
    {
        result.errors.push_back(QueryError{ .Code = QueryErrorCode::DeviceFactsUnavailable });
        return result;
    }

    const manifest::EnvironmentView& environment = index->environment;
    std::vector<uint64_t> supported(environment.MakeDeviceCapabilities(supports, user, {}));
    environment.MakeDeviceCapabilities(supports, user, supported);
    result.device = DeviceState{ .Supported = std::move(supported), .Limits = limits };
    return result;
}

ManifestQueryBuilder ManifestQueryBuilder::PreferCapable() const noexcept
{
    ManifestQueryBuilder result = *this;
    result.preferCapable = true;
    return result;
}

std::vector<uint32_t> ManifestQueryBuilder::allEntryPoints() const
{
    std::vector<uint32_t> all(index->environment.Module().EntryPoints().size());
    std::iota(all.begin(), all.end(), 0u);
    return all;
}

std::vector<VariantKey> ManifestQueryBuilder::selectFitting(std::span<const uint32_t> entry_points,
                                                            manifest::DeviceFit& first_rejection) const noexcept
{
    std::vector<VariantKey> keys = index->select(constraints);
    if (!device.has_value())
    {
        return keys;
    }

    const manifest::EnvironmentView& environment = index->environment;
    const auto doesNotFit = [&](VariantKey key)
    {
        const manifest::VariantView variant = environment.VariantAt(*environment.FindVariant(key));
        for (const uint32_t entryPoint : entry_points)
        {
            const manifest::DeviceFit fit = variant.EntryPoint(entryPoint).Fit(device->Supported, device->Limits);
            if (!fit.Fits())
            {

                if (first_rejection.Fits())
                {
                    first_rejection = fit;
                }
                
                return true;
            }
        }
        return false;
    };

    std::erase_if(keys, doesNotFit);

    if (preferCapable)
    {
        removeDominatedByCapability(keys, entry_points);
    }

    return keys;
}

void ManifestQueryBuilder::removeDominatedByCapability(std::vector<VariantKey>& keys,
                                                       std::span<const uint32_t> entry_points) const noexcept
{
    const manifest::EnvironmentView& environment = index->environment;
    const manifest::ModuleView& module = environment.Module();

    std::vector<uint32_t> capabilityAxes;
    for (uint32_t axisIndex = 0u; std::cmp_less(axisIndex, index->radices.size()); ++axisIndex)
    {
        const manifest::Axis& axis = module.AxisData(axisIndex);
        if (axis.Kind == AxisKind::Capability)
        {
            capabilityAxes.push_back(axisIndex);
        }
    }

    if (capabilityAxes.empty())
    {
        return;
    }

    // the key with every capability digit zeroed is equal for two keys exactly when they differ only in capability axes
    const auto withoutCapabilityDigits = [&](VariantKey key) -> uint64_t
    {
        uint64_t remaining = std::to_underlying(key);
        for (const uint32_t axisIndex : capabilityAxes)
        {
            remaining -= static_cast<uint64_t>(index->digitOf(key, axisIndex)) * index->placeValues[axisIndex];
        }
        return remaining;
    };

    std::vector<std::pair<uint64_t, VariantKey>> classes;
    classes.reserve(keys.size());
    for (const VariantKey key : keys)
    {
        classes.emplace_back(withoutCapabilityDigits(key), key);
    }
    std::ranges::sort(classes);

    const auto capabilitiesOf = [&](VariantKey key, uint32_t entry_point) -> std::span<const uint64_t>
    {
        const manifest::VariantView variant = environment.VariantAt(*environment.FindVariant(key));
        return variant.EntryPoint(entry_point).RequiredCapabilities();
    };

    const auto dominates = [&](VariantKey dominant, VariantKey dominated) -> bool
    {
        bool strict = false;
        for (const uint32_t entryPoint : entry_points)
        {
            const std::span<const uint64_t> dominantNames = capabilitiesOf(dominant, entryPoint);
            const std::span<const uint64_t> dominatedNames = capabilitiesOf(dominated, entryPoint);
            if (!IsCapabilitySubset(dominatedNames, dominantNames))
            {
                return false;
            }

            strict = strict || !std::ranges::equal(dominatedNames, dominantNames);
        }

        return strict;
    };

    std::vector<VariantKey> removed;
    size_t begin = 0u;
    while (begin < classes.size())
    {
        size_t end = begin + 1u;
        while (end < classes.size() && classes[end].first == classes[begin].first)
        {
            ++end;
        }

        for (size_t candidate = begin; candidate < end; ++candidate)
        {
            const auto dominatesCandidate = [&](size_t other)
            {
                return other != candidate && dominates(classes[other].second, classes[candidate].second);
            };

            const auto others = std::views::iota(begin, end);
            if (std::ranges::any_of(others, dominatesCandidate))
            {
                removed.push_back(classes[candidate].second);
            }
        }

        begin = end;
    }

    std::ranges::sort(removed);
    const auto isRemoved = [&removed](VariantKey key)
    {
        return std::ranges::binary_search(removed, key);
    };
    std::erase_if(keys, isRemoved);
}

QueryResult<std::vector<DecodedVariant>> ManifestQueryBuilder::Variants() const noexcept
{
    auto keysResult = Keys();
    if (!keysResult)
    {
        return std::unexpected(keysResult.error());
    }
    const std::vector<VariantKey>& keys = *keysResult;

    std::vector<DecodedVariant> results;
    results.reserve(keys.size());
    for (const VariantKey key : keys)
    {
        results.emplace_back(key, index->Decode(key));
    }
    
    return results;
}

std::vector<DecodedVariant> ManifestQueryBuilder::VariantsFromKeys(const std::vector<VariantKey>& keys) const noexcept
{
    std::vector<DecodedVariant> results;
    results.reserve(keys.size());
    for (const VariantKey key : keys)
    {
        results.emplace_back(key, index->Decode(key));
    }
    return results;
}

std::expected<VariantKey, QueryError> ManifestQueryBuilder::Single() const noexcept
{
    const manifest::ModuleView& module = index->environment.Module();
    std::vector<uint64_t> allAxes(module.AxisMaskWordCount(), ~uint64_t{ 0u });
    return singleOver(allAxes, allEntryPoints());
}

std::expected<VariantKey, QueryError> ManifestQueryBuilder::SingleFor(
    std::span<const std::string_view> entry_point_names) const noexcept
{
    if (!errors.empty())
    {
        return std::unexpected(errors.front());
    }

    std::vector<uint32_t> entryPoints;
    const std::expected<std::vector<uint64_t>, QueryError> relevant = joinRelevance(entry_point_names, entryPoints);
    if (!relevant)
    {
        return std::unexpected(relevant.error());
    }
    return singleOver(*relevant, entryPoints);
}

std::expected<std::vector<uint64_t>, QueryError> ManifestQueryBuilder::joinRelevance(
    std::span<const std::string_view> entry_point_names,
    std::vector<uint32_t>& entry_points) const
{
    if (entry_point_names.empty())
    {
        return std::unexpected(QueryError{ .Code = QueryErrorCode::EmptyEntryPointSet });
    }

    const manifest::EnvironmentView& environment = index->environment;
    const manifest::ModuleView& module = environment.Module();
    const std::span<const manifest::EntryPoint> entryPoints = module.EntryPoints();

    std::vector<uint64_t> relevant(module.AxisMaskWordCount(), 0u);
    for (const std::string_view name : entry_point_names)
    {
        const manifest::ManifestResult<uint32_t> found = module.FindEntryPointIndex(name);
        if (!found)
        {
            auto getNameStrView = [&](const manifest::EntryPoint& entry_point)->std::string_view
            {
                return environment.String(entry_point.NameString);
            };

            const std::vector<std::string_view> names =
                entryPoints |
                std::views::transform(getNameStrView) |
                std::ranges::to<std::vector>();

            return std::unexpected(QueryError{ .Code = QueryErrorCode::UnknownEntryPoint,
                                               .AxisName = std::string(name),
                                               .Suggestion = NearestName(name, names) });
        }

        
        entry_points.push_back(*found);
        const std::span<const uint64_t> mask = environment.AxisRelevance(*found);
        for (size_t word = 0u; word < relevant.size(); ++word)
        {
            relevant[word] |= mask[word];
        }
    }

    return relevant;
}

std::expected<std::vector<QueryNote>, QueryError> ManifestQueryBuilder::NotesFor(
    std::span<const std::string_view> entry_point_names) const
{
    std::vector<uint32_t> entryPoints;
    const std::expected<std::vector<uint64_t>, QueryError> relevant = joinRelevance(entry_point_names, entryPoints);
    if (!relevant)
    {
        return std::unexpected(relevant.error());
    }

    constexpr size_t k_WordBits = std::numeric_limits<uint64_t>::digits;
    std::vector<QueryNote> result = notes;
    const std::vector<uint64_t>& relevantBits = *relevant;
    for (const QueryAxisRange& constraint : constraints)
    {
        const uint32_t axisIndex = constraint.AxisIndex;
        const bool isRelevant = ((relevantBits[axisIndex / k_WordBits] >> (axisIndex % k_WordBits)) & 1u) != 0u;
        if (!isRelevant)
        {
            result.emplace_back(QueryNoteCode::PinAxisIrrelevant, std::string(constraint.AxisName));
        }
    }
    return result;
}

void ManifestQueryBuilder::noteDuplicateValues(std::string_view axis_name,
                                               uint32_t axis_index,
                                               std::span<const QueryAxisValue> values)
{
    const auto existing = std::ranges::find(constraints, axis_index, &QueryAxisRange::AxisIndex);
    for (size_t i = 0u; i < values.size(); ++i)
    {
        const bool earlierInInput = std::ranges::contains(values.first(i), values[i]);
        const bool inConstraint = existing != constraints.end() &&
                                  std::ranges::contains(existing->Values, values[i]);
        if (earlierInInput || inConstraint)
        {
            notes.emplace_back(QueryNoteCode::DuplicateValue, std::string(axis_name));
        }
    }
}

std::expected<VariantKey, QueryError> ManifestQueryBuilder::singleOver(std::span<const uint64_t> relevant_axes,
                                 std::span<const uint32_t> entry_points) const noexcept
{
    if (!errors.empty())
    {
        // the whole error, so the suggestion reaches the caller
        return std::unexpected(errors.front());
    }

    if (preferCapable && !device.has_value())
    {
        return std::unexpected(QueryError{ .Code = QueryErrorCode::PreferCapableNeedsDevice });
    }

    manifest::DeviceFit rejection;
    const std::vector<VariantKey> keys = selectFitting(entry_points, rejection);
    if (keys.empty())
    {
        if (!rejection.Fits())
        {
            const std::string_view capability =
                rejection.Reason == manifest::DeviceMismatch::MissingCapability
                    ? index->environment.CapabilityName(rejection.Detail)
                    : std::string_view{};
            return std::unexpected(QueryError{ .Code = QueryErrorCode::NoVariantFitsDevice,
                                               .AxisName = std::string(capability),
                                               .Detail = static_cast<uint32_t>(rejection.Reason) });
        }
        return std::unexpected(QueryError{ .Code = QueryErrorCode::NoVariantForConstraints,
                                           .AxisName = std::string(blockingAxisName()) });
    }
    else if (keys.size() == 1u)
    {
        return keys.front();
    }

    constexpr size_t k_WordBits = std::numeric_limits<decltype(relevant_axes)::value_type>::digits;
    std::vector<uint32_t> varying = index->varyingAxes(keys);
    const auto isIrrelevant = [&](uint32_t axisIndex)
    {
        return ((relevant_axes[axisIndex / k_WordBits] >> (axisIndex % k_WordBits)) & 1u) == 0u;
    };
    std::erase_if(varying, isIrrelevant);

    if (varying.empty())
    {
        // the matches give the same instances. The smallest key keeps the answer stable between calls
        return std::ranges::min(keys);
    }

    return std::unexpected(QueryError{ .Code = QueryErrorCode::AmbiguousVariant,
                                       .Detail = static_cast<uint32_t>(keys.size()),
                                       .VaryingAxes = std::move(varying) });
}

std::string_view ManifestQueryBuilder::blockingAxisName() const
{
    if (constraints.empty())
    {
        return {};
    }

    // holds every constraint but the one at `i`. After each test, the removed constraint goes back in at the
    // position of the next one to remove
    std::vector<QueryAxisRange> others(constraints.begin() + 1, constraints.end());
    for (size_t i = 0u; i < constraints.size(); ++i)
    {
        if (!index->select(others).empty())
        {
            return constraints[i].AxisName;
        }

        if (i < others.size())
        {
            others[i] = constraints[i];
        }
    }

    return {};
}

bool ManifestQueryBuilder::IsValid() const noexcept
{
    return errors.empty();
}

std::span<const QueryError> ManifestQueryBuilder::Errors() const noexcept
{
    return errors;
}

std::span<const QueryNote> ManifestQueryBuilder::Notes() const noexcept
{
    return notes;
}

std::optional<uint32_t> ManifestQueryBuilder::resolveAndValidate(ManifestQueryBuilder& result,
                                                                 std::string_view axis_name,
                                                                 std::span<const QueryAxisValue> values) const
{
    auto indexIter = index->axisNameToIndex.find(axis_name);
    if (indexIter == index->axisNameToIndex.end())
    {
        const std::vector<std::string_view> axisNames =
            index->axisNameToIndex | std::views::keys | std::ranges::to<std::vector>();
        result.errors.emplace_back(QueryErrorCode::UnknownAxis,
                                   std::string(axis_name),
                                   0u,
                                   NearestName(axis_name, axisNames));
        return std::nullopt;
    }

    if (values.empty())
    {
        result.errors.emplace_back(QueryErrorCode::EmptyConstraintSet,
                                   std::string(axis_name));
        return std::nullopt;
    }

    const uint32_t axisIndex = indexIter->second;
    const manifest::Axis& axis = index->environment.Module().AxisData(axisIndex);
    // values are homogeneous by construction
    if (values.front().Type != axis.Domain)
    {
        result.errors.emplace_back(QueryErrorCode::IncorrectValueDomain,
                                   std::string(axis_name),
                                   static_cast<uint32_t>(axis.Domain));
        return std::nullopt;
    }

    // a boolean axis always declares 0 and 1. The value lists are short, so linear searches are fine
    if (axis.Domain == AxisValueDomain::Type || axis.Domain == AxisValueDomain::Enum)
    {
        const std::vector<std::string_view> axisValueNames = index->stringTableForAxis(axisIndex);
        auto queryValueNames = values | std::views::transform(&QueryAxisValue::Name);
        for (const auto& queryValueName : queryValueNames)
        {
            if (std::ranges::find(axisValueNames, queryValueName) == axisValueNames.end())
            {
                result.errors.emplace_back(QueryErrorCode::ValueNotInAxis,
                                           std::string(axis_name),
                                           0u,
                                           NearestName(queryValueName, axisValueNames));
                return std::nullopt;
            }
        }
    }
    else if (axis.Domain == AxisValueDomain::Integral)
    {
        std::span<const AxisValueType> axisValues = index->axisValues[axisIndex];
        auto queryIntegralValues = values | std::views::transform(&QueryAxisValue::IntegralValue);
        for (const auto& queryValue : queryIntegralValues)
        {
            if (std::ranges::find(axisValues, queryValue) == axisValues.end())
            {
                result.errors.emplace_back(QueryErrorCode::ValueNotInAxis,
                                           std::string(axis_name),
                                           queryValue); // empty suggestion 
                return std::nullopt;
            }
        }
    }

    return axisIndex;
}

void ManifestQueryBuilder::insertConstraint(std::string_view axis_name,
                                            uint32_t axis_index,
                                            std::vector<QueryAxisValue> allowed,
                                            PinMode mode)
{
    auto constraintIter = std::ranges::lower_bound(constraints,
                                                   axis_index,
                                                   std::less<uint32_t>{},
                                                   &QueryAxisRange::AxisIndex);
    if (constraintIter != constraints.end() &&
        constraintIter->AxisIndex == axis_index)
    {
        QueryAxisRange& constraint = *constraintIter;
        constraint.Values.append_range(allowed);
        // (active and A) or (inactive or B) is (inactive or A or B)
        if (mode == PinMode::WhenActive)
        {
            constraint.Mode = PinMode::WhenActive;
        }
    }
    else
    {
        constraints.insert(constraintIter,
            QueryAxisRange{ axis_name, std::move(allowed), axis_index, mode });
    }
}

ManifestQueryBuilder ManifestQueryBuilder::whereAnyOf(std::string_view axis_name,
                                                     std::vector<QueryAxisValue> values,
                                                     PinMode mode) const noexcept
{
    ManifestQueryBuilder result{ *this };
    std::optional<uint32_t> axisIndexOpt = resolveAndValidate(result, axis_name, values);
    if (!axisIndexOpt)
    {
        return result;
    }

    const uint32_t axisIndex = *axisIndexOpt;
    result.noteDuplicateValues(axis_name, axisIndex, values);
    result.insertConstraint(axis_name, axisIndex, std::move(values), mode);
    return result;
}

namespace
{
    bool IsCapabilitySubset(std::span<const uint64_t> subset, std::span<const uint64_t> superset) noexcept
    {
        const auto isWordSubset = [](uint64_t subset_word, uint64_t superset_word) -> bool
        {
            return (subset_word & ~superset_word) == 0u;
        };

        return std::ranges::equal(subset, superset, isWordSubset);
    }
}

} // namespace lodestone

#ifdef __clang__
#pragma clang diagnostic pop
#endif
