#pragma once
#ifndef LODESTONE_DATA_SCHEMA_HPP
#define LODESTONE_DATA_SCHEMA_HPP
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/** The cooker's schema. No Slang, WebGPU, or filesystem type may appear here.
 * todo-ship: refactor this into a true C ABI */
namespace lodestone
{

std::string_view ToString(BindingKind kind) noexcept;
std::string_view ToString(ShaderStageKind stage) noexcept;
std::string_view ToString(ResourceShape shape) noexcept;
std::string_view ToString(TextureSampleType sample_type) noexcept;

struct BoundPlacement
{
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };

    friend bool operator==(const BoundPlacement&, const BoundPlacement&) = default;
};

/** @brief A resource that a shader reaches through a value stored in a block: a descriptor handle (`Handle`) or a buffer
 * device address (`Pointer`). `Block` is the uniform buffer that stores it, and is unused for push constants. */
struct StoredValuePlacement
{
    PlacementKind Kind{ PlacementKind::None };
    StoredValueContainer Container{ StoredValueContainer::None };
    BoundPlacement Block;
    uint32_t ByteOffset{ 0u };

    friend bool operator==(const StoredValuePlacement&, const StoredValuePlacement&) = default;
};

/** @brief `std::monostate` means the target reported no placement, which differs from group 0, binding 0. */
using ResourcePlacement = std::variant<std::monostate, BoundPlacement, StoredValuePlacement>;

const BoundPlacement* GetBoundPlacement(const ResourcePlacement& placement) noexcept;
const StoredValuePlacement* GetStoredValuePlacement(const ResourcePlacement& placement) noexcept;

/** @brief Bound placements sort first, by group and binding, then stored values by container, block, and offset, and
 * resources with no placement last. */
bool PlacementLess(const ResourcePlacement& lhs, const ResourcePlacement& rhs) noexcept;

struct BufferFootprint
{
    uint64_t ElementCount{ 0u };
    std::string Expression;

    friend bool operator==(const BufferFootprint&, const BufferFootprint&) = default;
};

/** @brief A texture extent in texels. An axis the attribute does not state is 1. */
struct TextureFootprint
{
    uint32_t ExtentX{ 1u };
    uint32_t ExtentY{ 1u };
    uint32_t ExtentZ{ 1u };
    std::string Expression;

    friend bool operator==(const TextureFootprint&, const TextureFootprint&) = default;
};

using ResourceFootprint = std::variant<std::monostate, BufferFootprint, TextureFootprint>;

struct ReflectedUniformMember
{
    std::string Name;
    // hashed as one block of memory
    struct Packed
    {
        uint32_t Offset{ 0u };
        uint32_t Size{ 0u };
        uint32_t ArrayCount{ 1u };
        uint16_t ElementStride{ 0u };
        MatrixLayout MatrixLayout{ 0u };
        uint8_t Padding{ 0u }; // make this safe to hash by explicitly adding+setting padding
        friend bool operator==(const Packed&, const Packed&) = default;
    } Data;

    friend bool operator==(const ReflectedUniformMember&, const ReflectedUniformMember&) = default;
};

/** @brief What a shader states about one resource. The CPU side never writes any of it. `StorageFormat` is set only for
 * storage textures, where the shader spells the format. */
struct ReflectedBinding
{
    std::string Name;
    std::string ScopeName;
    ResourcePlacement Placement;
    BindingKind Kind{ BindingKind::Invalid };

    uint32_t ElementStride{ 0u };
    uint64_t ByteSize{ 0u };
    // 0 for an unbounded binding array
    uint32_t ArrayCount{ 1u };

    ResourceShape Shape{ ResourceShape::Invalid };
    TextureSampleType SampleType{ TextureSampleType::Invalid };
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    ResourceAccess Access{ ResourceAccess::Invalid };
    bool IsComparisonSampler{ false };

    /** Filled for uniform blocks and structured buffers. */
    std::vector<ReflectedUniformMember> UniformMembers;

    ResourceRoleFlags Roles{ ResourceRoleFlags::None };
    /** Sorted and unique. */
    std::vector<std::string> Tags;

    friend bool operator==(const ReflectedBinding&, const ReflectedBinding&) = default;
};

/** @brief Zero when the resource has no bound placement. */
uint32_t GroupOf(const ReflectedBinding& binding) noexcept;
/** @brief Zero when the resource has no bound placement. */
uint32_t BindingOf(const ReflectedBinding& binding) noexcept;

struct ResolvedBinding
{
    ReflectedBinding Resource;
    ResourceFootprint Footprint;

    friend bool operator==(const ResolvedBinding&, const ResolvedBinding&) = default;
};

struct ResolvedBindingView
{
    const ReflectedBinding* Resource{ nullptr };
    const ResourceFootprint* Footprint{ nullptr };
    // unannotated resources have no footprint
    // todo-ship: shouldn't we just intern that still lol
    constexpr bool operator==(const ResolvedBindingView& rhs) const noexcept
    {
        const bool resourcesAgree =
            (Resource == rhs.Resource) ||
            (Resource != nullptr && rhs.Resource != nullptr && *Resource == *rhs.Resource);
        const bool footprintsAgree =
            (Footprint == rhs.Footprint) ||
            (Footprint != nullptr && rhs.Footprint != nullptr && *Footprint == *rhs.Footprint);

        return resourcesAgree && footprintsAgree;
    }
    constexpr bool operator!=(const ResolvedBindingView& rhs) const noexcept
    {
        return !(*this == rhs);
    }
};

std::string_view ToString(VertexScalarType scalar_type) noexcept;

