#pragma once
#ifndef LODESTONE_SUGGEST_HPP
#define LODESTONE_SUGGEST_HPP
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/**
 * @brief Finds the known name that a mistyped name most probably meant.
 *
 * A rejection names a value the author wrote and a set of values the cook accepts. This library reads
 * both and offers the accepted name that is nearest to the written one, the way a compiler asks "did
 * you mean X?". It measures nearness with the optimal string alignment distance, which counts an
 * adjacent transposition as one edit, so a common typo scores as one edit and not two.
 *
 * The library sits below the client and the cooker, because both reject a name and both want the same
 * suggestion. It matches a single name against the small set of names that were legal in one position,
 * never against a whole strings table, so the cost stays on the error path where it belongs.
 */
namespace lodestone
{

struct Suggestion
{
    std::string_view Text;
    int32_t Distance{ 0 };
};

[[nodiscard]] int32_t OptimalStringAlignmentDistance(std::string_view source, std::string_view target) noexcept;

/**
 * @brief Returns every candidate within maxDistance edits of input, nearest first.
 *
 * The result owns nothing but its own order. Each Text is a view into the matching candidate, so the
 * candidates must outlive the result. A tie in distance keeps the input order, so two cooks agree.
 */
[[nodiscard]] std::vector<Suggestion> FindSuggestions(std::string_view input,
                                                      std::span<const std::string_view> candidates,
                                                      int32_t max_distance) noexcept;

} // namespace lodestone

#endif // !LODESTONE_SUGGEST_HPP
