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
    ReadOnlyStructuredBuffer,
    ReadOnlyStorageBuffer,
    CombinedTextureSampler,
    InputRenderTarget,
    InlineUniform,
    RayTracingAccelerationStructure,
    StructuredBuffer,
    StorageBuffer,
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
    Node
};

/** @brief The shape of a bound resource, as the shader declares it. This should be viewed
 * as authoritative, where the CPU side only follows from this. */
enum class ResourceShape : uint8_t
{
    Invalid = 0,
    Buffer,
    Texture1D,
    Texture2D,
    Texture2DArray,
    Texture3D,
    TextureCube,
    TextureCubeArray,
    Texture2DMultisample,
};

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

/** @brief What a shader does to a storage texture. Stored as separate flags from
 * from a shared accessor since Buffers are either RW or read-only */
enum class StorageTextureAccess : uint8_t
{
    Invalid = 0,
    ReadOnly,
    WriteOnly,
    ReadWrite,
};

enum class SamplerBindingType : uint8_t
{
    Invalid = 0,
    Filtering,
    NonFiltering,
    Comparison,
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
struct BindingInfo
{
    std::string_view Name;
    std::string_view ScopeName;
    uint32_t Group{ static_cast<uint32_t>(-1) };
    uint32_t Binding{ static_cast<uint32_t>(-1) };
    BindingKind Kind{ BindingKind::Invalid };

    /** @brief Size of one structured buffer element, in bytes. Zero for a texture or a sampler. */
    uint32_t ElementStride{ 0u };
    /** @brief Total size of a uniform block, in bytes. Zero for every other binding kind. */
    uint64_t ByteSize{ 0u };
    uint32_t ArrayCount{ 1u };
    /** @brief Shape is Buffer/Texture[N]/Sampler, etc */
    ResourceShape Shape{ ResourceShape::Invalid };
    TextureSampleType SampleType{ TextureSampleType::Invalid };
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    /** @note Unlike a `Buffer`, `StorageTexture` access type is not part of the Shape value */
    StorageTextureAccess StorageAccess{ StorageTextureAccess::Invalid };
    SamplerBindingType SamplerType{ SamplerBindingType::Invalid };

    /** @brief Element count from a `[vx_element_count]` annotation, already evaluated for this
     * variant. Zero means the shader did not annotate the resource, so the caller must give a size. */
    uint64_t DerivedElementCount{ 0u };
    /** @brief Texture extent from a `[vx_extent_2d]` or `[vx_extent_3d]` annotation. Zero width means
     * the shader did not annotate the resource. This means the caller must drive and set the sizing.*/
    uint32_t DerivedExtentX{ 0u };
    uint32_t DerivedExtentY{ 0u };
    uint32_t DerivedExtentZ{ 0u };

    /** @brief The members of a uniform block. Empty for every other binding kind. */
    std::span<const UniformMemberInfo> Members;

    /** @brief Byte size the graph must create, or zero when the shader states no element count. */
    [[nodiscard]] uint64_t DerivedByteSize() const noexcept;
    /** @brief Validate the resource binding, ensuring it is correctly configured. */
    [[nodiscard]] bool Validate() const noexcept;
};
//NOLINTEND(misc-non-private-member-variables-in-classes)

/** @brief Finds one uniform block member by name. A missing name returns nullptr, and that must be an
 * error: it means the CPU side names a field the shader does not have. */
[[nodiscard]] const UniformMemberInfo* FindUniformMember(std::span<const UniformMemberInfo> members,
                                                         std::string_view name) noexcept;

/**
 * @brief Where the rendergraph gets shader sources and layouts. This is a virtual base class
 * so that we can choose to use a provider reading from baked source - or a provider that reads
 * from memory and can provide live-edit functionality. Clients binding and using shaders
 * and their reflection data should not care where it comes from.
 *
 * `Generation()` is the future hot-reload hook. A provider for baked data will always return the same value,
 * but a live provider can increment the value when any source changes - allowing users to reload
 * shaders and reset state gracefully
 *
 * todo-ship: Find a better approach for EntryPointId than setting the 0th value to 1. That's brittle
 * and breaks assumptions about indexing from zero.
 */
class ShaderSourceProvider
{
public:
    ShaderSourceProvider() noexcept;
    virtual ~ShaderSourceProvider() noexcept;
    ShaderSourceProvider(const ShaderSourceProvider&) = delete;
    ShaderSourceProvider& operator=(const ShaderSourceProvider&) = delete;

    /** @brief WGSL for one entry point of one variant. An unknown pair returns an empty view. */
    [[nodiscard]] virtual std::string_view Source(uint16_t entry_point,
                                                  uint32_t variant_index) const noexcept = 0;
    [[nodiscard]] virtual std::span<const BindingInfo> Bindings(uint16_t entry_point,
                                                                uint32_t variant_index) const noexcept = 0;
    [[nodiscard]] virtual WorkgroupSize Workgroup(uint16_t entry_point,
                                                  uint32_t variant_index) const noexcept = 0;
    /** @brief Increments when any source above changes. A constant means sources never change. */
    [[nodiscard]] virtual uint64_t Generation() const noexcept = 0;
};

/** @brief Finds one binding by the name the shader gave it.
 *
 * Compile() uses this to turn a declared binding name into a group and a binding index. A missing
 * name returns nullptr, and that must be an error naming both the shader and the declaration. A
 * silent default here would bind the wrong resource. */
[[nodiscard]] const BindingInfo* FindBindingByName(std::span<const BindingInfo> bindings,
                                                   std::string_view name) noexcept;

[[nodiscard]] bool IsBufferBinding(BindingKind kind) noexcept;
[[nodiscard]] bool IsTextureBinding(BindingKind kind) noexcept;

} // namespace lodestone

#endif // !LODESTONE_SHADER_LIBRARY_TYPES_HPP
