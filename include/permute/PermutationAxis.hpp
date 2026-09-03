#pragma once
#ifndef LODESTONE_PERMUTATION_AXIS_HPP
#define LODESTONE_PERMUTATION_AXIS_HPP
#include "PermutationValue.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>

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
    Enum
};

struct PermutationAxis
{
    /**This helps control permutation explosions, mostly. todo-ship: make it a cmake configure opt */
    static constexpr std::size_t k_MaxValues = 8u;
    /** `ParentIndex` of an axis with no parent. Such an axis is always active. */
    static constexpr int32_t k_NoParent = -1;

    PermutationAxis(std::string name,
                    std::span<const PermutationValue> values,
                    AxisKind kind,
                    EarliestBindingTime binding_time,
                    AxisValueDomain value_domain,
                    std::string active_when) noexcept;
    // initializer_list will be removed once we get to the data-driven permutation system
    // this just keeps things compiling and running, for now
    PermutationAxis(std::string name,
                    std::initializer_list<PermutationValue> values,
                    AxisKind kind,
                    EarliestBindingTime binding_time,
                    AxisValueDomain value_domain,
                    std::string active_when) noexcept;

    std::string Name;
    AxisKind Kind{ AxisKind::None };
    EarliestBindingTime BindingTime{ EarliestBindingTime::None };
    AxisValueDomain ValueDomain{ AxisValueDomain::None };
    std::string ActiveWhen{};

    [[nodiscard]] bool HasParent() const noexcept;
    [[nodiscard]] int64_t NumValues() const noexcept;
    [[nodiscard]] std::span<const PermutationValue> GetValues() const noexcept;
    [[nodiscard]] const PermutationValue& GetDefault() const noexcept;

private:
    int64_t numValues{ -1 };
    std::array<PermutationValue, k_MaxValues> values;
};

}

#endif // LODESTONE_PERMUTATION_AXIS_HPP
