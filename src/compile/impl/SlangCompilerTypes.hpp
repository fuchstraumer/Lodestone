#pragma once
#ifndef LODESTONE_SHADER_COMPILER_SLANG_COMPILER_TYPES_HPP
#define LODESTONE_SHADER_COMPILER_SLANG_COMPILER_TYPES_HPP
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <slang.h>
#include <slang-com-ptr.h>

/** @brief Holds types and definitions used only within this module,
 * but which are shared by multiple components within this module.*/
namespace lodestone
{

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wswitch-enum"
#endif

constexpr ShaderStageKind FromSlangStage(SlangStage stage) noexcept
{
    switch (stage)
    {
    case SLANG_STAGE_VERTEX:
        return ShaderStageKind::Vertex;
    case SLANG_STAGE_HULL:
        return ShaderStageKind::Hull;
    case SLANG_STAGE_DOMAIN:
        return ShaderStageKind::Domain;
    case SLANG_STAGE_FRAGMENT:
        return ShaderStageKind::Fragment;
    case SLANG_STAGE_COMPUTE:
        return ShaderStageKind::Compute;
    case SLANG_STAGE_RAY_GENERATION:
        return ShaderStageKind::RayGeneration;
    case SLANG_STAGE_INTERSECTION:
        return ShaderStageKind::Intersection;
    case SLANG_STAGE_ANY_HIT:
        return ShaderStageKind::AnyHit;
    case SLANG_STAGE_CLOSEST_HIT:
        return ShaderStageKind::ClosestHit;
    case SLANG_STAGE_MISS:
        return ShaderStageKind::Miss;
    case SLANG_STAGE_CALLABLE:
        return ShaderStageKind::Callable;
    case SLANG_STAGE_MESH:
        return ShaderStageKind::Mesh;
    case SLANG_STAGE_AMPLIFICATION:
        return ShaderStageKind::Amplification;
    case SLANG_STAGE_DISPATCH:
        return ShaderStageKind::Dispatch;
    case SLANG_STAGE_NODE:
        return ShaderStageKind::Node;
    default:
        return ShaderStageKind::Invalid;
    }
}

constexpr BindingKind FromSlangBindingType(slang::BindingType binding_type) noexcept
{
    switch (binding_type)
    {
    case slang::BindingType::Sampler:
        return BindingKind::Sampler;
    case slang::BindingType::Texture:
        return BindingKind::Texture;
    case slang::BindingType::ConstantBuffer:
        return BindingKind::UniformBuffer;
    case slang::BindingType::ParameterBlock:
        return BindingKind::ParameterBlock;
    case slang::BindingType::TypedBuffer:
        [[fallthrough]];
    case slang::BindingType::RawBuffer:
        return BindingKind::ReadOnlyStructuredBuffer;
    case slang::BindingType::CombinedTextureSampler:
        return BindingKind::CombinedTextureSampler;
    case slang::BindingType::InputRenderTarget:
        return BindingKind::InputRenderTarget;
    case slang::BindingType::InlineUniformData:
        return BindingKind::InlineUniform;
    case slang::BindingType::RayTracingAccelerationStructure:
        return BindingKind::RayTracingAccelerationStructure;
    case slang::BindingType::MutableTypedBuffer:
        return BindingKind::StructuredBuffer;
    case slang::BindingType::MutableRawBuffer:
        return BindingKind::StorageBuffer;
    case slang::BindingType::MutableTexture:
        return BindingKind::StorageTexture;
    default:
        return BindingKind::Invalid;
    }
}

/** Slang packs the base shape and the array and multisample flags into one value, so the base
    * shape must be masked out before the comparison. */
constexpr ResourceShape FromSlangResourceShape(SlangResourceShape shape) noexcept
{
    const auto baseShape = static_cast<SlangResourceShape>(shape & SLANG_RESOURCE_BASE_SHAPE_MASK);
    const bool isArray = (shape & SLANG_TEXTURE_ARRAY_FLAG) != 0;
    const bool isMultisample = (shape & SLANG_TEXTURE_MULTISAMPLE_FLAG) != 0;

    switch (baseShape)
    {
    case SLANG_TEXTURE_1D:
        return ResourceShape::Texture1D;
    case SLANG_TEXTURE_2D:
        if (isMultisample)
        {
            return ResourceShape::Texture2DMultisample;
        }
        return isArray ? ResourceShape::Texture2DArray : ResourceShape::Texture2D;
    case SLANG_TEXTURE_3D:
        return ResourceShape::Texture3D;
    case SLANG_TEXTURE_CUBE:
        return isArray ? ResourceShape::TextureCubeArray : ResourceShape::TextureCube;
    case SLANG_STRUCTURED_BUFFER:
        [[fallthrough]];
    case SLANG_BYTE_ADDRESS_BUFFER:
        [[fallthrough]];
    case SLANG_TEXTURE_BUFFER:
        return ResourceShape::Buffer;
    default:
        return ResourceShape::Invalid;
    }
}

/** Maps the scalar type a texture returns onto the sample type WebGPU wants. A depth texture is
    * not distinguishable here, so the graph decides that from the format it creates. */
constexpr TextureSampleType FromSlangScalarType(slang::TypeReflection::ScalarType scalar_type) noexcept
{
    switch (scalar_type)
    {
    case slang::TypeReflection::ScalarType::Int8:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::Int16:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::Int32:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::Int64:
        return TextureSampleType::SignedInteger;
    case slang::TypeReflection::ScalarType::UInt8:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::UInt16:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::UInt32:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::UInt64:
        return TextureSampleType::UnsignedInteger;
    case slang::TypeReflection::ScalarType::Float16:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::Float32:
        [[fallthrough]];
    case slang::TypeReflection::ScalarType::Float64:
        return TextureSampleType::Float;
    default:
        return TextureSampleType::Invalid;
    }
}

constexpr VertexScalarType FromSlangVertexScalarType(
    slang::TypeReflection::ScalarType scalar_type) noexcept
{
    switch (scalar_type)
    {
    case slang::TypeReflection::ScalarType::Float16:
        return VertexScalarType::Float16;
    case slang::TypeReflection::ScalarType::Float32:
        return VertexScalarType::Float32;
    case slang::TypeReflection::ScalarType::Int32:
        return VertexScalarType::SignedInteger32;
    case slang::TypeReflection::ScalarType::UInt32:
        return VertexScalarType::UnsignedInteger32;
    default:
        return VertexScalarType::Invalid;
    }
}

/** Only the formats Velox curates. An unmapped format returns Invalid on purpose: the graph must
    * reject a shader that asks for a format the engine does not express. */
constexpr TextureFormat FromSlangImageFormat(SlangImageFormat format) noexcept
{
    switch (format)
    {
    case SLANG_IMAGE_FORMAT_r8:
        return TextureFormat::R8Unorm;
    case SLANG_IMAGE_FORMAT_rg8:
        return TextureFormat::RG8Unorm;
    case SLANG_IMAGE_FORMAT_rgba8:
        return TextureFormat::RGBA8Unorm;
    case SLANG_IMAGE_FORMAT_r16f:
        return TextureFormat::R16Float;
    case SLANG_IMAGE_FORMAT_rg16f:
        return TextureFormat::RG16Float;
    case SLANG_IMAGE_FORMAT_rgba16f:
        return TextureFormat::RGBA16Float;
    case SLANG_IMAGE_FORMAT_r32f:
        return TextureFormat::R32Float;
    case SLANG_IMAGE_FORMAT_rg32f:
        return TextureFormat::RG32Float;
    case SLANG_IMAGE_FORMAT_rgba32f:
        return TextureFormat::RGBA32Float;
    case SLANG_IMAGE_FORMAT_r32ui:
        return TextureFormat::R32Uint;
    case SLANG_IMAGE_FORMAT_rg32ui:
        return TextureFormat::RG32Uint;
    case SLANG_IMAGE_FORMAT_rgba32ui:
        return TextureFormat::RGBA32Uint;
    case SLANG_IMAGE_FORMAT_rgb10_a2:
        return TextureFormat::R10G10B10A2Unorm;
    case SLANG_IMAGE_FORMAT_r11f_g11f_b10f:
        return TextureFormat::R11G11B10Ufloat;
    default:
        return TextureFormat::Invalid;
    }
}

constexpr StorageTextureAccess FromSlangBindingTypeAccess(slang::BindingType binding_type) noexcept
{
    switch (binding_type)
    {
    case slang::BindingType::MutableTexture:
        return StorageTextureAccess::ReadWrite;
    case slang::BindingType::Texture:
        return StorageTextureAccess::ReadOnly;
    default:
        return StorageTextureAccess::Invalid;
    }
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

/** True for a name the hardware supplies, such as SV_Position. Those never become a vertex buffer
    * attribute, so they must not reach the vertex input list. */
constexpr bool IsSystemSemantic(std::string_view semantic_name) noexcept
{
    return semantic_name.starts_with("SV_") || semantic_name.starts_with("sv_");
}

constexpr bool IsDepthSemantic(std::string_view semantic_name) noexcept
{
    return semantic_name == "SV_Depth" || semantic_name == "SV_DEPTH" ||
            semantic_name == "SV_DepthGreaterEqual" || semantic_name == "SV_DepthLessEqual";
}

/**
 * Where one parameter scope starts, and what Slang emits around the bindings inside it.
 *
 * The global scope starts at group 0 and binding 0, and it adds no name. Every other scope takes
 * each answer from reflection. A nested scope carries the whole chain in `Name`, so the emitted
 * name of a binding stays `<Name>_<binding>` at any depth.
 *
 * `Base` and `SpaceBase` are two different numbers, and reading one for the other costs a cook.
 * `Base` is where a binding declared directly in this scope sits. `SpaceBase` is where a
 * parameter block declared in this scope starts counting spaces. The entry point scope of a probe
 * reported a slot offset of 0 and a sub-element space offset of 1, and its block took space 1.
 */
struct BindingScope
{
    BoundPlacement Base;
    uint32_t SpaceBase{ 0u };
    std::string Name;
};

/** One binding, plus the `[vx_*]` annotations of that binding. The annotations travel beside the
 * binding until the order is settled, because a sort of the bindings alone would leave every
 * annotation against the wrong one. */
struct RawBindingDraft
{
    RawBinding Binding;
    std::vector<RawSizeAttribute> Attributes;
};

/** One parameter block, as the sub-object range of its parent describes it.
 *
 * `UniformSize` is zero when the block holds resources alone. Slang emits no container for such a
 * block, so drafting one would report a binding the shader has not got. */
struct ParameterBlockInfo
{
    slang::TypeLayoutReflection* ElementLayout{ nullptr };
    slang::VariableLayoutReflection* Container{ nullptr };
    std::string_view Name;
    BindingScope Scope;
    uint32_t UniformSize{ 0u };
};



struct SerializedModule
{
    std::string Name;
    std::string Path;
    Slang::ComPtr<slang::IBlob> Blob;
};

/** One compiler option as a row. A row of `Int` kind reads `IntValue`, and a row of `String`
 * kind reads `StringValue`. The unused field keeps the value Slang treats as absent, which is
 * what the entry held before this became a table. */
struct CompilerOptionRow
{
    slang::CompilerOptionName Name{ slang::CompilerOptionName::CountOf };
    slang::CompilerOptionValueKind Kind{};
    union
    {
        int32_t IntValue{ 0 };
        const char* StringValue;
    };
};

constexpr const char* k_AllWarningsAsErrors = "all";
constexpr const char* k_DisabledWarnings = "31010";
// Order reaches Slang, and the two warning level rows must stay in this order.
constexpr std::array<CompilerOptionRow, 8u> k_CompilerOptionRows{
    CompilerOptionRow{ .Name = slang::CompilerOptionName::WarningLevel,
                       .Kind = slang::CompilerOptionValueKind::Int,
                       .IntValue = SLANG_WARNING_LEVEL_PEDANTIC },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::WarningLevel,
                       .Kind = slang::CompilerOptionValueKind::Int,
                       .IntValue = SLANG_WARNING_LEVEL_ALL },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::DisableWarnings,
                       .Kind = slang::CompilerOptionValueKind::String,
                       .StringValue = k_DisabledWarnings },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::WarningsAsErrors,
                       .Kind = slang::CompilerOptionValueKind::String,
                       .StringValue = k_AllWarningsAsErrors },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::FloatingPointMode,
                       .Kind = slang::CompilerOptionValueKind::Int,
                       .IntValue = SLANG_FLOATING_POINT_MODE_FAST },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::DebugInformation,
                       .Kind = slang::CompilerOptionValueKind::Int,
                       .IntValue = SLANG_DEBUG_INFO_LEVEL_NONE },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::EnableMachineReadableDiagnostics,
                       .Kind = slang::CompilerOptionValueKind::Int,
                       .IntValue = 1 },
    CompilerOptionRow{ .Name = slang::CompilerOptionName::UseUpToDateBinaryModule,
                       .Kind = slang::CompilerOptionValueKind::Int,
                       .IntValue = 1 }
};

constexpr SlangOptimizationLevel ToSlangOptimizationLevel(uint32_t level) noexcept
{
    switch (level)
    {
    case 0u:
        return SLANG_OPTIMIZATION_LEVEL_NONE;
    case 1u:
        return SLANG_OPTIMIZATION_LEVEL_DEFAULT;
    case 2u:
        return SLANG_OPTIMIZATION_LEVEL_HIGH;
    case 3u:
        return SLANG_OPTIMIZATION_LEVEL_MAXIMAL;
    default:
        return SLANG_OPTIMIZATION_LEVEL_NONE;
    }
}

slang::CompilerOptionEntry ToOptionEntry(const CompilerOptionRow& row) noexcept;

/** Every option this cook sets. Only the optimization level comes from a caller, so it is the one
 * row the table cannot hold. */
std::vector<slang::CompilerOptionEntry> MakeCompilerOptions(uint32_t optimization_level);

std::string BlobToString(slang::IBlob* blob);

void ReportDiagnostics(class DiagnosticSink& sink, std::string_view context, slang::IBlob* blob);

} // namespace lodestone

#endif // LODESTONE_SHADER_COMPILER_SLANG_COMPILER_TYPES_HPP
