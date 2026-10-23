#pragma once
#ifndef LODESTONE_PERMUTATION_AXIS_HPP
#define LODESTONE_PERMUTATION_AXIS_HPP
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

struct PermutationAxis
{

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
                             std::string root_module,
                             std::vector<RawEnumCase> enum_cases) noexcept;

    std::string Name;
    AxisKind Kind{ AxisKind::None };
    EarliestBindingTime BindingTime{ EarliestBindingTime::None };
    AxisValueDomain ValueDomain{ AxisValueDomain::None };
    std::string ActiveWhen;

    [[nodiscard]] size_t NumValues() const noexcept;
    [[nodiscard]] std::span<const PermutationValue> GetValues() const noexcept;
    [[nodiscard]] const PermutationValue& operator[](uint32_t idx) const noexcept;
    /** @brief The value an unused axis takes in the canonical assignment. Not the declared default. */
    [[nodiscard]] const PermutationValue& GetCanonicalValue() const noexcept;
    [[nodiscard]] const PermutationValue& GetDeclaredDefault() const noexcept;
    [[nodiscard]] uint32_t DeclaredDefaultIndex() const noexcept;
    void SetDeclaredDefaultIndex(uint32_t value_index) noexcept;
    
    void SetEnumCaseParams(std::string enum_type_name, std::vector<RawEnumCase> enum_cases) noexcept;
    void SetInterfaceAxisParams(std::string interface_name, std::vector<RawInterfaceImpl> interface_impls) noexcept;
    [[nodiscard]] std::string_view InterfaceName() const noexcept;
    [[nodiscard]] const RawInterfaceImpl& InterfaceImpl(uint32_t idx) const noexcept;
    [[nodiscard]] const RawEnumCase& EnumCase(uint32_t idx) const noexcept;
    [[nodiscard]] std::string_view EnumTypeName() const noexcept;
    [[nodiscard]] std::string_view EnumCaseName(uint32_t idx) const noexcept;
    /** @brief The ordinal of the case, for `PermutationValue::MakeEnum`. */
    [[nodiscard]] std::optional<uint32_t> FindEnumCase(std::string_view case_name) const noexcept;
    [[nodiscard]] std::string_view InterfaceImplTypeName(uint32_t idx) const noexcept;
    
    /** @brief The module that declares this axis's type, for the synthetic module's import. */
    [[nodiscard]] std::string_view Module() const noexcept;
private:
    std::vector<PermutationValue> values;
    // the root interface of a type axis, or the enum of an enum axis
    std::string rootName;
    std::string rootModule;
    std::vector<RawInterfaceImpl> interfaceImpls;
    std::vector<RawEnumCase> enumCases;
    // hand-built axes (tests) keep the first value
    uint32_t declaredDefaultIndex{ 0u };
};

}

#endif // LODESTONE_PERMUTATION_AXIS_HPP
