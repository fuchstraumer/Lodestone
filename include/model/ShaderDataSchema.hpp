#pragma once
#ifndef LODESTONE_DATA_SCHEMA_HPP
#define LODESTONE_DATA_SCHEMA_HPP
#include "ShaderLibraryTypes.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/** The schema the cooker extracts and the rendergraph eventually consumes. It names no Slang, WebGPU,
 * or filesystem type. It is the contract between the offline tool and the runtime. A compiler type
 * that leaks into it becomes a dependency the engine must carry.
 *
 * The enums live in shader/ShaderLibraryTypes.hpp, because the graph reads the same vocabulary. This
 * schema adds only the parts that the offline tool needs: owning strings, and the raw expression text
 * that a diagnostic must quote. 
 * 
 * todo-ship: refactor this into a true C ABI. We have patterns from prev ShaderTools iteration for 
 * passing data like strings across DLL boundary, so we can do this just fine.*/
namespace lodestone
{

std::string_view ToString(BindingKind kind) noexcept;
std::string_view ToString(ShaderStageKind stage) noexcept;
std::string_view ToString(ResourceShape shape) noexcept;
std::string_view ToString(TextureSampleType sample_type) noexcept;

/**@brief Where a resource lives under the bound access model. */
struct BoundPlacement
{
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };

    friend bool operator==(const BoundPlacement&, const BoundPlacement&) = default;
};

/**@brief `std::monostate` means the target reported no placement. That is not the same fact as group 0
 * binding 0. Future work will add Indexed (bindless) and Pointer (BDA) placements here as well. */
using ResourcePlacement = std::variant<std::monostate, BoundPlacement>;

/**@brief Null when this resource is not placed by group and binding. */
const BoundPlacement* GetBoundPlacement(const ResourcePlacement& placement) noexcept;

/**@brief Orders two resources by placement. A resource with no placement sorts last. Most useful for
 * bound resources, but not really relevant for the others since those switch their "locations" to runtime */
bool PlacementLess(const ResourcePlacement& lhs, const ResourcePlacement& rhs) noexcept;

/**@brief How many elements a buffer holds, dynamically evaluated from a `[ls_element_count]` attribute. */
struct BufferFootprint
{
    uint64_t ElementCount{ 0u };
    std::string Expression;

    friend bool operator==(const BufferFootprint&, const BufferFootprint&) = default;
};

/**@brief The extent a texture is created with, from a `[ls_extent_2d]` or `[ls_extent_3d]` attribute.
 *
 * @note Briefly, it's important to realize why this isn't a byte size: textures are formatted, so every
 * graphics API only wants the texture dimensions. We don't have format fields yet.
 *
 * todo-ship: Optional format information would be helpful... but only really for storage images. For many
 * of the rest, it could change based on asset pipeline and shouldn't be a concern of the shader. */
struct TextureFootprint
{
    uint32_t ExtentX{ 1u };
    uint32_t ExtentY{ 1u };
    uint32_t ExtentZ{ 1u };
    std::string Expression;

    friend bool operator==(const TextureFootprint&, const TextureFootprint&) = default;
};

using ResourceFootprint = std::variant<std::monostate, BufferFootprint, TextureFootprint>;

/**@brief One member of a uniform block, with the offset and the size the shader gave it. */
struct ReflectedUniformMember
{
    std::string Name;
    uint32_t Offset{ 0u };
    uint32_t Size{ 0u };
    uint32_t ArrayCount{ 1u };

    friend bool operator==(const ReflectedUniformMember&, const ReflectedUniformMember&) = default;
};

/**@brief What a shader states about one resource. The CPU side should never write any of this: this
 * information should be enough to drive most of the work the CPU needs to do to resolve a resource into
 * the final packed data we'll write into the output resource tables.
 *
 * Note some key redundancies: `ElementStride` is useful for structured buffers and other resource
 * types where a stride is required or helpful for CPU validation. `ByteSize` is most often explicit
 * and required for uniforms. `ArrayCount` is *the binding array size* as declared in the shader (usually 1)
 *
 * `StorageFormat` is set only for a storage texture, because the shader spells the format into the
 * binding type there. A sampled texture leaves it Invalid: the shader says it samples a float4, not
 * that the texture is Rgba16Float. That choice stays with the caller. */
struct ReflectedBinding
{
    std::string Name;
    std::string ScopeName;
    ResourcePlacement Placement;
    BindingKind Kind{ BindingKind::Invalid };

    uint32_t ElementStride{ 0u };
    uint64_t ByteSize{ 0u };
    uint32_t ArrayCount{ 1u };

    ResourceShape Shape{ ResourceShape::Invalid };
    TextureSampleType SampleType{ TextureSampleType::Invalid };
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    StorageTextureAccess StorageAccess{ StorageTextureAccess::Invalid };
    SamplerBindingType SamplerType{ SamplerBindingType::Invalid };

    /** Filled only for a uniform block. Every other binding kind leaves it empty. */
    std::vector<ReflectedUniformMember> UniformMembers;

    friend bool operator==(const ReflectedBinding&, const ReflectedBinding&) = default;
};

/**@brief Group number, or zero when this resource has no bound placement. */
uint32_t GroupOf(const ReflectedBinding& binding) noexcept;
/**@brief Binding number, or zero when this resource has no bound placement. */
uint32_t BindingOf(const ReflectedBinding& binding) noexcept;

/**@brief Combined the reflected data (from the shader verbatim) with our "resolved"
 * footprint data extracted from the resolve step. The latter is described in our markup
 * / meta-annotation language, and is best constructed by using ReflectedBinding alongside it. */
