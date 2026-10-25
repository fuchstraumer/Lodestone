#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_FORMAT_HPP
#define LODESTONE_SHADER_MANIFEST_FORMAT_HPP
#include "ShaderLibraryTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

/** @brief The on-disk form of a cook bundle: the version, the reader's error codes, and every record. */
namespace lodestone::manifest
{

inline constexpr uint32_t k_ShaderManifestMagic = 0x48535856u;
inline constexpr uint32_t k_ShaderManifestVersion = 17u;

// NOLINTBEGIN(readability-enum-initial-value, performance-enum-size)
enum class ErrorCode : uint32_t
{
    Invalid = 0,
    Success = 1,
    TooSmall = 2,
    BadMagic = 3,
    VersionMismatch = 4,
    SizeMismatch = 5,
    SectionOutOfBounds = 6,
    IndexOutOfBounds = 7,
    Misaligned = 8,
    StringOutOfBounds = 9,
    SourceOutOfBounds = 10,
    ManifestBindingInvalidName = 11,
    ManifestBindingInvalidUniforms = 12,
    EntryPointInvalidName = 13,
    EntryPointInvalidStage = 14,
    InvalidResourceBindingIndex = 15,
    InvalidResourceListRun = 16,
    InvalidFootprintListRun = 17,
    InvalidVisibilityListRun = 18,
    InvalidEntryPointInstanceSourceIndex = 19,
    InvalidEntryPointInstanceVisibilityIndex = 20,
    InvalidEntryPointInstanceRasterIndex = 21,
    InvalidVariantKeyOrder = 22,
    EntryPointInstanceGridSizeMismatch = 23,
    InvalidRasterVertexInputRange = 24,
    InvalidRasterColorTargetRange = 25,
    InvalidVertexInput = 26,
    InvalidVariantFootprintListIndex = 27,
    InvalidUniformMember = 28,
    InvalidAxisName = 29,
    InvalidAxisValueRange = 30,
    InvalidAxisTypeStrIndex = 31,
    InvalidModuleNameString = 32,
    InvalidProfileTargetName = 33,
    EnvironmentExtentOutOfBounds = 34,
    InvalidModuleAxisIndex = 35,
    InvalidModuleAxisValueMask = 36,
    InvalidSpecializationConstant = 37,
    EnvironmentNotCooked = 38,
    InvalidVariantAxisMask = 39,
    InvalidProfileAccessModel = 40,
    InvalidVariantSuffixString = 41,
    InvalidProfileCodeFormat = 42,
    SpirvSourceMisaligned = 43,
    InvalidVisibilityListOrder = 44,
    MergedLayoutTooManyStages = 45,
    MergedLayoutDuplicateStage = 46,
    MergedLayoutPlacementUnsupported = 47,
    MergedLayoutConflict = 48,
    InvalidModuleAxisDefault = 49,
    InvalidAxisRelevance = 50,
    InvalidSamplerPairListRun = 51,
    InvalidEntryPointInstanceSamplerPairIndex = 52,
    InvalidSamplerTexturePair = 53,
    InvalidEntryPointInstanceConstantsIndex = 54,
    InvalidEntryPointConstants = 55,
    // the shorter block's members are not the first members of the longer block
    MergedLayoutPushConstantConflict = 56,
    InvalidBindingRoles = 57,
    InvalidBindingTags = 58,
    InvalidFootprintAxes = 59,
    InvalidCapabilityNames = 60,
    InvalidSourceCapabilities = 61,
    InvalidStoredValue = 62,
    StoredValueContainerMissing = 63,
    Count
};

enum class ShaderManifestTable : uint32_t
{
    Invalid = 0,
    Modules,
    ModuleAxes,
    Profiles,
    Environments,
    Strings,
    Sources,
    Bindings,
    ResourceIndices,
    ResourceLists,
    Footprints,
    FootprintLists,
    VisibilityLists,
    VisibilityIndices,
    EntryPoints,
    EntryPointInstances,
    Variants,
    VariantKeys,
    AxisMasks,
    Axes,
    AxisValues,
    Rasters,
    VertexInputs,
    ColorTargets,
    UniformMembers,
    SpecializationConstants,
    AxisRelevance,
    SamplerPairLists,
    SamplerTexturePairs,
    EntryPointConstants,
    ResourceTags,
    FootprintAxes,
    CapabilityNames,
    SourceCapabilities,
    Count
};
//NOLINTEND(readability-enum-initial-value, performance-enum-size)

struct ErrorState
{
    ErrorCode Code{ ErrorCode::Invalid };
    ShaderManifestTable Table{ ShaderManifestTable::Invalid };
    uint32_t RecordIndex{ 0u };
    // the offending value, bound, or version, depending on the code
    uint32_t Detail{ 0u };
    constexpr explicit operator bool() const noexcept
    {
        return Code == ErrorCode::Success;
    }

