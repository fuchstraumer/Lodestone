#pragma once
#ifndef LODESTONE_SUGGEST_HPP
#define LODESTONE_SUGGEST_HPP
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** @brief "Did you mean X?": optimal string alignment distance, where a transposition is one edit. Shared by the
 * client and the cooker. */
namespace lodestone
{

struct Suggestion
{
    std::string_view Text;
    int32_t Distance{ 0 };
};

[[nodiscard]] int32_t OptimalStringAlignmentDistance(std::string_view source, std::string_view target) noexcept;

/** @brief Nearest first, ties in input order. Results view the candidates, which must outlive them. */
[[nodiscard]] std::vector<Suggestion> FindSuggestions(std::string_view input,
                                                      std::span<const std::string_view> candidates,
                                                      int32_t max_distance) noexcept;

} // namespace lodestone

#endif // !LODESTONE_SUGGEST_HPP
