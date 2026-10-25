#pragma once
#ifndef LODESTONE_SHADER_LIBRARY_TYPES_HPP
#define LODESTONE_SHADER_LIBRARY_TYPES_HPP
#include "EnumClassUtils.hpp"
#include <cstdint>
#include <string_view>

/** @brief The vocabulary the cooker writes and clients read: the contract between Lodestone and its clients. */
namespace lodestone
{

enum class MatrixLayout : uint8_t
{
    Invalid = 0,
    RowMajor,
    ColumnMajor,
};

/** @brief How a shader reaches a resource. Only `Bound` is produced today. */
enum class PlacementKind : uint8_t
{
    None = 0,
    Bound,
    Handle,
    Pointer,
};

/** @brief The code form of a profile. Each SPIR-V source is whole 32-bit words, aligned to 4 bytes. */
enum class ShaderCodeFormat : uint8_t
{
    None = 0,
    Wgsl,
    Spirv,
};

/** @brief `None` means the shader declares no size, and the caller supplies it. */
enum class FootprintKind : uint8_t
{
    None = 0,
    Buffer,
    Texture,
};

enum class BindingKind : uint8_t
{
    Invalid = 0,
    Sampler,
    Texture,
    UniformBuffer,
    ParameterBlock,
    StorageBuffer,
    TexelBuffer,
    CombinedTextureSampler,
    InputRenderTarget,
    InlineUniform,
    RayTracingAccelerationStructure,
    StorageTexture,
    /** Reserved: no reflection produces this yet. */
    PushConstant
};

/** @brief How a target delivers entry point constants: a uniform buffer with a placement (WGSL), or a push constant
 * block with none (SPIR-V). */
enum class EntryPointConstantsDelivery : uint8_t
{
    None = 0,
    UniformBuffer,
    PushConstant,
};

enum class ShaderStageKind : uint32_t
{
    Invalid = 0u,
    Vertex = 1u << 0u,
    Hull = 1u << 1u,
    Domain = 1u << 2u,
    Fragment = 1u << 3u,
    Compute = 1u << 4u,
    RayGeneration = 1u << 5u,
    Intersection = 1u << 6u,
    AnyHit = 1u << 7u,
    ClosestHit = 1u << 8u,
    Miss = 1u << 9u,
    Callable = 1u << 10u,
    Mesh = 1u << 11u,
    Amplification = 1u << 12u,
    Dispatch = 1u << 13u,
    Node = 1u << 14u,
    // so old I forgot them, so they go at the end (god i feel old)
    TessellationControl = 1u << 15u,
    TessellationEvaluation = 1u << 16u,
    Geometry = 1u << 17u,
    Task = 1u << 18u,
    Count = Task + 1u, // just enough to compare against numerically (e.g, stage >= Count stuff)
};

MAKE_ENUM_CLASS_FLAGS(ShaderStageKind)

/** @brief The declared shape of a resource, mirroring Slang's resource shape: a base shape plus flags. */
enum class ResourceShape : uint8_t
{
    Invalid = 0x00,
    Texture1D = 0x01,
    Texture2D = 0x02,
    Texture3D = 0x03,
    TextureCube = 0x04,
    StructuredBuffer = 0x06,
    ByteAddressBuffer = 0x07,
    AccelerationStructure = 0x09,
    TextureSubpass = 0x0A, // added for exact slang parity, but not needed

    BaseShapeMask = 0x0F,
    FeedbackFlag = 0x10,
    ShadowFlag = 0x20,
    ArrayFlag = 0x40,
    MultisampleFlag = 0x80
};

MAKE_ENUM_CLASS_FLAGS(ResourceShape)

constexpr ResourceShape GetBaseShape(ResourceShape shape)
{
    return shape & ResourceShape::BaseShapeMask;
}

constexpr bool ResourceShapeIsArray(ResourceShape shape)
{
    return HasAnyFlag(shape, ResourceShape::ArrayFlag);
}

constexpr bool ResourceShapeIsMultisample(ResourceShape shape)
{
    return HasAnyFlag(shape, ResourceShape::MultisampleFlag);
}

constexpr bool ResourceShapeIsShadow(ResourceShape shape)
{
    return HasAnyFlag(shape, ResourceShape::ShadowFlag);
}

/** @brief How a shader samples a texture, to check against the formats a client creates. */
enum class TextureSampleType : uint8_t
{
    Invalid = 0,
    Float,
    UnfilterableFloat,
    Depth,
    SignedInteger,
    UnsignedInteger,
};

enum class ResourceAccess : uint8_t // increased to 16 for alignment
{
    Invalid = 0,
    ReadOnly,
    WriteOnly,
    ReadWrite,
    RasterizerOrdered, // this will require VK_EXT_fragment_shader_interlock for SPIRV targets
    Append,
    Consume,
    Feedback
};

/** @brief Widened to 32 bits, so that the records holding it are 16 bytes. */
enum class VertexScalarType : uint32_t
{
    Invalid = 0,
    Float16,
    Float32,
    SignedInteger32,
    UnsignedInteger32,
};

enum class AxisKind : uint8_t
{
    None,
    ResourcePresence, // Whether a resource is used (e.g, texture, buffer, etc.)
    Capability, // Whether a specific capability is required, e.g Wave or Subgroup ops
    Tuning, // Often uses a size expression: buffer sizes, wave dims, thread dims, etc
    Technique, // Which technique or algorithm is used: uniform branching
};

/** @brief The earliest time an axis value can be bound. */
enum class EarliestBindingTime : uint8_t
{
    None = 0,
    Cook, // Value is set during cook (shader uniform)
    Bind, // Value is set during pipeline bind (pipeline uniform)
    Invocation, // Value is set for a single invocation of a pipeline (draw/dispatch uniform)
    Execution, // Value is set during shader execution (per-thread, divergent)
};

/** @brief The domain of an axis's values. `Type` is an interface axis, stored as the implementation name. */
enum class AxisValueDomain : uint8_t
{
    None,
    Boolean,
    Integral,
    Enum,
    Type
};

/** @brief An alias, because this was once a wider signed type by mistake. */
using AxisValueType = uint32_t;

/** @brief One vertex input. WGSL keeps only `@location`, so the semantic exists only in this record. */
struct VertexAttributeInfo
{
    std::string_view SemanticName;
    uint32_t SemanticIndex{ 0u };
    uint32_t Location{ 0u };
    VertexScalarType ScalarType{ VertexScalarType::Invalid };
    uint32_t ComponentCount{ 0u };
};

/** @brief One color target. The format cannot be derived: `float4 : SV_Target0` could be Rgba8Unorm or Rgba16Float. */
struct ColorTargetInfo
{
    uint32_t Location{ 0u };
    VertexScalarType ScalarType{ VertexScalarType::Invalid };
    uint32_t ComponentCount{ 0u };
};

struct WorkgroupSize
{
    uint32_t X{ 1u };
    uint32_t Y{ 1u };
    uint32_t Z{ 1u };
};

} // namespace lodestone

#endif // !LODESTONE_SHADER_LIBRARY_TYPES_HPP
