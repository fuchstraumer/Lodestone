#pragma once
#ifndef LODESTONE_CLIENT_VARIANT_KEY_HPP
#define LODESTONE_CLIENT_VARIANT_KEY_HPP
#include <cstdint>
#include <span>

namespace lodestone
{

enum class VariantKey : uint64_t {};
constexpr static VariantKey INVALID_VARIANT = static_cast<VariantKey>(0xFFFFFFFFFFFFFFFF);
constexpr bool operator!(VariantKey key) noexcept
{
    return key == INVALID_VARIANT;
}

// the codec shared by the cooker and the client
VariantKey PackVariantKey(std::span<const uint32_t> value_indices, std::span<const uint32_t> radices) noexcept;
void UnpackVariantKey(VariantKey key, std::span<const uint32_t> radices, std::span<uint32_t> out_value_indices) noexcept;

}

#endif // LODESTONE_CLIENT_VARIANT_KEY_HPP
