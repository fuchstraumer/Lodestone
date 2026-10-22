#pragma once
#ifndef LODESTONE_GRAPH_RESOURCE_FLAGS_HPP
#define LODESTONE_GRAPH_RESOURCE_FLAGS_HPP
#include "EnumClassUtils.hpp"
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

/** @brief Resource creation vocabulary, independent of any WebGPU header so that it stays cheap to include. */
namespace lodestone
{

/** @brief The usages a buffer is created with, which bound every usage a pass may ask of it. */
enum class BufferUsageFlags : uint16_t
{
    Invalid = 0,
    /** @brief Readback buffers only: slow CPU-visible memory. */
    MapRead = 1 << 0,
    MapWrite = 1 << 1,
    CopySource = 1 << 2,
    CopyDestination = 1 << 3,
    Index = 1 << 4,
    Vertex = 1 << 5,
    Uniform = 1 << 6,
    Storage = 1 << 7,
    Indirect = 1 << 8,
    QueryResolve = 1 << 9,
    // WebGPU separates these two
    AllCpuAccess = MapRead | MapWrite,
    AllGpuTransfer = CopySource | CopyDestination,
    AllGeometry = Index | Vertex,
    AllShaderBinding = Uniform | Storage
};

MAKE_ENUM_CLASS_FLAGS(BufferUsageFlags);

/** @brief The usages a texture is created with. Sampled and Storage name what the shader does. */
enum class TextureUsageFlags : uint16_t
{
    Invalid = 0,
    CopySource = 1 << 0,
    CopyDestination = 1 << 1,
    Sampled = 1 << 2,
    /** @brief Raw loads and stores, with a restricted format list. */
    Storage = 1 << 3,
    RenderAttachment = 1 << 4,

    AllTransfer = CopySource | CopyDestination,
    AllShaderBinding = Sampled | Storage
};

MAKE_ENUM_CLASS_FLAGS(TextureUsageFlags);

/** @brief Usages of a buffer outside of its shader bindings, from `[ls_resource_role("...")]`. The cooker checks each
 * role against the binding kind and the element stride. */
enum class ResourceRoleFlags : uint16_t
{
    None = 0,
    /** @brief Arguments of an indirect dispatch: three `uint` values, 12 bytes. */
    IndirectDispatch = 1u << 0,
    /** @brief Arguments of an indirect draw: four `uint` values, 16 bytes. */
    IndirectDraw = 1u << 1u,
    /** @brief Arguments of an indirect indexed draw: five 4-byte values, 20 bytes. */
    IndirectDrawIndexed = 1u << 2u,
    /** @brief 32-bit indices. */
    IndexData = 1u << 3u,
    VertexData = 1u << 4u,
    All = IndirectDispatch | IndirectDraw | IndirectDrawIndexed | IndexData | VertexData,
};

MAKE_ENUM_CLASS_FLAGS(ResourceRoleFlags);

inline constexpr std::array<std::pair<std::string_view, ResourceRoleFlags>, 5u> k_ResourceRoleNames{ {
    { "IndirectDispatch", ResourceRoleFlags::IndirectDispatch },
    { "IndirectDraw", ResourceRoleFlags::IndirectDraw },
    { "IndirectDrawIndexed", ResourceRoleFlags::IndirectDrawIndexed },
    { "IndexData", ResourceRoleFlags::IndexData },
    { "VertexData", ResourceRoleFlags::VertexData },
} };

[[nodiscard]] constexpr BufferUsageFlags BufferUsageOfRoles(ResourceRoleFlags roles) noexcept
{
    constexpr static ResourceRoleFlags k_IndirectRoles = ResourceRoleFlags::IndirectDispatch |
                                                         ResourceRoleFlags::IndirectDraw |
                                                         ResourceRoleFlags::IndirectDrawIndexed;
    BufferUsageFlags usage = BufferUsageFlags::Invalid;
    if (HasAnyFlag(roles, k_IndirectRoles))
    {
        usage |= BufferUsageFlags::Indirect;
    }
    if (HasAnyFlag(roles, ResourceRoleFlags::IndexData))
    {
        usage |= BufferUsageFlags::Index;
    }
    if (HasAnyFlag(roles, ResourceRoleFlags::VertexData))
    {
        usage |= BufferUsageFlags::Vertex;
    }
    return usage;
}

/** @brief A curated format list: grow it as needed. */
enum class TextureFormat : uint8_t
{
    Invalid = 0,

    R8Unorm,
    RG8Unorm,
    RGBA8Unorm,
    RGBA8UnormSrgb,
    BGRA8Unorm,
    BGRA8UnormSrgb,

    R16Float,
    RG16Float,
    RGBA16Float,

    R32Float,
    RG32Float,
    RGBA32Float,

    R32Uint,
    RG32Uint,
    RGBA32Uint,

    R10G10B10A2Unorm,
    R11G11B10Ufloat,

    Depth16Unorm,
    Depth24Plus,
    Depth24PlusStencil8,
    Depth32Float
};

/** @brief The allocated dimension: an array texture is 2D with layers. */
enum class TextureDimension : uint8_t
{
    Invalid = 0,
    Texture1D,
    Texture2D,
    Texture3D
};

/** @brief How a binding views a texture, reflected from the shader. */
enum class TextureViewDimension : uint8_t
{
    Invalid = 0,
    View1D,
    View2D,
    View2DArray,
    ViewCube,
    ViewCubeArray,
    View3D
};

} // namespace lodestone

#endif // !LODESTONE_RESOURCE_FLAGS_HPP