    constexpr bool operator!() const noexcept
    {
        return Code != ErrorCode::Success;
    }
};

template<typename T>
using ManifestResult = std::expected<T, ErrorState>;

std::string_view ToString(ErrorCode error) noexcept;

/** @brief Returns a one-line description that includes the table, the record index, and `Detail`. */
std::string DescribeShaderManifestError(const ErrorState& error);

/** @brief A contiguous subrange of an index table. */
struct alignas(8) Run
{
    uint32_t First{ 0u };
    uint32_t Count{ 0u };
};

/** @brief A table's offset and count, stored in a header. */
struct alignas(8) TableRef
{
    uint32_t Offset{ 0u };
    uint32_t Count{ 0u };
};

struct alignas(8) TableRef64
{
    uint64_t Offset{ 0u };
    uint64_t Count{ 0u };
};

/** @brief The whole-cook header, at offset zero, with absolute offsets. The header region (`HeaderSize` bytes) can be
 * loaded alone, before the extents of one profile. */
struct alignas(8) Header
{
    uint32_t Magic{ 0u };
    uint32_t Version{ 0u };
    uint64_t HeaderSize{ 0u };
    uint64_t FileSize{ 0u };

    // the module headers follow this record
    uint64_t ModuleCount{ 0u };
    TableRef64 Profiles{ 0u, 0u };
    /** @brief `ProfileCount * ModuleCount` entries. (profile P, module M) is at `P * ModuleCount + M`. */
    uint64_t EnvironmentDirectoryOffset{ 0u };

    TableRef64 Strings{ 0u, 0u };
    TableRef64 StringBlobs{ 0u, 0u };
    TableRef64 Axes{ 0u, 0u };
    TableRef64 AxesValues{ 0u, 0u };
};

struct alignas(8) StringRef
{
    uint32_t Offset{ 0u };
    uint32_t Length{ 0u };
};

/** @brief A source in the source blob of one extent. Its capability mask is at the same index. */
struct alignas(8) SourceRef
{
    uint32_t Offset{ 0u };
    uint32_t Length{ 0u };
    /** Compare with `maxComputeSharedMemorySize` or `maxComputeWorkgroupStorageSize`. Zero for other stages. */
    uint32_t WorkgroupMemoryBytes{ 0u };
    uint32_t Reserved{ 0u };
};

/** @brief One cooked form: a target and the placement of each class of shim resource. */
struct alignas(8) Profile
{
    uint32_t TargetNameString{ 0u };
    PlacementForms Forms{};
    ShaderCodeFormat CodeFormat{ ShaderCodeFormat::None };
    uint8_t Reserved{ 0u };
    uint16_t Flags{ 0u };
    uint32_t Reserved2{ 0u };
};

enum class ProfileFlags : uint16_t
{
    None = 0u,
    /** Clear when nothing read the pairs, so an empty list does not mean "no pairs". */
    SamplerTexturePairsRecorded = 1u << 0u,
    /** Clear when no reader ran, so empty capability names and zero workgroup memory do not mean "needs nothing". */
    CodeFactsRead = 1u << 1u,
};

/** @brief Where one (profile, module) environment sits in the file. A size of zero means it was not cooked. */
struct alignas(8) EnvironmentDirectoryEntry
{
    uint64_t ExtentOffset{ 0u };
    uint64_t ExtentSize{ 0u };
};

struct alignas(8) Axis
{
    uint32_t NameString{ 0u };
    uint32_t FirstValue{ 0u };
    uint32_t ValueCount{ 0u };
    AxisKind Kind{ AxisKind::None };
    AxisValueDomain Domain{ AxisValueDomain::None };
    EarliestBindingTime BindingTime{ EarliestBindingTime::None };
    uint8_t Pad{ 0u };
};

struct alignas(8) ModuleAxis
{
    uint32_t AxisIndex{ 0u };
    // the values this module uses, as bits of the root axis
    uint32_t LiveValuesMask{ 0u };
    // a digit of this module's key
    uint32_t DefaultDigit{ 0u };
    uint32_t Reserved{ 0u };
};

/** @brief The profile-independent data of one module. */
struct alignas(8) ModuleRootHeader
{
    uint32_t ModuleNameString{ 0u };
    uint32_t Reserved{ 0u };
    TableRef64 EntryPoints{ 0u, 0u };
    TableRef64 ModuleAxes{ 0u, 0u };
};

struct alignas(8) EntryPoint
{
    uint32_t NameString{ 0u };
    uint32_t Stage{ 0u };
};

/** @brief The first record of one environment extent. Offsets are relative to the extent. Tables with an offset only:
 * - variant keys, variant records: `VariantCount`
 * - axis masks: `VariantCount * W(axes)` words, W(n) = ceil(n / 64)
 * - entry point instances: `VariantCount * EntryPointCount`, (V, E) at `V * EntryPointCount + E`
 * - axis relevance: one mask per entry point, `EntryPointCount * W(axes)`
 * - footprint axes: one mask per binding record, `Bindings.Count * W(axes)`
 * - source capabilities: one mask per source, `Sources.Count * W(CapabilityNames.Count)` */
struct alignas(8) EnvironmentHeader
{
    uint32_t VariantCount{ 0u };
    uint32_t VariantKeyTableOffset{ 0u };
    uint32_t VariantTableOffset{ 0u };
    uint32_t AxisMaskTableOffset{ 0u };
    uint32_t EntryPointInstanceTableOffset{ 0u };
    uint32_t AxisRelevanceTableOffset{ 0u };
    uint32_t FootprintAxisTableOffset{ 0u };
    uint32_t SourceCapabilityTableOffset{ 0u };

