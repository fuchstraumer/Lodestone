#include "model/ShaderDataSchema.hpp"
#include "model/ContentHash.hpp"
#include "ShaderLibraryTypes.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace lodestone
{

std::string_view ToString(BindingKind kind) noexcept
{
    switch (kind)
    {
    case BindingKind::Sampler:
        return "Sampler";
    case BindingKind::Texture:
        return "Texture";
    case BindingKind::UniformBuffer:
        return "UniformBuffer";
    case BindingKind::ParameterBlock:
        return "ParameterBlock";
    case BindingKind::StorageBuffer:
        return "StorageBuffer";
    case BindingKind::TexelBuffer:
        return "TexelBuffer";
    case BindingKind::CombinedTextureSampler:
        return "CombinedTextureSampler";
    case BindingKind::InputRenderTarget:
        return "InputRenderTarget";
    case BindingKind::InlineUniform:
        return "InlineUniform";
    case BindingKind::RayTracingAccelerationStructure:
        return "RayTracingAccelerationStructure";
    case BindingKind::StorageTexture:
        return "StorageTexture";
    case BindingKind::Invalid:
        return "Invalid";
    }

    return "Invalid";
}

//NOLINTBEGIN(cppcoreguidelines-pro-bounds-constant-array-index)
namespace
{

/** Using constexpr, this structure allows for compile-time composition of shape names from the various
  * flags present in the mask. To populate it, we just iterate over all possible shape values at compile time
  * and have that populate the array. Checked in Godbolt: it really does work at compile time! */
struct ShapeName
{
    std::array<char, 64u> Data{};
    uint8_t Size{ 0u };

    constexpr void Append(std::string_view text) noexcept
    {
        for (const char character : text)
        {
            Data[Size] = character;
            ++Size;
        }
    }

    [[nodiscard]] constexpr std::string_view View() const noexcept
    {
        return std::string_view{ Data.data(), Size };
    }
};

constexpr std::string_view BaseShapeName(ResourceShape base_shape) noexcept
{
    // input is masked to extract the base shape before it gets here
    switch (base_shape)
    {
    case ResourceShape::Texture1D:
        return "Texture1D";
    case ResourceShape::Texture2D:
        return "Texture2D";
    case ResourceShape::Texture3D:
        return "Texture3D";
    case ResourceShape::TextureCube:
        return "TextureCube";
    case ResourceShape::StructuredBuffer:
        return "StructuredBuffer";
    case ResourceShape::ByteAddressBuffer:
        return "ByteAddressBuffer";
    case ResourceShape::AccelerationStructure:
        return "AccelerationStructure";
    case ResourceShape::TextureSubpass:
        return "TextureSubpass";
    default:
        return "Invalid";
    }
}

/** Composes the base name by working from the base shape "downards" (in bits). 
  * Unconditionally appends as it goes, so even invalid combinations still produce
  * a name, which can help identify invalid or unexpected combinations. */
constexpr ShapeName ComposeShapeName(ResourceShape shape) noexcept
{
    ShapeName name;
    name.Append(BaseShapeName(GetBaseShape(shape)));
    if (ResourceShapeIsMultisample(shape))
    {
        name.Append("Multisample");
    }
    if (ResourceShapeIsArray(shape))
    {
        name.Append("Array");
    }
    if (ResourceShapeIsShadow(shape))
    {
        name.Append("Shadow");
    }
    if (HasAnyFlag(shape, ResourceShape::FeedbackFlag))
    {
        name.Append("Feedback");
    }
    return name;
}

/** Build the names for each value in the span of a uint8_t. This one is consteval,
 *  so it absolutely runs at compile time. */
consteval std::array<ShapeName, 256u> BuildShapeNameTable() noexcept
{
    std::array<ShapeName, 256u> table{};
    for (size_t value = 0u; value < table.size(); ++value)
    {
        table[value] = ComposeShapeName(static_cast<ResourceShape>(value));
    }
    return table;
}

inline constexpr std::array<ShapeName, 256u> k_ShapeNames = BuildShapeNameTable();

} // namespace

std::string_view ToString(ResourceShape shape) noexcept
{
    return k_ShapeNames[static_cast<uint8_t>(shape)].View();
}
//NOLINTEND(cppcoreguidelines-pro-bounds-constant-array-index)

std::string_view ToString(TextureSampleType sample_type) noexcept
{
    switch (sample_type)
    {
    case TextureSampleType::Float:
        return "Float";
    case TextureSampleType::UnfilterableFloat:
        return "UnfilterableFloat";
    case TextureSampleType::Depth:
        return "Depth";
    case TextureSampleType::SignedInteger:
        return "SignedInteger";
    case TextureSampleType::UnsignedInteger:
        return "UnsignedInteger";
    case TextureSampleType::Invalid:
        return "Invalid";
    }

    return "Invalid";
}

std::string_view ToString(ShaderStageKind stage) noexcept
{
    switch (stage)
    {
    case ShaderStageKind::Vertex:
        return "Vertex";
    case ShaderStageKind::Hull:
        return "Hull";
    case ShaderStageKind::Domain:
        return "Domain";
    case ShaderStageKind::Fragment:
        return "Fragment";
    case ShaderStageKind::Compute:
        return "Compute";
    case ShaderStageKind::RayGeneration:
        return "RayGeneration";
    case ShaderStageKind::Intersection:
        return "Intersection";
    case ShaderStageKind::AnyHit:
        return "AnyHit";
    case ShaderStageKind::ClosestHit:
        return "ClosestHit";
    case ShaderStageKind::Miss:
        return "Miss";
    case ShaderStageKind::Callable:
        return "Callable";
    case ShaderStageKind::Mesh:
        return "Mesh";
    case ShaderStageKind::Amplification:
        return "Amplification";
    case ShaderStageKind::Dispatch:
        return "Dispatch";
    case ShaderStageKind::Node:
        return "Node";
    case ShaderStageKind::Invalid:
        return "Invalid";
    case ShaderStageKind::Count:
        std::unreachable();
    }

    return "Invalid";
}

std::string_view ToString(VertexScalarType scalar_type) noexcept
{
    switch (scalar_type)
    {
    case VertexScalarType::Float16:
        return "f16";
    case VertexScalarType::Float32:
        return "f32";
    case VertexScalarType::SignedInteger32:
        return "i32";
    case VertexScalarType::UnsignedInteger32:
        return "u32";
    case VertexScalarType::Invalid:
        return "Invalid";
    }

    return "Invalid";
}

std::string DescribeRasterState(const ReflectedRasterState& raster)
{
    std::string description;

    for (const ReflectedVertexInput& input : raster.VertexInputs)
    {
        description += std::format("      @location({}) {}{} : {}x{}\n",
                                   input.Data.Location,
                                   input.SemanticName,
                                   input.Data.SemanticIndex,
                                   ToString(input.Data.ScalarType),
                                   input.Data.ComponentCount);
    }

    for (const ReflectedColorTarget& target : raster.ColorTargets)
    {
        description += std::format("      target {} : {}x{} (format stays with the caller)\n",
                                   target.Location,
                                   ToString(target.ScalarType),
                                   target.ComponentCount);
    }

    if (raster.WritesFragDepth)
    {
        description += "      writes SV_Depth\n";
    }

    return description;
}

const BoundPlacement* GetBoundPlacement(const ResourcePlacement& placement) noexcept
{
    return std::get_if<BoundPlacement>(&placement);
}

bool PlacementLess(const ResourcePlacement& lhs, const ResourcePlacement& rhs) noexcept
{
    const BoundPlacement* left = GetBoundPlacement(lhs);
    const BoundPlacement* right = GetBoundPlacement(rhs);

    if (left == nullptr || right == nullptr)
    {
        return left != nullptr;
    }

    if (left->Group != right->Group)
    {
        return left->Group < right->Group;
    }

    return left->Binding < right->Binding;
}

uint32_t GroupOf(const ReflectedBinding& binding) noexcept
{
    const BoundPlacement* placement = GetBoundPlacement(binding.Placement);
    return placement != nullptr ? placement->Group : 0u;
}

uint32_t BindingOf(const ReflectedBinding& binding) noexcept
{
    const BoundPlacement* placement = GetBoundPlacement(binding.Placement);
    return placement != nullptr ? placement->Binding : 0u;
}

bool SameBindingLocation(const ReflectedBinding& lhs, const ReflectedBinding& rhs) noexcept
{
    return lhs.Placement == rhs.Placement;
}

std::vector<ResolvedBinding> BuildEntryPointLayout(const CompiledVariant& variant, size_t entry_point_index)
{
    if (entry_point_index >= variant.EntryPoints.size())
    {
        return {};
    }

    std::vector<ResolvedBinding> layout;
    layout.reserve(variant.EntryPoints[entry_point_index].Reflection.UsedBindingIndices.size());

    for (const uint32_t index : variant.EntryPoints[entry_point_index].Reflection.UsedBindingIndices)
    {
        if (index >= variant.Bindings.size())
        {
            continue;
        }

        ResourceFootprint footprint{};
        if (index < variant.Footprints.size())
        {
            footprint = variant.Footprints[index];
        }

        layout.emplace_back(variant.Bindings[index], std::move(footprint));
    }

    return layout;
}

std::vector<ResolvedBindingView> BuildEntryPointLayoutView(const CompiledVariant& variant, size_t entry_point_index)
{
    if (entry_point_index >= variant.EntryPoints.size())
    {
        return {};
    }

    std::vector<ResolvedBindingView> layout;
    layout.reserve(variant.EntryPoints[entry_point_index].Reflection.UsedBindingIndices.size());

    for (const uint32_t index : variant.EntryPoints[entry_point_index].Reflection.UsedBindingIndices)
    {
        if (index >= variant.Bindings.size())
        {
            continue;
        }

        const ReflectedBinding* resource = &variant.Bindings[index];
        const ResourceFootprint* footprint = index < variant.Footprints.size() ? &variant.Footprints[index] : nullptr;

        layout.emplace_back(resource, footprint);
    }

    return layout;
}

void SortBindingsByLocation(std::span<ReflectedBinding> bindings) noexcept
{
    std::ranges::sort(bindings,
                      [](const ReflectedBinding& lhs, const ReflectedBinding& rhs)
                      {
                          return PlacementLess(lhs.Placement, rhs.Placement);
                      });
}

std::string DescribeFootprint(const ResourceFootprint& footprint)
{
    if (const BufferFootprint* buffer = std::get_if<BufferFootprint>(&footprint))
    {
        return std::format(" count={} [{}]", buffer->ElementCount, buffer->Expression);
    }

    if (const TextureFootprint* texture = std::get_if<TextureFootprint>(&footprint))
    {
        return std::format(" extent={}x{}x{} [{}]",
                           texture->ExtentX,
                           texture->ExtentY,
                           texture->ExtentZ,
                           texture->Expression);
    }

    return {};
}

std::string DescribeBinding(const ReflectedBinding& binding)
{
    std::string description = std::format("@group({}) @binding({}) {} : {}",
                                          GroupOf(binding),
                                          BindingOf(binding),
                                          binding.Name,
                                          ToString(binding.Kind));

    if (binding.Shape != ResourceShape::Invalid)
    {
        description += std::format(" {}", ToString(binding.Shape));
    }

    if (binding.ElementStride != 0u)
    {
        description += std::format(" stride={}", binding.ElementStride);
    }

    if (binding.ByteSize != 0u)
    {
        description += std::format(" bytes={}", binding.ByteSize);
    }

    if (binding.SampleType != TextureSampleType::Invalid)
    {
        description += std::format(" sample={}", ToString(binding.SampleType));
    }

    if (binding.ArrayCount > 1u)
    {
        description += std::format(" array={}", binding.ArrayCount);
    }

    return description;
}

std::string DescribeUniformMembers(const ReflectedBinding& binding)
{
    std::string description;

    for (const ReflectedUniformMember& member : binding.UniformMembers)
    {
        description += std::format("[shader_cooker]       +{} {} ({} bytes{})\n",
                                   member.Offset,
                                   member.Name,
                                   member.Size,
                                   member.ArrayCount > 1u ? std::format(", array={}", member.ArrayCount)
                                                          : std::string{});
    }

    return description;
}

//todo-ship: audit and update this. we've changed the field widths quite a bit
uint64_t HashReflectedBinding(const ReflectedBinding& binding) noexcept
{
    thread_local StreamingHash compositeHasher;
    compositeHasher.Reset(); // always reset at opening
    // this replaces previous crappy xor fnv-1a combine hash stuff, with proper hashing
    // should mean much better distribution and fewer collisions than the previous approach.
    compositeHasher.Append(std::string_view{ binding.Name });
    compositeHasher.Append(static_cast<uint64_t>(binding.Placement.index()));
    // gather all our integral fields into one big run of uint64_t's. xxHash3 avalanches exceedingly well,
    // so unlike with fnv-1a where we worry about repeated low bits having little effect on the output, 
    // that's not true here. converting once to 64 bit uints before hashing is sufficient for good distribution.
    const uint64_t scalarValues[]
    {
        static_cast<uint64_t>(binding.Kind),
        static_cast<uint64_t>(binding.ElementStride),
        binding.ByteSize,
        static_cast<uint64_t>(binding.ArrayCount),
        static_cast<uint64_t>(binding.Shape),
        static_cast<uint64_t>(binding.SampleType),
        static_cast<uint64_t>(binding.StorageFormat),
        static_cast<uint64_t>(binding.Access),
        static_cast<uint64_t>(binding.IsComparisonSampler)
    };
    compositeHasher.Append(std::span{ scalarValues, std::size(scalarValues) });

    for (const ReflectedUniformMember& member : binding.UniformMembers)
    {
        compositeHasher.Append(std::string_view{ member.Name });
        // todo: startlifetimeasarray or asbytes would probably work here
        const uint64_t memberScalars[]
        {
            static_cast<uint64_t>(member.Offset),
            static_cast<uint64_t>(member.Size),
            static_cast<uint64_t>(member.ArrayCount)
        };
        compositeHasher.Append(std::span{ memberScalars, std::size(memberScalars) });
    }

    return compositeHasher.Finalize();
}

uint64_t HashReflectedRasterState(const ReflectedRasterState& rasterState) noexcept
{
    thread_local StreamingHash compositeHasher;
    compositeHasher.Reset();
    // we could probably reinterpret most of these as byte spans

    for (const ReflectedVertexInput& vertexInput : rasterState.VertexInputs)
    {
        compositeHasher.Append(std::string_view{ vertexInput.SemanticName });
        const std::span<const ReflectedVertexInput::Packed> vertexInputScalarsSpan = std::span{ &vertexInput.Data, 1 };
        const std::span<const std::byte> bytes = std::as_bytes(vertexInputScalarsSpan);
        compositeHasher.Append(bytes);
    }

    const std::span<const ReflectedColorTarget> colorTargetsSpan = std::span{ rasterState.ColorTargets.data(), rasterState.ColorTargets.size() };
    const std::span<const std::byte> colorTargetsBytes = std::as_bytes(colorTargetsSpan);
    compositeHasher.Append(colorTargetsBytes);

    return compositeHasher.Finalize();
}

} // namespace lodestone