struct ReflectedVertexInput
{
    std::string SemanticName;
    struct Packed
    {
        uint32_t SemanticIndex{ 0u };
        uint32_t Location{ 0u };
        VertexScalarType ScalarType{ VertexScalarType::Invalid };
        uint32_t ComponentCount{ 0u };
        friend bool operator==(const Packed&, const Packed&) = default;
    } Data;

    friend bool operator==(const ReflectedVertexInput&, const ReflectedVertexInput&) = default;
};

/** @brief The format is the client's choice: do not assume one from the scalar type and component count. */
struct alignas(16) ReflectedColorTarget
{
    uint32_t Location{ 0u };
    VertexScalarType ScalarType{ VertexScalarType::Invalid };
    uint32_t ComponentCount{ 0u };
    uint32_t Padding{ 0u };

    friend bool operator==(const ReflectedColorTarget&, const ReflectedColorTarget&) = default;
};

/** @brief The raster facts a shader states. Clients declare the rest of their raster state. */
struct ReflectedRasterState
{
    std::vector<ReflectedVertexInput> VertexInputs;
    std::vector<ReflectedColorTarget> ColorTargets;
    /** Detection is not flawless, so render state code should still drive it. */
    bool WritesFragDepth{ false };

    friend bool operator==(const ReflectedRasterState&, const ReflectedRasterState&) = default;
};

 
/** @brief The entry point constants in one target's layout. `Binding` is a position in `UsedBindingIndices`, for the
 * `UniformBuffer` delivery only. */
struct EntryPointConstants
{
    EntryPointConstantsDelivery Delivery{ EntryPointConstantsDelivery::None };
    uint32_t ByteSize{ 0u };
    uint32_t Binding{ 0u };
    std::vector<ReflectedUniformMember> Members;

    friend bool operator==(const EntryPointConstants&, const EntryPointConstants&) = default;
};

struct EntryPointReflection
{
    std::string Name;
    ShaderStageKind Stage{ ShaderStageKind::Invalid };
    WorkgroupSize Workgroup;
    std::vector<uint32_t> UsedBindingIndices;
    ReflectedRasterState Raster;
    EntryPointConstants Constants;
};

std::string DescribeRasterState(const ReflectedRasterState& raster);

/** @brief A texture and its sampler, as indices into `UsedBindingIndices`. Only the emitted code can tell. */
struct SamplerTexturePair
{
    uint32_t Texture{ 0u };
    uint32_t Sampler{ 0u };

    friend bool operator==(const SamplerTexturePair&, const SamplerTexturePair&) = default;
    friend auto operator<=>(const SamplerTexturePair&, const SamplerTexturePair&) = default;
};

/** @brief What a device needs to run one entry point's code. Equal code gives equal facts. */
struct SourceCodeFacts
{
    /** Sorted and unique, without the names the profile guarantees. */
    std::vector<std::string> CapabilityNames;
    uint32_t WorkgroupMemoryBytes{ 0u };

    friend bool operator==(const SourceCodeFacts&, const SourceCodeFacts&) = default;
};

/** @brief One distinct form of entry point code from a particular set of compilation flags.
 *  Needed to distinguish the code generated by the base no-flags cook (for reflection and
 *  and measurement purposes) from potential optimized cooks that may produce different code. */
struct EntryPointCode
{
    std::vector<std::byte> Code;
    std::vector<uint32_t> UsedBindingIndices;
    EntryPointConstants Constants;
    std::vector<SamplerTexturePair> SamplerTexturePairs;
    SourceCodeFacts CodeFacts;
};

struct CompiledEntryPoint
{
    std::string Name;
    std::string VariantSuffix;
    /** Reflection only. The reader edits the forms, never this. */
    EntryPointReflection Reflection;
    /** This is the code that will be delivered in the manifest. */
    EntryPointCode Delivered;
    /** Empty when base and target flags match. */
    std::optional<EntryPointCode> Base;
};

/** Returns the base form, which is the delivered form when the cook compiled once. */
const EntryPointCode& BaseForm(const CompiledEntryPoint& entry_point) noexcept;

struct CompiledVariant
{
    std::string VariantSuffix;
    std::string VariantDescription;
    /** Dense mixed-radix index over the canonical assignment. */
    uint64_t VariantIndex{ 0u };
    std::vector<ReflectedBinding> Bindings;
    /** Parallel to `Bindings`. Size expressions read axis values, so footprints belong to the variant. */
    std::vector<ResourceFootprint> Footprints;
    std::vector<CompiledEntryPoint> EntryPoints;
};

/** Returns the bindings one entry point reads, joined with this variant's footprints. */
std::vector<ResolvedBinding> BuildEntryPointLayout(const CompiledVariant& variant, size_t entry_point_index);
std::vector<ResolvedBindingView> BuildEntryPointLayoutView(const CompiledVariant& variant, size_t entry_point_index);

bool SameBindingLocation(const ReflectedBinding& lhs, const ReflectedBinding& rhs) noexcept;
void SortBindingsByLocation(std::span<ReflectedBinding> bindings) noexcept;
std::string DescribeBinding(const ReflectedBinding& binding);
std::string DescribeFootprint(const ResourceFootprint& footprint);
std::string DescribeUniformMembers(const ReflectedBinding& binding);

uint64_t HashReflectedBinding(const ReflectedBinding& binding) noexcept;
uint64_t HashReflectedRasterState(const ReflectedRasterState& raster) noexcept;
uint64_t HashEntryPointConstants(const EntryPointConstants& constants) noexcept;

} // namespace lodestone

#endif // !LODESTONE_DATA_SCHEMA_HPP