    TableRef Sources{ 0u, 0u };
    TableRef SourceBlob{ 0u, 0u };

    TableRef Bindings{ 0u, 0u };
    TableRef ResourceLists{ 0u, 0u };
    TableRef ResourceIndices{ 0u, 0u };
    TableRef Footprints{ 0u, 0u };
    TableRef FootprintLists{ 0u, 0u };
    TableRef VisibilityLists{ 0u, 0u };
    TableRef VisibilityIndices{ 0u, 0u };
    TableRef Rasters{ 0u, 0u };
    TableRef VertexInputs{ 0u, 0u };
    TableRef ColorTargets{ 0u, 0u };
    TableRef UniformMembers{ 0u, 0u };
    TableRef SpecConstants{ 0u, 0u };
    TableRef SamplerPairLists{ 0u, 0u };
    TableRef SamplerTexturePairs{ 0u, 0u };
    TableRef EntryPointConstants{ 0u, 0u };
    TableRef ResourceTags{ 0u, 0u };
    TableRef CapabilityNames{ 0u, 0u };
};

/** @brief One variant of one environment, at the position of its key. */
struct alignas(8) Variant
{
    uint32_t SuffixString{ 0u };
    uint32_t ResourceList{ 0u };
    uint32_t FootprintList{ 0u };
    uint32_t Reserved{ 0u };
};

/** @brief Where a shader finds a resource. Bound: group, binding. Handle and Pointer: the group and binding of the uniform
 * buffer that stores the value, or 0 and 0 for push constants (`Binding::ValueContainer`). */
struct alignas(8) PlacementPayload
{
    uint32_t Word0{ 0u };
    uint32_t Word1{ 0u };
};

/** @brief One resource binding. Wide members come first, so that no padding is inserted. */
struct alignas(8) Binding
{
    uint64_t ByteSize{ 0u };
    PlacementPayload Placement{};
    uint32_t NameString{ 0u };
    uint32_t ScopeString{ 0u };
    uint32_t ElementStride{ 0u };
    // 0 for an unbounded binding array, whose descriptor count the renderer chooses
    uint32_t ArrayCount{ 1u };
    uint32_t StorageFormat{ 0u };
    uint32_t UniformMemberCount{ 0u };
    uint32_t FirstUniformMember{ 0u };
    // string indices, in the order of the tags' text
    uint32_t FirstTag{ 0u };
    uint32_t TagCount{ 0u };
    uint8_t PlacementKind{ 0u };
    uint8_t Kind{ 0u };
    uint8_t Shape{ 0u };
    uint8_t IsComparisonSampler{ 0u };
    uint8_t Access{ 0u };
    uint8_t SampleType{ 0u };
    // a `ResourceRoleFlags`
    uint16_t Roles{ 0u };
    // Handle and Pointer: the byte offset of the value in its container
    uint32_t ValueOffset{ 0u };
    // a `StoredValueContainer`, `None` for a bound resource
    uint32_t ValueContainer{ 0u };
    uint32_t Reserved2{ 0u };
};

/** @brief The declared size of a resource: `ElementCount` for a buffer, or `ExtentX/Y/Z` in texels for a texture. */
// todo-ship: replace ElementCount with ExtentX, for a 16-byte record
struct alignas(8) Footprint
{
    uint64_t ElementCount{ 0u };
    uint32_t ExtentX{ 0u };
    uint32_t ExtentY{ 0u };
    uint32_t ExtentZ{ 0u };
    uint32_t Kind{ 0u };
};

/** @brief A texture and a sampler sampled together, as positions in the entry point's visibility list. */
struct alignas(8) SamplerTexturePair
{
    uint32_t Texture{ 0u };
    uint32_t Sampler{ 0u };
};

/** @brief The entry point constants of one instance, in the profile's layout. For a uniform buffer, `Binding` is a
 * position in the visibility list. A push constant block has no placement. */
struct alignas(8) EntryPointConstants
{
    uint32_t ByteSize{ 0u };
    uint32_t Binding{ 0u };
    uint32_t FirstUniformMember{ 0u };
    uint32_t UniformMemberCount{ 0u };
    uint8_t Delivery{ 0u };
    uint8_t Reserved0{ 0u };
    uint16_t Reserved1{ 0u };
    uint32_t Reserved2{ 0u };
};

struct alignas(8) EntryPointInstance
{
    uint32_t Source{ 0u };
    uint32_t Visibility{ 0u };
    uint32_t WorkgroupX{ 1u };
    uint32_t WorkgroupY{ 1u };
    uint32_t WorkgroupZ{ 1u };
    uint32_t Raster{ 0u };
    uint32_t SamplerPairs{ 0u };
    uint32_t Constants{ 0u };
};

struct alignas(8) VertexInput
{
    uint32_t SemanticNameString{ 0u };
    uint32_t SemanticIndex{ 0u };
    uint32_t Location{ 0u };
    uint32_t ScalarType{ 0u };
    uint32_t ComponentCount{ 0u };
    uint32_t Reserved{ 0u };
};

struct alignas(8) UniformMember
{
    uint32_t NameString{ 0u };
    uint32_t Offset{ 0u };
    uint32_t Size{ 0u };
    uint32_t ArrayCount{ 1u };
    uint32_t ElementStride{ 0u };
    uint32_t MatrixLayout{ 0u };
};

/** @brief One specialization constant (a WGSL `override`). Reserved: the cooker does not write this table yet. */
struct alignas(8) SpecializationConstant
{
    uint64_t DefaultBits{ 0u };
    uint32_t NameString{ 0u };
    uint32_t ConstantId{ 0u };
    uint32_t ScalarType{ 0u };
    uint32_t Reserved{ 0u };
};

struct alignas(8) ColorTarget
{
    uint32_t Location{ 0u };
    uint32_t ScalarType{ 0u };
    uint32_t ComponentCount{ 0u };
    uint32_t Reserved{ 0u };
};

/** @brief Runs of vertex inputs and color targets. Compute entry points use a record with both counts at zero. */
struct alignas(8) RasterState
{
    uint32_t FirstVertexInput{ 0u };
    uint32_t VertexInputCount{ 0u };
    uint32_t FirstColorTarget{ 0u };
    uint32_t ColorTargetCount{ 0u };
    uint32_t WritesFragDepth{ 0u };
    uint32_t Reserved{ 0u };
};

} // namespace lodestone::manifest

#endif // !LODESTONE_SHADER_MANIFEST_FORMAT_HPP
