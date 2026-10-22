#pragma once
#ifndef LODESTONE_PERMUTATION_ASSIGNMENT_HPP
#define LODESTONE_PERMUTATION_ASSIGNMENT_HPP
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationValue.hpp"
#include <cstddef>
#include <string>
#include <vector>

namespace lodestone
{

/** @brief One axis bound to one value. */
struct PermutationBinding
{
    const PermutationAxis* Axis{ nullptr };
    PermutationValue Value;
};

using PermutationAssignment = std::vector<PermutationBinding>;

/** Every axis of one space, in declaration order. Only `CanonicalizeAssignment` builds one, so that a partial
 * assignment cannot reach the key computation. */
class CanonicalAssignment
{
public:
    CanonicalAssignment() noexcept = default;

    [[nodiscard]] operator const PermutationAssignment&() const noexcept;

    // stl-compatible names
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
    // ugly, but it allows exactly one way to build this
    friend class PermutationSpace;
    explicit CanonicalAssignment(PermutationAssignment&& canonical) noexcept;
    PermutationAssignment values;
};

std::string MakeAssignmentSuffix(const PermutationAssignment& assignment);
std::string DescribeAssignment(const PermutationAssignment& assignment);

} // namespace lodestone

#endif // LODESTONE_PERMUTATION_ASSIGNMENT_HPP
