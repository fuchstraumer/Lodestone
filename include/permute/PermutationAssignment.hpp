#pragma once
#ifndef LODESTONE_PERMUTATION_ASSIGNMENT_HPP
#define LODESTONE_PERMUTATION_ASSIGNMENT_HPP
#include "permute/PermutationValue.hpp"
#include "permute/PermutationAxis.hpp"
#include <string>
#include <vector>

namespace lodestone
{

/** @brief A binding is a *concrete* assignment of a value to an axis,
  * it's a real assignment that has actually been evaluated. */
struct PermutationBinding
{
    const PermutationAxis* Axis{ nullptr };
    PermutationValue Value;
};

using PermutationAssignment = std::vector<PermutationBinding>;

/** An assignment that holds every axis of one space, in declaration order. Only
 * `CanonicalizeAssignment` builds one, so a partial assignment cannot reach `ComputeVariantIndex` and
 * return a plausible wrong index. The conversion to `PermutationAssignment` runs one way only. */
class CanonicalAssignment
{
public:
    CanonicalAssignment() noexcept = default;

    [[nodiscard]] operator const PermutationAssignment&() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept; //NOLINT(readability-identifier-naming)
    [[nodiscard]] const PermutationBinding& operator[](std::size_t index) const noexcept;

private:
    // friend class is ugly, but this lets us allow exactly one way to build a CanonicalAssignment, so 
    // that's worth it
    friend class PermutationSpace;
    explicit CanonicalAssignment(PermutationAssignment&& canonical) noexcept;

    PermutationAssignment values;
};

std::string MakeAssignmentSuffix(const PermutationAssignment& assignment);
std::string DescribeAssignment(const PermutationAssignment& assignment);

} // namespace lodestone

#endif // LODESTONE_PERMUTATION_ASSIGNMENT_HPP
