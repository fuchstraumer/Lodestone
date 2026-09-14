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
    PermutationAxis(std::string name,
                    std::vector<PermutationValue> values,
                    AxisKind kind,
                    EarliestBindingTime binding_time,
                    AxisValueDomain value_domain,
                    std::string active_when = {}) noexcept;

    std::string Name;
    AxisKind Kind{ AxisKind::None };
    EarliestBindingTime BindingTime{ EarliestBindingTime::None };
    AxisValueDomain ValueDomain{ AxisValueDomain::None };
    std::string ActiveWhen;

    [[nodiscard]] size_t NumValues() const noexcept;
    [[nodiscard]] std::span<const PermutationValue> GetValues() const noexcept;
    [[nodiscard]] const PermutationValue& GetDefault() const noexcept;

    void SetInterfaceAxisParams(std::string interface_name, std::vector<RawInterfaceImpl> interface_impls) noexcept;
    [[nodiscard]] std::string_view InterfaceName() const noexcept;
    [[nodiscard]] const RawInterfaceImpl& InterfaceImpl(uint32_t idx) const noexcept;
    /** @brief Shortcut to get the name of the interface axis at `idx`, to avoid extra includes where this is used */
    [[nodiscard]] std::string_view InterfaceImplTypeName(uint32_t idx) const noexcept;
private:
    std::vector<PermutationValue> values;
    std::string interfaceName; // e.g, IBrdfImpl: the root interface that a Type axis instantiates
    std::vector<RawInterfaceImpl> interfaceImpls;
};

}

#endif // LODESTONE_PERMUTATION_AXIS_HPP
