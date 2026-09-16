#include "Suggest.hpp"
#include "TestHarness.hpp"

#include <array>
#include <span>
#include <string_view>
#include <vector>

using lodestone::FindSuggestions;
using lodestone::OptimalStringAlignmentDistance;
using lodestone::Suggestion;

int main()
{
    lodestone::tests::TestRunner runner{ "SuggestTests" };

    runner.BeginSection("the distance counts each edit kind");
    runner.Check(OptimalStringAlignmentDistance("", "") == 0, "two empty names agree");
    runner.Check(OptimalStringAlignmentDistance("axis", "axis") == 0, "an exact name costs nothing");
    runner.Check(OptimalStringAlignmentDistance("", "axis") == 4, "an empty name costs one insert per letter");
    runner.Check(OptimalStringAlignmentDistance("axes", "axis") == 1, "one substitution costs one");
    runner.Check(OptimalStringAlignmentDistance("axi", "axis") == 1, "one deletion costs one");

    runner.BeginSection("an adjacent transposition costs one, not two");
    runner.Check(OptimalStringAlignmentDistance("teh", "the") == 1, "a swapped pair is one edit");
    runner.Check(OptimalStringAlignmentDistance("flaot", "float") == 1, "a swap inside a word is one edit");

    runner.BeginSection("a candidate outside the threshold is dropped");
    const std::array<std::string_view, 3> axes{ "ShadeMode", "TileSize", "UseShadows" };
    const std::vector<Suggestion> none = FindSuggestions("Nonsense", axes, 2);
    runner.Check(none.empty(), "no near name yields no suggestion");

    runner.BeginSection("the nearest names come back first");
    const std::vector<Suggestion> matches = FindSuggestions("ShaddeMode", axes, 3);
    runner.Check(!matches.empty(), "a near name yields a suggestion");
    runner.Check(matches.front().Text == "ShadeMode", "the nearest name leads");
    runner.Check(matches.front().Distance == 1, "the nearest name is one edit away");

    runner.BeginSection("a negative threshold and an empty set yield nothing");
    runner.Check(FindSuggestions("ShadeMode", axes, -1).empty(), "a negative threshold yields nothing");
    const std::span<const std::string_view> empty;
    runner.Check(FindSuggestions("ShadeMode", empty, 3).empty(), "an empty candidate set yields nothing");

    return runner.Report();
}
