#pragma once
#ifndef LODESTONE_SHADER_LIBRARY_TYPES_HPP
#define LODESTONE_SHADER_LIBRARY_TYPES_HPP
#include "ResourceFlags.hpp"
#include <cstdint>
#include <span>
#include <string_view>

/**
 * @brief The vocabulary the shader cooker writes and clients read. This header is the contract between
 * Lodestone and clients that want to use the data the cooker produces. Texture formats and view dimensions
 * come from ResourceFlags.hpp. One definition serves both the resource layer and the cooker, so the two
 * cannot disagree.
 */
namespace lodestone
{

/** @brief How a shader reaches a resource. Only `Bound` is produced today. */
enum class PlacementKind : uint8_t
{
    None = 0,
    Bound,
    Indexed,
    Pointer,
};

/** @brief Type of a memory footprint field: `None` means unspecified in the shader,
 *  so users are expected to handle it. */
enum class FootprintKind : uint8_t
{
    None = 0,
    Buffer,
    Texture,
};

/** @brief The Graphics API binding type that one shader resource needs. */
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
    StorageTexture
};

enum class ShaderStageKind : uint8_t
{
    Invalid = 0,
    Vertex,
    Hull,
    Domain,
    Fragment,
    Compute,
    RayGeneration,
    Intersection,
    AnyHit,
    ClosestHit,
    Miss,
    Callable,
    Mesh,
    Amplification,
    Dispatch,
    Node,
    Count
};

/** @brief The shape of a bound resource, as the shader declares it. This should be viewed
 * as authoritative, where the CPU side only follows from this.
 * @note This is an almost exact mirror of Slang's resource shape definitions, mostly bc
 * they're better than a huge set of combined enums to represent what flags do with bitwise ops
 */
enum class ResourceShape : uint8_t
{
    Invalid = 0x00,
    Texture1D = 0x01,
    Texture2D = 0x02,
    Texture3D = 0x03,
    TextureCube = 0x04,
    // TextureBuffer was at 0x05, but it's not at all needed
    StructuredBuffer = 0x06,
    ByteAddressBuffer = 0x07,
    // 0x08 was used for unknown, but that hasn't come up
    AccelerationStructure = 0x09,
    TextureSubpass = 0x0A, // added for exact slang parity, but not needed

