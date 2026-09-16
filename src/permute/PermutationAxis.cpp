#include "permute/PermutationAxis.hpp"
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

PermutationAxis::PermutationAxis(std::string name,
                                 std::vector<PermutationValue> _values,
                                 AxisKind kind,
                                 EarliestBindingTime binding_time,
                                 AxisValueDomain value_domain,
                                 std::string active_when,
                                 std::string root_name,
                                 std::vector<RawInterfaceImpl> interface_impls) noexcept
    : Name(std::move(name)),
      Kind(kind),
      BindingTime(binding_time),
      ValueDomain(value_domain),
      ActiveWhen(std::move(active_when)),
      values(std::move(_values)),
      rootName(std::move(root_name)),
      interfaceImpls(std::move(interface_impls))
{
}

PermutationAxis::PermutationAxis(std::string name,
                                 std::vector<PermutationValue> _values,
                                 AxisKind kind,
                                 EarliestBindingTime binding_time,
                                 AxisValueDomain value_domain,
                                 std::string active_when,
                                 std::string root_name,
                                 std::string root_module,
                                 std::vector<RawEnumCase> enum_cases) noexcept
    : Name(std::move(name)),
      Kind(kind),
      BindingTime(binding_time),
      ValueDomain(value_domain),
      ActiveWhen(std::move(active_when)),
      values(std::move(_values)),
      rootName(std::move(root_name)),
      rootModule(std::move(root_module)),
      enumCases(std::move(enum_cases))
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

void PermutationAxis::SetEnumCaseParams(std::string enum_type_name, std::vector<RawEnumCase> enum_cases) noexcept
{
    rootName = std::move(enum_type_name);
    enumCases = std::move(enum_cases);
}

void PermutationAxis::SetInterfaceAxisParams(std::string interface_name, std::vector<RawInterfaceImpl> interface_impls) noexcept
{
    rootName = std::move(interface_name);
    interfaceImpls = std::move(interface_impls);
}

std::string_view PermutationAxis::InterfaceName() const noexcept
{
    return rootName;
}

const RawInterfaceImpl& PermutationAxis::InterfaceImpl(uint32_t idx) const noexcept
{
    return interfaceImpls[static_cast<size_t>(idx)];
}

std::string_view PermutationAxis::InterfaceImplTypeName(uint32_t idx) const noexcept
{
    return interfaceImpls[static_cast<size_t>(idx)].TypeName;
}

const RawEnumCase& PermutationAxis::EnumCase(uint32_t idx) const noexcept
{
    return enumCases[static_cast<size_t>(idx)];
}

std::string_view PermutationAxis::EnumTypeName() const noexcept
{
    return rootName;
}

std::string_view PermutationAxis::Module() const noexcept
{
    return rootModule;
}

std::string_view PermutationAxis::EnumCaseName(uint32_t idx) const noexcept
{
    return enumCases[static_cast<size_t>(idx)].Name;
}

} // namespace lodestone
