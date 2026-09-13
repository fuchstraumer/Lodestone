#pragma once
#ifndef LODESTONE_PERMUTATION_AXIS_HPP
#define LODESTONE_PERMUTATION_AXIS_HPP
#include "PermutationValue.hpp"
#include "compile/RawLibrary.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <vector>

namespace lodestone
{

enum class AxisKind : uint8_t
{
    None,
    ResourcePresence, // Whether a resource is used (e.g, texture, buffer, etc.)
    Capability, // Whether a specific capability is required, e.g Wave or Subgroup ops
    Tuning, // Often uses a size expression: buffer sizes, wave dims, thread dims, etc
    Technique // Which technique or algorithm is used: uniform branching
};

enum class EarliestBindingTime : uint8_t
{
    None = 0,
    Cook, // Value is set during cook (shader uniform)
    Bind, // Value is set during pipeline bind (pipeline uniform)
    Invocation, // Value is set for a single invocation of a pipeline (draw/dispatch uniform)
    Execution, // Value is set during shader execution (per-thread, divergent)
};

enum class AxisValueDomain : uint8_t
{
    None,
    Boolean,
    Integral,
    Enum,
    Type
};

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
private:
    std::vector<PermutationValue> values;
    std::string interfaceName; // e.g, IBrdfImpl: the root interface that a Type axis instantiates
    std::vector<RawInterfaceImpl> interfaceImpls;
};

}

#endif // LODESTONE_PERMUTATION_AXIS_HPP
