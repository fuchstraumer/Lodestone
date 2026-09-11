#pragma once
#ifndef LODESTONE_RAW_LIBRARY_HPP
#define LODESTONE_RAW_LIBRARY_HPP
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/** What stage 3 produces: everything the compiler said, and no opinion about any of it.
 *
 * Stage 3 is the only stage that talks to Slang.The interfaces name and use no Slang types,
 * so that dependencies on the compiler can stay precisely isolated to this stage. It carries
 * through our meta-annotations without interpreting them, ensuring that stage 4 can evaluate
 * those alongside the retrieved compiler information to fully build resource information
 * (thus why it's the "resolve" step)
 *
 * A binding record here holds **placement** and the shape of the resource. It holds no visibility and
 * no footprint, because those have different keys and different lifetimes. Additionally, most of our
 * visiblity and footprint information is dependent on the permutation system and is highly
 * variant-specific.*/
namespace lodestone
{

/** Placement is vocabulary rather than a stage 3 idea, so it lives on the data schema and stage 4
 * carries it across without flattening it. */
using RawPlacement = ResourcePlacement;

enum class RawSizeAttributeKind : uint8_t
{
    Invalid = 0,
    ElementCount,
    Extent2d,
    Extent3d,
};

std::string_view ToString(RawSizeAttributeKind kind) noexcept;

/** How many arguments the attribute of this kind takes. */
uint32_t ArgumentCountOf(RawSizeAttributeKind kind) noexcept;

/** One `[ls_*]` annotation, exactly as the shader author wrote it.
 *
 * Size attributes travel as strings because it's the only way to stop slang
 * from evaluating them for us - so this lets us evaluate them when we want to. */
struct RawSizeAttribute
{
    /** Index into `RawVariant::Bindings`. */
    uint32_t BindingIndex{ 0u };
    RawSizeAttributeKind Kind{ RawSizeAttributeKind::Invalid };
    std::vector<std::string> Arguments;
};

struct RawAxisDeclaration
{
    std::string Name;
    bool IsBooleanAxis{ false }; // axis with ls_boolean_axis attribute
    std::string AxisValues; // unparsed ls_axis_values arg
    std::string ActiveWhen; // unparsed ls_axis_active_when
    std::string Kind; // unparsed ls_axis_kind arg
    std::string SourceFile;
    int32_t SourceLine;
    int32_t SourceColumn;
};


/** What a resource is, and where it lives. Not how much of it, and not who reads it.
 *
 * `ElementStride` and `ByteSize` come from the type layout rather than from an annotation, so they are
 * properties of the declared type and stay here. The footprint an author asked for with a `[ls_*]`
 * annotation is per variant, and it travels in `RawVariant::SizeAttributes`. */
struct RawBinding
{
    std::string Name;
    std::string ScopeName;
    RawPlacement Placement;
    BindingKind Kind{ BindingKind::Invalid };

    uint32_t ElementStride{ 0u };
    uint64_t ByteSize{ 0u };
    uint32_t ArrayCount{ 1u };

    ResourceShape Shape{ ResourceShape::Invalid };
    TextureSampleType SampleType{ TextureSampleType::Invalid };
    TextureFormat StorageFormat{ TextureFormat::Invalid };
    StorageTextureAccess StorageAccess{ StorageTextureAccess::Invalid };
    SamplerBindingType SamplerType{ SamplerBindingType::Invalid };

    std::vector<ReflectedUniformMember> UniformMembers;
};

struct RawEntryPoint
{
    std::string Name;
    std::string VariantSuffix;
    ShaderStageKind Stage{ ShaderStageKind::Invalid };
    WorkgroupSize Workgroup;
    /** The text the target backend generated. Stage 3 does not read it. */
    std::string TargetText;
    /** Indices into `RawVariant::Bindings`, ascending. This is visibility. */
    std::vector<uint32_t> UsedBindingIndices;
    ReflectedRasterState Raster;
};

struct RawVariant
{
    std::string VariantSuffix;
    std::string VariantDescription;
    uint64_t VariantIndex{ 0u };
    std::vector<RawBinding> Bindings;
    /** Unevaluated. Stage 4 turns these into numbers. */
    std::vector<RawSizeAttribute> SizeAttributes;
    std::vector<RawEntryPoint> EntryPoints;
};

/** Stage 3's per-module output.
 *
 * `ExternDefaults` is easy to miss and stage 4 cannot work without it. A size expression may name an
 * `extern static const` constant that no axis drives, and only Slang knows what that constant
 * defaults to. Stage 4 must not call Slang, so the defaults travel here. */
struct RawModule
{
    std::string Name;
    std::vector<std::string> EntryPointNames;
    std::vector<ExternConstantDefault> ExternDefaults;
    std::vector<RawVariant> Variants;
};

struct RawLibrary
{
    std::vector<RawModule> Modules;
};

} // namespace lodestone

#endif // !LODESTONE_RAW_LIBRARY_HPP
