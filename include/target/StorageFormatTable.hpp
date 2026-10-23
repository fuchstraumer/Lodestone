#pragma once
#ifndef LODESTONE_STORAGE_FORMAT_TABLE_HPP
#define LODESTONE_STORAGE_FORMAT_TABLE_HPP
#include "ResourceFlags.hpp"
#include <span>
#include <string_view>

namespace lodestone
{

/** @brief One storage texture format that every target accepts. The case names are the ones of `StorageFormat` in
 * `builtins/LodestoneShims.slang`, and `StorageFormatTest` compares the two. */
struct StorageFormatInfo
{
    std::string_view EnumCase;
    /** @brief The Slang type that names the format as a texture type argument, such as `RGBA16F`. */
    std::string_view SlangType;
    TextureFormat Format{ TextureFormat::Invalid };
    std::string_view WgslName;
};

/** @brief In the order of the cases of `StorageFormat`. */
std::span<const StorageFormatInfo> GetStorageFormats() noexcept;

/** @brief Null when the case name is not in the table. */
const StorageFormatInfo* FindStorageFormat(std::string_view enum_case) noexcept;

} // namespace lodestone

#endif // !LODESTONE_STORAGE_FORMAT_TABLE_HPP
