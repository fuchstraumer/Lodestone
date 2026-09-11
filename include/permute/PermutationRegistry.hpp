#pragma once
#ifndef LODESTONE_PERMUTATION_REGISTRY_HPP
#define LODESTONE_PERMUTATION_REGISTRY_HPP
#include <string_view>

/** Finds the permutation space and the policy of one module by name.
 *
 * The registry is compiled in, and phase E step E6 replaces it with a data file. Each lookup gives an
 * empty space or an empty policy for a module that has no entry, so a caller never gets null. */
namespace lodestone
{

class PermutationSpace;
[[nodiscard]] const PermutationSpace* FindPermutationSpaceForModule(std::string_view module_name) noexcept;

} // namespace lodestone

#endif // !LODESTONE_PERMUTATION_REGISTRY_HPP
