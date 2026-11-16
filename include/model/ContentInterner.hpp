#pragma once
#ifndef LODESTONE_CONTENT_INTERNER_HPP
#define LODESTONE_CONTENT_INTERNER_HPP
#include "ContentHash.hpp"
#include "containers/DenseHashMap.hpp"
#include <concepts>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
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
        firstEntryByHash.reserve(256);
        nextEntryWithSameHash.reserve(1024);
    }

    void Disable() noexcept
    {
        dedupeEnabled = false;
    }

    [[nodiscard]] bool IsEnabled() const noexcept
    {
        return dedupeEnabled;
    }

    template<typename T> requires std::same_as<std::remove_cvref_t<T>, PayloadType>
    InternResult Intern(T&& payload, ProvenanceRecord origin)
    {
        ++statistics.ArtifactsSeen;

        if (!dedupeEnabled)
        {
            return append(std::forward<T>(payload), std::move(origin), k_NoEntry);
        }

        const ContentHashValue hash = hashFunction(payload);
        const auto head = firstEntryByHash.find(hash);
        int32_t firstCandidate = (head != firstEntryByHash.end()) ? head->second : k_NoEntry;

        for (int32_t candidate = firstCandidate; candidate != k_NoEntry; candidate = nextEntryWithSameHash[candidate])
        {
            ++statistics.ByteComparisons;
            if (uniqueEntries[candidate] == payload)
            {
                origins[candidate].emplace_back(std::move(origin));
                return InternResult{ static_cast<uint32_t>(candidate), false };
            }

            // same bucket, different bytes: both entries stay
            ++statistics.HashCollisions;
        }

        const InternResult appended = append(std::forward<T>(payload), std::move(origin), firstCandidate);
        firstEntryByHash.insert_or_assign(hash, static_cast<int32_t>(appended.Index));
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
        assert(index < origins.size());
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

    template<typename T, typename P> requires std::same_as<std::remove_cvref_t<T>, PayloadType>
    InternResult append(T&& payload, P&& origin, int32_t next_entry)
    {
        const uint32_t index = static_cast<uint32_t>(uniqueEntries.size());
        uniqueEntries.emplace_back(std::forward<T>(payload));
        origins.emplace_back(std::vector<ProvenanceRecord>{ std::forward<P>(origin) });
        nextEntryWithSameHash.emplace_back(next_entry);
        statistics.UniqueEntries = static_cast<uint32_t>(uniqueEntries.size());
        return InternResult{ index, true };
    }

    HashFunction hashFunction;
    std::string_view hashName;
    bool dedupeEnabled{ true };
    constexpr static int32_t k_NoEntry{ -1 };
    foundation::DenseHashMap<ContentHashValue, int32_t> firstEntryByHash;
    std::vector<int32_t> nextEntryWithSameHash;
    std::vector<PayloadType> uniqueEntries;
    std::vector<std::vector<ProvenanceRecord>> origins;
    InternerStatistics statistics;
};

} // namespace lodestone

#endif // !LODESTONE_CONTENT_INTERNER_HPP
