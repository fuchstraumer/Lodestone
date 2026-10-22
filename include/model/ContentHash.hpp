#pragma once
#ifndef LODESTONE_CONTENT_HASH_HPP
#define LODESTONE_CONTENT_HASH_HPP
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

struct XXH3_state_s;

/** The hash the interner uses to find candidates. It never decides equality. The name reaches the output, so take
 * it from `k_HashName` only.
 * todo-ship: measure this on mobile */
namespace lodestone
{

using ContentHashValue = uint64_t;
inline constexpr std::string_view k_HashName{ "xxHash3_64" };
ContentHashValue HashBytes(std::span<const std::byte> values) noexcept;

/** @brief RAII over xxHash3's streaming API: append values, then finalize. */
struct StreamingHash
{
    StreamingHash();
    ~StreamingHash();
    StreamingHash(const StreamingHash&) = delete;
    StreamingHash& operator=(const StreamingHash&) = delete;
    StreamingHash(StreamingHash&&) = delete;
    StreamingHash& operator=(StreamingHash&&) = delete;
    // templates would be easier, and header bloat. booooo
    void Append(std::span<const std::byte> values) noexcept;
    void Append(std::string_view bytes) noexcept;
    void Append(uint64_t value) noexcept;
    void Append(int64_t value) noexcept;
    void Append(uint32_t value) noexcept;
    void Append(int32_t value) noexcept;
    void Append(std::span<const uint64_t> values) noexcept;
    void Append(std::span<const int64_t> values) noexcept;
    void Append(std::span<const uint32_t> values) noexcept;
    void Append(std::span<const int32_t> values) noexcept;
    // memsets only, so the thread-local state skips the aligned alloc
    void Reset() noexcept;
    ContentHashValue Finalize() const noexcept;
private:
    XXH3_state_s* hashState{ nullptr };
};

} // namespace lodestone

#endif // !LODESTONE_CONTENT_HASH_HPP
