#pragma once
#ifndef LODESTONE_SHADER_COMPILER_SLANG_COMPILER_TYPES_HPP
#define LODESTONE_SHADER_COMPILER_SLANG_COMPILER_TYPES_HPP
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <slang.h>
#include <slang-com-ptr.h>

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

constexpr SlangCompileTarget ToSlangCompileTarget(TargetLanguage language) noexcept
{
    switch (language)
    {
    case TargetLanguage::Wgsl:
        return SLANG_WGSL;
    case TargetLanguage::Spirv:
        return SLANG_SPIRV;
    case TargetLanguage::Invalid:
        [[fallthrough]];
    default:
        return SLANG_TARGET_UNKNOWN;
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
        return BindingKind::TexelBuffer;
    case slang::BindingType::RawBuffer:
        return BindingKind::StorageBuffer;
    case slang::BindingType::CombinedTextureSampler:
        return BindingKind::CombinedTextureSampler;
    case slang::BindingType::InputRenderTarget:
        return BindingKind::InputRenderTarget;
    case slang::BindingType::InlineUniformData:
        return BindingKind::InlineUniform;
    case slang::BindingType::RayTracingAccelerationStructure:
        return BindingKind::RayTracingAccelerationStructure;
    case slang::BindingType::MutableTypedBuffer:
        return BindingKind::TexelBuffer;
    case slang::BindingType::MutableRawBuffer:
        return BindingKind::StorageBuffer;
    case slang::BindingType::MutableTexture:
        return BindingKind::StorageTexture;
    default:
        return BindingKind::Invalid;
    }
}

constexpr ResourceShape FromSlangResourceShape(SlangResourceShape shape) noexcept
{
    // the values line up, minus TextureBuffer and Unknown, which we removed
    return static_cast<ResourceShape>(shape);
}

/** Depth is not visible here, so the graph decides it from the format it creates. */
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

/** Unmapped formats return Invalid, so that the graph rejects formats the engine does not express. */
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
    case SLANG_IMAGE_FORMAT_rgba8_snorm:
        return TextureFormat::RGBA8Snorm;
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

constexpr bool IsResourceTypeKind(slang::TypeReflection::Kind kind) noexcept
{
    constexpr static slang::TypeReflection::Kind k_ResourceKinds[]
    {
        slang::TypeReflection::Kind::ConstantBuffer,
        slang::TypeReflection::Kind::Resource,
        slang::TypeReflection::Kind::SamplerState,
        slang::TypeReflection::Kind::TextureBuffer,
        slang::TypeReflection::Kind::ShaderStorageBuffer,
        slang::TypeReflection::Kind::ParameterBlock,
        slang::TypeReflection::Kind::Feedback,
        slang::TypeReflection::Kind::Pointer,
        slang::TypeReflection::Kind::DynamicResource
    };
    // im doing this just because i think it's funny that it's constexpr
    return std::ranges::contains(k_ResourceKinds, kind);
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif

/** True for hardware-supplied names such as SV_Position, which never become vertex attributes. */
constexpr bool IsSystemSemantic(std::string_view semantic_name) noexcept
{
    return semantic_name.starts_with("SV_") || semantic_name.starts_with("sv_");
}

constexpr bool IsDepthSemantic(std::string_view semantic_name) noexcept
{
    return semantic_name == "SV_Depth" || semantic_name == "SV_DEPTH" ||
            semantic_name == "SV_DepthGreaterEqual" || semantic_name == "SV_DepthLessEqual";
}

/** The starting placement of one parameter scope, and the name Slang emits around its bindings. `Base` places the
 * scope's own bindings, and `SpaceBase` is the first space of its parameter blocks: they differ, and mixing them up
 * breaks the cook. */
struct BindingScope
{
    BoundPlacement Base;
    uint32_t SpaceBase{ 0u };
    std::string Name;
};

/** A binding and its annotations, kept together until the order is final. */
struct RawBindingDraft
{
    RawBinding Binding;
    std::vector<RawSizeAttribute> Attributes;
};

/** One parameter block. `UniformSize` is zero for a block of resources alone, which has no constant buffer. */
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

struct InterfaceAxisStub
{
    std::string Name;
    slang::TypeReflection* Type{ nullptr };
    std::string SourceFile;
    int32_t SourceLine{ 0 };
    int32_t SourceColumn{ 0 };
};

struct InterfaceAxisImplStub
{
    std::string InterfaceName;
    slang::TypeReflection* Type{ nullptr };
    RawInterfaceImpl Impl;
};

/** One compiler option. The unused value field keeps the value Slang treats as absent. */
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
// 41012 checks the entry point before specialization, so a capability axis fails every variant. Device capabilities
// are ours to record, from the emitted code
constexpr const char* k_DisabledWarnings = "31010,41012";
// the two warning level rows must stay in this order
constexpr std::array<CompilerOptionRow, 9u> k_CompilerOptionRows{
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
                           .IntValue = 1 },
    // the cook compares the OpEntryPoint name with the manifest name
    CompilerOptionRow{ .Name = slang::CompilerOptionName::VulkanUseEntryPointName,
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

std::vector<slang::CompilerOptionEntry> MakeCompilerOptions(uint32_t optimization_level);

std::vector<std::byte> BlobToBytes(slang::IBlob* blob);
std::string BlobToString(slang::IBlob* blob);

void ReportDiagnostics(class DiagnosticSink& sink, std::string_view context, slang::IBlob* blob);

} // namespace lodestone

#endif // LODESTONE_SHADER_COMPILER_SLANG_COMPILER_TYPES_HPP
