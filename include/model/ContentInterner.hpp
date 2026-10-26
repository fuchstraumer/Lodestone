#pragma once
#ifndef LODESTONE_CONTENT_INTERNER_HPP
#define LODESTONE_CONTENT_INTERNER_HPP
#include "ContentHash.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lodestone
{

struct ProvenanceRecord
{
    std::string EntryPointName;
    std::string VariantDescription;
    uint64_t VariantIndex{ 0u };
};

struct InternResult
{
    uint32_t Index{ 0u };
    bool WasNew{ false };
};

struct InternerStatistics
{
    uint32_t ArtifactsSeen{ 0u };
    uint32_t UniqueEntries{ 0u };
    uint32_t HashCollisions{ 0u };
    /** A rising count means a worse hash, not a bug. */
    uint32_t ByteComparisons{ 0u };
};

/** @brief A hash table with provenance: it collapses equal artifacts onto one entry and records every input that mapped
 * there. Hashes only find buckets and bytes decide. `Disable()` gives every artifact its own entry, and the output
 * must stay correct in both modes.
 *
 * @tparam PayloadType The type of the artifacts being interned. Must support equality comparison and move semantics. */
template<typename PayloadType>
class ContentInterner final
{
public:
    using HashFunction = ContentHashValue (*)(const PayloadType&) noexcept;

    ContentInterner(HashFunction hash_function, std::string_view hash_name) noexcept
        : hashFunction{ hash_function },
          hashName{ hash_name }
    {
        uniqueEntries.reserve(1024);
        origins.reserve(1024);
        buckets.reserve(256);
    }

    void Disable() noexcept
    {
        dedupeEnabled = false;
    }

    [[nodiscard]] bool IsEnabled() const noexcept
    {
        return dedupeEnabled;
    }

    /** @brief take that payload and shove it somewhere else (intern. get it) */
    InternResult Intern(PayloadType payload, ProvenanceRecord origin)
    {
        ++statistics.ArtifactsSeen;

        if (!dedupeEnabled)
        {
            return append(std::move(payload), std::move(origin));
        }

        const ContentHashValue hash = hashFunction(payload);
        std::vector<uint32_t>& bucket = buckets[hash];

        for (uint32_t candidate : bucket)
        {
            ++statistics.ByteComparisons;
            if (uniqueEntries[candidate] == payload)
            {
                origins[candidate].emplace_back(std::move(origin));
                return InternResult{ candidate, false };
            }

            // same bucket, different bytes: both entries stay
            ++statistics.HashCollisions;
        }

        const InternResult appended = append(std::move(payload), std::move(origin));
        bucket.push_back(appended.Index);
        return appended;
    }

    [[nodiscard]] std::span<const PayloadType> UniqueEntries() const noexcept
    {
        return uniqueEntries;
    }

    /** @brief Moves the payload table out, for the freeze, to avoid copies of what can be a huge table of data */
    [[nodiscard]] std::vector<PayloadType> ConsumeTable() noexcept
    {
        return std::move(uniqueEntries);
    }

    [[nodiscard]] std::span<const ProvenanceRecord> OriginsOf(uint32_t index) const noexcept
    {
        if (index >= origins.size())
        {
            return {};
        }

        return origins[index];
    }

    [[nodiscard]] const InternerStatistics& Statistics() const noexcept
    {
        return statistics;
    }

    // @brief Used to make sure that the hash hasn't changed between versions of the interner/manifest
    [[nodiscard]] std::string_view HashName() const noexcept
    {
        return hashName;
    }

private:

    InternResult append(PayloadType&& payload, ProvenanceRecord&& origin)
    {
        const uint32_t index = static_cast<uint32_t>(uniqueEntries.size());
        uniqueEntries.emplace_back(std::move(payload));
        origins.emplace_back(std::vector<ProvenanceRecord>{ std::move(origin) });
        statistics.UniqueEntries = static_cast<uint32_t>(uniqueEntries.size());
        return InternResult{ index, true };
    }

    HashFunction hashFunction;
    std::string_view hashName;
    bool dedupeEnabled{ true };
    std::unordered_map<ContentHashValue, std::vector<uint32_t>> buckets;
    std::vector<PayloadType> uniqueEntries;
    std::vector<std::vector<ProvenanceRecord>> origins;
    InternerStatistics statistics;
};

} // namespace lodestone

#endif // !LODESTONE_CONTENT_INTERNER_HPP
