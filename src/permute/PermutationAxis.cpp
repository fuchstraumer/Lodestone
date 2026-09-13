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
                                 std::vector<PermutationValue> _values,
                                 AxisKind kind,
                                 EarliestBindingTime binding_time,
                                 AxisValueDomain value_domain,
                                 std::string active_when) noexcept
    : Name(std::move(name)),
      Kind(kind),
      BindingTime(binding_time),
      ValueDomain(value_domain),
      ActiveWhen(std::move(active_when)),
      values(std::move(_values))
{
}

size_t PermutationAxis::NumValues() const noexcept
{
    return values.size();
}

std::span<const PermutationValue> PermutationAxis::GetValues() const noexcept
{
    return std::span<const PermutationValue>{ values.data(), values.size() };
}

const PermutationValue& PermutationAxis::GetDefault() const noexcept
{
    return values.front();
}

void PermutationAxis::SetInterfaceAxisParams(std::string interface_name, std::vector<RawInterfaceImpl> interface_impls) noexcept
{
    interfaceName = std::move(interface_name);
    interfaceImpls = std::move(interface_impls);
}

} // namespace lodestone
