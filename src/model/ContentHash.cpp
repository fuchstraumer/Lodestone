#include "model/ContentHash.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <xxhash.h>

namespace lodestone
{

    ContentHashValue HashBytes(std::span<const std::byte> bytes) noexcept
    {
        return XXH3_64bits(bytes.data(), bytes.size());
    }

    StreamingHash::StreamingHash()
    {
        hashState = XXH3_createState();
        XXH3_64bits_reset(hashState);
    }

    StreamingHash::~StreamingHash()
    {
        assert(hashState != nullptr);
        XXH3_freeState(hashState);
    }

    void StreamingHash::Append(std::span<const std::byte> values) noexcept
    {
        XXH3_64bits_update(hashState, values.data(), values.size());
    }

    void StreamingHash::Append(std::string_view bytes) noexcept
    {
        XXH3_64bits_update(hashState, bytes.data(), bytes.size());
    }

    void StreamingHash::Append(uint64_t value) noexcept
    {
        XXH3_64bits_update(hashState, &value, sizeof(value));
    }

    void StreamingHash::Append(int64_t value) noexcept
    {
        XXH3_64bits_update(hashState, &value, sizeof(value));
    }

    void StreamingHash::Append(uint32_t value) noexcept
    {
        XXH3_64bits_update(hashState, &value, sizeof(value));
    }

    void StreamingHash::Append(int32_t value) noexcept
    {
        XXH3_64bits_update(hashState, &value, sizeof(value));
    }

    void StreamingHash::Append(std::span<const uint64_t> values) noexcept
    {
        XXH3_64bits_update(hashState, values.data(), values.size() * sizeof(uint64_t));
    }

    void StreamingHash::Append(std::span<const int64_t> values) noexcept
    {
        XXH3_64bits_update(hashState, values.data(), values.size() * sizeof(int64_t));
    }

    void StreamingHash::Append(std::span<const uint32_t> values) noexcept
    {
        XXH3_64bits_update(hashState, values.data(), values.size() * sizeof(uint32_t));
    }

    void StreamingHash::Append(std::span<const int32_t> values) noexcept
    {
        XXH3_64bits_update(hashState, values.data(), values.size() * sizeof(int32_t));
    }

    void StreamingHash::Reset() noexcept
    {
        XXH3_64bits_reset(hashState);
    }

    ContentHashValue StreamingHash::Finalize() const noexcept
    {
        return XXH3_64bits_digest(hashState);
    }

} // namespace lodestone
