#pragma once
#ifndef LODESTONE_PERMUTATION_AXIS_HPP
#define LODESTONE_PERMUTATION_AXIS_HPP
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

struct PermutationAxis
{
    // I know these ctors are ugly but it lets us inplace-construct during space build at least

    explicit PermutationAxis(std::string name,
                             std::vector<PermutationValue> values,
                             AxisKind kind,
                             EarliestBindingTime binding_time,
                             AxisValueDomain value_domain,
                             std::string active_when = {}) noexcept;

    explicit PermutationAxis(std::string name,
                             std::vector<PermutationValue> values,
                             AxisKind kind,
                             EarliestBindingTime binding_time,
                             AxisValueDomain value_domain,
                             std::string active_when,
                             std::string root_name,
                             std::vector<RawInterfaceImpl> interface_impls) noexcept;

    explicit PermutationAxis(std::string name,
                             std::vector<PermutationValue> values,
                             AxisKind kind,
                             EarliestBindingTime binding_time,
                             AxisValueDomain value_domain,
                             std::string active_when,
                             std::string root_name,
                             std::vector<RawEnumCase> enum_cases) noexcept;

    std::string Name;
    AxisKind Kind{ AxisKind::None };
    EarliestBindingTime BindingTime{ EarliestBindingTime::None };
    AxisValueDomain ValueDomain{ AxisValueDomain::None };
    std::string ActiveWhen;

    [[nodiscard]] size_t NumValues() const noexcept;
    [[nodiscard]] std::span<const PermutationValue> GetValues() const noexcept;
    [[nodiscard]] const PermutationValue& GetDefault() const noexcept;
    
    void SetEnumCaseParams(std::string enum_type_name, std::vector<RawEnumCase> enum_cases) noexcept;
    void SetInterfaceAxisParams(std::string interface_name, std::vector<RawInterfaceImpl> interface_impls) noexcept;
    [[nodiscard]] std::string_view InterfaceName() const noexcept;
    [[nodiscard]] const RawInterfaceImpl& InterfaceImpl(uint32_t idx) const noexcept;
    [[nodiscard]] const RawEnumCase& EnumCase(uint32_t idx) const noexcept;
    /** @brief Get full qualified name of enum case at given index. like below, used to save one include where this is used */
    [[nodiscard]] std::string_view EnumTypeName() const noexcept;
    [[nodiscard]] std::string_view EnumCaseName(uint32_t idx) const noexcept;
    /** @brief Shortcut to get the name of the interface axis at `idx`, to avoid extra includes where this is used */
    [[nodiscard]] std::string_view InterfaceImplTypeName(uint32_t idx) const noexcept;
private:
    std::vector<PermutationValue> values;
    // rootName either holds root interface for type axis, or root enum type for enum axis
    // in either case, full instantiation of either type requires some kind of "root" name
    std::string rootName;
    std::vector<RawInterfaceImpl> interfaceImpls;
    std::vector<RawEnumCase> enumCases;
};

}

#endif // LODESTONE_PERMUTATION_AXIS_HPP
