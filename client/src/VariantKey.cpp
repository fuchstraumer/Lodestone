#include "VariantKey.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <ranges>
#include <utility>

namespace lodestone
{

VariantKey PackVariantKey(std::span<const uint32_t> value_indices, std::span<const uint32_t> radices) noexcept
{
    assert(value_indices.size() == radices.size());
    uint64_t result{ 0u };
    for (auto&& [value, radix] : std::views::zip(value_indices, radices))
    {
        // only cast the mul, adding different widths is fine
        result = (result * static_cast<uint64_t>(radix)) + value;
    }
    return static_cast<VariantKey>(result);
}

void UnpackVariantKey(VariantKey key, std::span<const uint32_t> radices, std::span<uint32_t> out_value_indices) noexcept
{
    uint64_t value = std::to_underlying(key);
    for (size_t i = radices.size(); i-- > 0;)
    {
        out_value_indices[i] = static_cast<uint32_t>(value % radices[i]);
        value /= radices[i];
    }
}

}
