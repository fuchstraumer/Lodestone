#pragma once
#ifndef LODESTONE_CLIENT_VARIANT_KEY_HPP
#define LODESTONE_CLIENT_VARIANT_KEY_HPP
#include <cstdint>
#include <span>

namespace lodestone
{

// This is defined as an enum class since it provides strong typing and prevents accidental misuse of
// raw integers where a callsite expects a key: we'll have to cast to underlying type for packed output
// or storage, but that's fine and clear as a boundary between strong typing and low-level representation.
enum class VariantKey : uint64_t {};

// This uses radix indices to avoid any Slang or Permutation includes - but shares common logic for
// packing and unpacking the VariantKey from its underlying inputs that generate it. Used to make sure
// client and library code agree on the representation without introducing direct dependencies.
VariantKey PackVariantKey(std::span<const uint32_t> value_indices, std::span<const uint32_t> radices) noexcept;
void UnpackVariantKey(VariantKey key, std::span<const uint32_t> radices, std::span<uint32_t> out_value_indices) noexcept;

}

#endif // LODESTONE_CLIENT_VARIANT_KEY_HPP