    // flags, not shapes
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

/** @brief How a shader samples a texture. Users can check this against
 * the formats they create or bind for validation. */
enum class TextureSampleType : uint8_t
{
    Invalid = 0,
    Float,
    UnfilterableFloat,
    Depth,
    SignedInteger,
    UnsignedInteger,
};

/** @brief The kind of accessmodel a shader uses with a resource */
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

/** @brief The scalar type of one vertex attribute or one color target.
 *  @note Widened to uint32_t so struct holding it is 16 bytes even vs 13
*/
enum class VertexScalarType : uint32_t
{
    Invalid = 0,
    Float16,
    Float32,
    SignedInteger32,
    UnsignedInteger32,
};

/** @brief The kind of a permutation axis: stored in both Lodestone runtime 
 * and reflected in the shader manifest. */
enum class AxisKind : uint8_t
{
    None,
    ResourcePresence, // Whether a resource is used (e.g, texture, buffer, etc.)
    Capability, // Whether a specific capability is required, e.g Wave or Subgroup ops
    Tuning, // Often uses a size expression: buffer sizes, wave dims, thread dims, etc
    Technique // Which technique or algorithm is used: uniform branching
};

/** @brief When a permutation axis value is made concrete and discretely bound
  * to an actual value, i.e. the granularity at which it is bound. */
enum class EarliestBindingTime : uint8_t
{
    None = 0,
    Cook, // Value is set during cook (shader uniform)
    Bind, // Value is set during pipeline bind (pipeline uniform)
    Invocation, // Value is set for a single invocation of a pipeline (draw/dispatch uniform)
    Execution, // Value is set during shader execution (per-thread, divergent)
};

/** @brief The fundamental type/domain of a permutation axis' values, and how they
  * should be interpreted or reflected. "Type" is for interface axes, but reflects
  * as the name of that type to keep things succinct. */
enum class AxisValueDomain : uint8_t
{
    None,
    Boolean,
    Integral,
    Enum,
    Type
};

/** @brief The type used to represent values of a permutation axis, particularly when stored or reflected.
  * @note Declared as a using alias in case we need to widen or otherwise change this in the future, as has
  * happened when it was mistakenly implemented as a wider signed type. */
using AxisValueType = uint32_t;

/** @brief One vertex shader input.
 *
 * WGSL keeps only `@location`. The semantic name and index live in the Slang source and in no part of
 * the emitted text, so the semantic information would be lost: we store it in cooked layout/reflection
 * data packed alongside source. */
struct VertexAttributeInfo
{
    std::string_view SemanticName;
    uint32_t SemanticIndex{ 0u };
    uint32_t Location{ 0u };
    VertexScalarType ScalarType{ VertexScalarType::Invalid };
    uint32_t ComponentCount{ 0u };
};

/** @brief One fragment shader color target.
 *
 * The format is absent, and it cannot be derived. `float4 : SV_Target0` could be Rgba8Unorm or
 * Rgba16Float. Shader reflection gives what it can, but the rest is up to the caller configuring
 * pipelines and renderpasses. */
struct ColorTargetInfo
{
    uint32_t Location{ 0u };
    VertexScalarType ScalarType{ VertexScalarType::Invalid };
    uint32_t ComponentCount{ 0u };
};

/** @brief Compute workgroup thread dimensions. The shader declares these, so they are always reflectable.
 * Clients may use these along with known input data sizes to calculate workgroup sizes exactly. */
struct WorkgroupSize
{
    uint32_t X{ 1u };
    uint32_t Y{ 1u };
    uint32_t Z{ 1u };
};

/** @brief One member of a uniform block, with the offset and the size the shader gave it.
 *  Use this to validate that CPU-size structs match the layout expected by the shader. */
struct UniformMemberInfo
{
    std::string_view Name;
    uint32_t Offset{ 0u };
    uint32_t Size{ 0u };
    uint32_t ArrayCount{ 1u };
};

/** @brief One resource a shader binds, as the generated library states it. This is the optimized and
 * compact form of the cooker's `ReflectedBinding`. Strings are stored in the cooked data, so views
 * are used here instead of owning strings. It is critical to use the group and binding indices
 * declared here to avoid errors and crashes.
 */
//NOLINTBEGIN(misc-non-private-member-variables-in-classes)
// todo-ship: maybe we strip out string_view and span, and just use C-style strings and arrays
// to avoid the standard library includes in an interface header
// todo-ship: this is already 104 bytes, we should find a way to pack it better. i was only able
// to cut 8 bytes by changing ordering to get rid of the hidden padding
struct BindingInfo
{
    std::string_view Name;
    std::string_view ScopeName;
    uint32_t Group{ static_cast<uint32_t>(-1) };
    uint32_t Binding{ static_cast<uint32_t>(-1) };
    BindingKind Kind{ BindingKind::Invalid };
    /** @brief Shape is Buffer/Texture[N]/Sampler, etc */
    ResourceShape Shape{ ResourceShape::Invalid };
    TextureSampleType SampleType{ TextureSampleType::Invalid };
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    /** @note Unlike a `Buffer`, `StorageTexture` access type is not part of the Shape value */
    ResourceAccess Access{ ResourceAccess::Invalid };
    bool IsComparisonSampler{ false };
    /** @brief Size of one structured buffer element, in bytes. Zero for a texture or a sampler. */
    uint32_t ElementStride{ 0u };
    uint32_t ArrayCount{ 1u };
    /** @brief Total size of a uniform block, in bytes. Zero for every other binding kind. */
    uint64_t ByteSize{ 0u };
    /** @brief Element count from a `[ls_element_count]` annotation, already evaluated for this
     * variant. Zero means the shader did not annotate the resource, so the caller must give a size. */
    uint64_t DerivedElementCount{ 0u };
    /** @brief Texture extent from a `[ls_extent_2d]` or `[ls_extent_3d]` annotation. Zero width means
     * the shader did not annotate the resource. This means the caller must drive and set the sizing.*/
    uint32_t DerivedExtentX{ 0u };
    uint32_t DerivedExtentY{ 0u };
    uint32_t DerivedExtentZ{ 0u };

    /** @brief The members of a uniform block. Empty for every other binding kind. */
    std::span<const UniformMemberInfo> Members;
};
//NOLINTEND(misc-non-private-member-variables-in-classes)

} // namespace lodestone

#endif // !LODESTONE_SHADER_LIBRARY_TYPES_HPP
