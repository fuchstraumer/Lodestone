#include "Suggest.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    struct AlignmentRows
    {
        std::vector<int32_t> BeforePrevious;
        std::vector<int32_t> Previous;
        std::vector<int32_t> Current;
    };

    int32_t BoundedAlignmentDistance(std::string_view source,
                                     std::string_view target,
                                     int32_t threshold,
                                     AlignmentRows& rows);

} // namespace

int32_t OptimalStringAlignmentDistance(std::string_view source, std::string_view target) noexcept
{
    AlignmentRows rows;
    return BoundedAlignmentDistance(source, target, std::numeric_limits<int32_t>::max(), rows);
}

std::vector<Suggestion> FindSuggestions(std::string_view input,
                                        std::span<const std::string_view> candidates,
                                        int32_t max_distance) noexcept
{
    std::vector<Suggestion> matches;
    if (max_distance < 0 || candidates.empty())
    {
        return matches;
    }

    matches.reserve(candidates.size());
    const int32_t inputLength = static_cast<int32_t>(std::ssize(input));

    AlignmentRows rows;
    for (const std::string_view candidate : candidates)
    {
        const int32_t candidateLength = static_cast<int32_t>(std::ssize(candidate));
        if (std::abs(inputLength - candidateLength) > max_distance)
        {
            continue;
        }

        const int32_t distance = BoundedAlignmentDistance(input, candidate, max_distance, rows);
        if (distance <= max_distance)
        {
            matches.push_back(Suggestion{ .Text = candidate, .Distance = distance });
        }
    }

    std::ranges::stable_sort(matches, {}, &Suggestion::Distance);
    return matches;
}

namespace
{

    int32_t BoundedAlignmentDistance(std::string_view source,
                                     std::string_view target,
                                     int32_t threshold,
                                     AlignmentRows& rows)
    {
        const int32_t sourceLength = static_cast<int32_t>(std::ssize(source));
        const int32_t targetLength = static_cast<int32_t>(std::ssize(target));
        const int32_t columnCount = targetLength + 1;

        rows.BeforePrevious.assign(columnCount, 0);
        rows.Previous.resize(columnCount);
        rows.Current.resize(columnCount);
        std::ranges::iota(rows.Previous, 0);

        for (int32_t row = 1; row <= sourceLength; ++row)
        {
            rows.Current[0] = row;
            int32_t rowMinimum = row;
            const char sourceChar = source[row - 1];

            for (int32_t column = 1; column <= targetLength; ++column)
            {
                const char targetChar = target[column - 1];
                const int32_t substitutionCost = sourceChar == targetChar ? 0 : 1;

                int32_t best = std::min({ rows.Previous[column] + 1,
                                          rows.Current[column - 1] + 1,
                                          rows.Previous[column - 1] + substitutionCost });

                const bool transposes = row > 1 &&
                                        column > 1 &&
                                        sourceChar == target[column - 2] &&
                                        source[row - 2] == targetChar;
                if (transposes)
                {
                    best = std::min(best, rows.BeforePrevious[column - 2] + 1);
                }

                rows.Current[column] = best;
                rowMinimum = std::min(rowMinimum, best);
            }

            if (rowMinimum > threshold)
            {
                return threshold + 1;
            }

            std::swap(rows.BeforePrevious, rows.Previous);
            std::swap(rows.Previous, rows.Current);
        }

        return rows.Previous[targetLength];
    }

} // namespace

} // namespace lodestone
