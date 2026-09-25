#pragma once
#ifndef LODESTONE_TRANSPARENT_STRING_HASH_HPP
#define LODESTONE_TRANSPARENT_STRING_HASH_HPP
#include <functional>
#include <string_view>
#include <span>

namespace lodestone
{

// required to enable copy-free hashing of string_views when doing lookups in a map
struct TransparentStringHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

struct TransparentVectorByteHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(const std::span<const std::byte>& data) const noexcept
    {
        std::string_view view{reinterpret_cast<const char*>(data.data()), data.size()};
        return std::hash<std::string_view>{}(view);
    }
};

}

#endif // !LODESTONE_TRANSPARENT_STRING_HASH_HPP
