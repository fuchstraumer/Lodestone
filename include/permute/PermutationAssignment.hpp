#pragma once
#ifndef LODESTONE_PERMUTATION_ASSIGNMENT_HPP
#define LODESTONE_PERMUTATION_ASSIGNMENT_HPP
#include "permute/PermutationValue.hpp"
#include "permute/PermutationAxis.hpp"
#include <cstddef>
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

    // (clang annoyingly complains bc we don't match our clang-format, but these are stl-compat overrides) 
    //NOLINTBEGIN(readability-identifier-naming)
    [[nodiscard]] std::size_t size() const noexcept;
    using value_type = PermutationBinding;
    using iterator = PermutationAssignment::iterator;
    using const_iterator = PermutationAssignment::const_iterator;
    using const_reference = PermutationAssignment::const_reference;
    [[nodiscard]] const_reference operator[](std::size_t index) const noexcept;
    [[nodiscard]] iterator begin() noexcept;
    [[nodiscard]] const_iterator begin() const noexcept;
    [[nodiscard]] iterator end() noexcept;
    [[nodiscard]] const_iterator end() const noexcept;
    //NOLINTEND(readability-identifier-naming)

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
