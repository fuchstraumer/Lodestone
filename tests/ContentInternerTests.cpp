#include "model/ContentHash.hpp"
#include "model/ContentInterner.hpp"
#include "TestHarness.hpp"

#include <cstdint>
#include <format>
#include <span>
#include <string>

// every hash here returns one constant, so only the byte comparison can separate payloads

using lodestone::ContentHashValue;
using lodestone::ContentInterner;
using lodestone::InternerStatistics;
using lodestone::InternResult;
using lodestone::ProvenanceRecord;

namespace
{

/** Forces every payload into one bucket. This is the whole point of the file. */
ContentHashValue HashToOneBucket(const std::string& payload) noexcept
{
    static_cast<void>(payload);
    return 0x1234u;
}

ProvenanceRecord MakeOrigin(uint32_t variant_index)
{
    return ProvenanceRecord{ .EntryPointName = "MainCS",
                             .VariantDescription = std::format("variant {}", variant_index),
                             .VariantIndex = variant_index };
}

constexpr uint32_t k_PayloadCount = 10u;

std::string MakePayload(uint32_t index)
{
    return std::format("source number {}", index);
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "ContentInternerTests" };

    runner.BeginSection("distinct payloads survive a total hash collision");
    ContentInterner<std::string> interner{ &HashToOneBucket, "constant-for-test" };

    bool everyIndexIsNew = true;
    bool everyIndexIsDense = true;
    for (uint32_t i = 0u; i < k_PayloadCount; ++i)
    {
        const InternResult result = interner.Intern(MakePayload(i), MakeOrigin(i));
        if (!result.WasNew)
        {
            everyIndexIsNew = false;
        }

        if (result.Index != i)
        {
            everyIndexIsDense = false;
        }
    }

    runner.Check(everyIndexIsNew, "ten distinct payloads each report a new entry");
    runner.Check(everyIndexIsDense, "ten distinct payloads take ten dense indices");
    runner.Check(interner.UniqueEntries().size() == k_PayloadCount,
                 "the table holds one entry for each distinct payload");

    runner.BeginSection("the counters describe the work the comparison did");
    const InternerStatistics& afterDistinct = interner.Statistics();
    // Insert number k compares against the k-1 entries already in the bucket, and none of them match.
    // Ten inserts therefore force 0+1+2+...+9 comparisons, and each one is a real hash collision.
    constexpr uint32_t k_ExpectedComparisons = (k_PayloadCount * (k_PayloadCount - 1u)) / 2u;
    runner.Check(afterDistinct.ArtifactsSeen == k_PayloadCount, "every intern counts as an artifact");
    runner.Check(afterDistinct.UniqueEntries == k_PayloadCount, "no payload collapsed onto another");
    runner.Check(afterDistinct.HashCollisions == k_ExpectedComparisons,
                 "each equal hash with unequal bytes counts as a collision");
    runner.Check(afterDistinct.ByteComparisons == k_ExpectedComparisons,
                 "each hash hit forces one byte comparison");

    runner.BeginSection("an equal payload collapses and keeps both origins");
    const InternResult repeated = interner.Intern(MakePayload(0u), MakeOrigin(99u));
    runner.Check(!repeated.WasNew, "an equal payload reports no new entry");
    runner.Check(repeated.Index == 0u, "an equal payload returns the index of the first copy");
    runner.Check(interner.UniqueEntries().size() == k_PayloadCount, "the table did not grow");

    const std::span<const ProvenanceRecord> origins = interner.OriginsOf(0u);
    runner.Check(origins.size() == 2u, "the entry records both artifacts that mapped onto it");
    runner.Check(origins.size() == 2u && origins[0].VariantIndex == 0u && origins[1].VariantIndex == 99u,
                 "provenance keeps the origins in the order they arrived");

    runner.BeginSection("the identity path stays correct");
    ContentInterner<std::string> disabled{ &HashToOneBucket, "constant-for-test" };
    disabled.Disable();
    runner.Check(!disabled.IsEnabled(), "Disable turns collapsing off");

    const InternResult first = disabled.Intern(MakePayload(0u), MakeOrigin(0u));
    const InternResult second = disabled.Intern(MakePayload(0u), MakeOrigin(1u));
    runner.Check(first.Index == 0u && second.Index == 1u,
                 "with dedupe off, two equal payloads take two indices");
    runner.Check(second.WasNew, "with dedupe off, every artifact is a new entry");
    runner.Check(disabled.Statistics().ByteComparisons == 0u,
                 "with dedupe off, no byte comparison runs at all");

    runner.BeginSection("the hash name reaches the report");
    runner.Check(interner.HashName() == "constant-for-test", "the interner reports the hash it used");

    return runner.Report();
}