struct ResolvedBinding
{
    ReflectedBinding Resource;
    ResourceFootprint Footprint;

    friend bool operator==(const ResolvedBinding&, const ResolvedBinding&) = default;
};

/**@brief View into a ResolvedBinding: ReflectiedBinding and ResourceFootprint both hold large 
 * amounts of data, and there are many locations we're copying the values when the backing data
 * would otherwise be alive and persisted safely. So, view time! */
struct ResolvedBindingView
{
    const ReflectedBinding* Resource{ nullptr };
    const ResourceFootprint* Footprint{ nullptr };
    // account for potential `nullptr` values: resources having no footprint
    // is a relatively common case, for unannotated resources
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

/**@brief One vertex shader input/attribute. Retrieving location, scalar type,
 * and component count should be sufficient for all APIs to create vertex bindings. */
struct ReflectedVertexInput
{
    struct Packed
    {
        uint32_t SemanticIndex{ 0u };
        uint32_t Location{ 0u };
        VertexScalarType ScalarType{ VertexScalarType::Invalid };
        uint32_t ComponentCount{ 0u };
        friend bool operator==(const Packed&, const Packed&) = default;
    } Data;
    std::string SemanticName;

    friend bool operator==(const ReflectedVertexInput&, const ReflectedVertexInput&) = default;
};

/**@brief One fragment shader color target. The scalar type and component count describe the output, but the
 * actual format is determined by the client API being used, and what the actual runtime configuration is.
 * Most of the time, client gfx APIs just convert. One should not build assumptions on the format you can
 * interpret from this because of that
 * @note alignas(16) for some guarantees on memory layout and hashing */
struct alignas(16) ReflectedColorTarget
{
    uint32_t Location{ 0u };
    VertexScalarType ScalarType{ VertexScalarType::Invalid };
    uint32_t ComponentCount{ 0u };
    uint32_t Padding{ 0u };

    friend bool operator==(const ReflectedColorTarget&, const ReflectedColorTarget&) = default;
};

/**@brief Everything the cooker can say about the raster stages of one entry point. This isn't much
 * on purpose, because we assume clients will have some data-driven way to declare the rest of their
 * raster state. This just accelerates creation of it by allowing code to dynamically configure what 
 * it can from shader information. */
struct ReflectedRasterState
{
    std::vector<ReflectedVertexInput> VertexInputs;
    std::vector<ReflectedColorTarget> ColorTargets;
    /**@note Detection of this state is not flawless, but can provide helpful validation. Users should
     * still try to drive frag depth writing configuration themselves in their render state code. */
    bool WritesFragDepth{ false };

    friend bool operator==(const ReflectedRasterState&, const ReflectedRasterState&) = default;
};

/** What the compiler says about one entry point.
 *
 * The binding list is not here. Slang reports the program-scope bindings, so every entry point of one
 * variant sees the same set, and a copy for each entry point states the same fact three times. The set
 * lives once on `CompiledVariant::Bindings`, and this record says only which of those bindings
 * this entry point reads.
 * todo-ship: That's not always going to be true, especially with entrypoint parameter blocks. This will
 * need revision.
 *
 * `UsedBindingIndices` holds indices into that shared list, in ascending order. A list rather than a
 * bit mask, because a mask caps a module at the width of the mask. */
struct EntryPointReflection
{
    std::string Name;
    ShaderStageKind Stage{ ShaderStageKind::Invalid };
    WorkgroupSize Workgroup;
    std::vector<uint32_t> UsedBindingIndices;
    ReflectedRasterState Raster;
};

std::string DescribeRasterState(const ReflectedRasterState& raster);

struct CompiledEntryPoint
{
    std::string Name;
    std::string VariantSuffix;
    std::string Code;
    EntryPointReflection Reflection;
};

struct CompiledVariant
{
    std::string VariantSuffix;
    std::string VariantDescription;
    /** Dense mixed-radix index over the canonical assignment. Stable across cooks, and the key the
     * rendergraph resolves a variant with. */
    uint64_t VariantIndex{ 0u };
    /** Every program-scope binding of this variant, once. It states where it is and what it is, that's it.*/
    std::vector<ReflectedBinding> Bindings;
    /** One footprint for each entry of `Bindings`. A size expression reads the axis values, so
     * a footprint belongs to the variant and not to the entry point that happens to use it. */
    std::vector<ResourceFootprint> Footprints;
    std::vector<CompiledEntryPoint> EntryPoints;
};

/** The bindings one entry point uses, joined with the footprints of this variant.
 *
 * This is the subset the entry point reads, not every binding of the variant. A compute pass that
 * touches no lookup table must not declare one. */
std::vector<ResolvedBinding> BuildEntryPointLayout(const CompiledVariant& variant, size_t entry_point_index);
std::vector<ResolvedBindingView> BuildEntryPointLayoutView(const CompiledVariant& variant, size_t entry_point_index);

bool SameBindingLocation(const ReflectedBinding& lhs, const ReflectedBinding& rhs) noexcept;
void SortBindingsByLocation(std::span<ReflectedBinding> bindings) noexcept;
std::string DescribeBinding(const ReflectedBinding& binding);
/** Empty when the shader declared no size for this resource. */
std::string DescribeFootprint(const ResourceFootprint& footprint);
std::string DescribeUniformMembers(const ReflectedBinding& binding);

uint64_t HashReflectedBinding(const ReflectedBinding& binding) noexcept;
uint64_t HashReflectedRasterState(const ReflectedRasterState& raster) noexcept;

} // namespace lodestone

#endif // !LODESTONE_DATA_SCHEMA_HPP
