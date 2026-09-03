#include "permute/PermutationAxis.hpp"
#include "permute/PermutationValue.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <utility>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-container"
#endif

namespace lodestone
{

PermutationAxis::PermutationAxis(std::string name,
                                 std::span<const PermutationValue> _values,
                                 AxisKind kind,
                                 EarliestBindingTime binding_time,
                                 AxisValueDomain value_domain,
                                 std::string active_when) noexcept
    : Name(std::move(name)),
      Kind(kind),
      BindingTime(binding_time),
      ValueDomain(value_domain),
      ActiveWhen(std::move(active_when))
{
    numValues = static_cast<int64_t>(std::min(_values.size(), static_cast<size_t>(k_MaxValues)));
    std::copy_n(_values.begin(), numValues, values.begin());
}

PermutationAxis::PermutationAxis(std::string name,
                                 std::initializer_list<PermutationValue> _values,
                                 AxisKind kind,
                                 EarliestBindingTime binding_time,
                                 AxisValueDomain value_domain,
                                 std::string active_when) noexcept
    : PermutationAxis(std::move(name),
                      std::span<const PermutationValue>{ _values.begin(), _values.size() },
                      kind,
                      binding_time,
                      value_domain,
                      std::move(active_when))
{
}

int64_t PermutationAxis::NumValues() const noexcept
{
    return numValues;
}

std::span<const PermutationValue> PermutationAxis::GetValues() const noexcept
{
    return std::span<const PermutationValue>{ values.data(), static_cast<size_t>(numValues) };
}

const PermutationValue& PermutationAxis::GetDefault() const noexcept
{
    return values.front();
}

} // namespace lodestone
