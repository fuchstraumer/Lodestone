#include "target/StorageFormatTable.hpp"
#include <algorithm>
#include <array>

namespace lodestone
{

namespace
{
    constexpr std::array<StorageFormatInfo, 6u> k_StorageFormats
    {
        StorageFormatInfo{ .EnumCase = "Rgba8Unorm", .SlangType = "RGBA8Unorm", .Format = TextureFormat::RGBA8Unorm, .WgslName = "rgba8unorm" },
        StorageFormatInfo{ .EnumCase = "Rgba8Snorm", .SlangType = "RGBA8Snorm", .Format = TextureFormat::RGBA8Snorm, .WgslName = "rgba8snorm" },
        StorageFormatInfo{ .EnumCase = "Rgba16Float", .SlangType = "RGBA16F", .Format = TextureFormat::RGBA16Float, .WgslName = "rgba16float" },
        StorageFormatInfo{ .EnumCase = "R32Float", .SlangType = "R32F", .Format = TextureFormat::R32Float, .WgslName = "r32float" },
        StorageFormatInfo{ .EnumCase = "Rg32Float", .SlangType = "RG32F", .Format = TextureFormat::RG32Float, .WgslName = "rg32float" },
        StorageFormatInfo{ .EnumCase = "Rgba32Float", .SlangType = "RGBA32F", .Format = TextureFormat::RGBA32Float, .WgslName = "rgba32float" },
    };
}

std::span<const StorageFormatInfo> GetStorageFormats() noexcept
{
    return k_StorageFormats;
}

const StorageFormatInfo* FindStorageFormat(std::string_view enum_case) noexcept
{
    const auto found = std::ranges::find(k_StorageFormats, enum_case, &StorageFormatInfo::EnumCase);
    return found != k_StorageFormats.end() ? &*found : nullptr;
}

} // namespace lodestone
