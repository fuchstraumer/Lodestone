#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_FORMAT_HPP
#define LODESTONE_SHADER_MANIFEST_FORMAT_HPP
#include "ShaderLibraryTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

/**
 * @brief The on-disk form of a cook bundle: the version, the error codes of the reader, and every record. This
 * header contains data and no views. `ShaderManifest.hpp` includes it, so a client includes that header.
 */
namespace lodestone::manifest
{

inline constexpr uint32_t k_ShaderManifestMagic = 0x48535856u;
inline constexpr uint32_t k_ShaderManifestVersion = 13u;

// clang-tidy complains about enums being too big, but uint32_t means
// the error struct is 16bytes, which is great alignment and still compact
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
    // a pair position is past the visibility list, names the wrong binding kind, or the list is not sorted and unique
    InvalidSamplerTexturePair = 53,
    InvalidEntryPointInstanceConstantsIndex = 54,
    // a delivery that is not known, a binding position that is not a uniform buffer of the same size, a position
    // on a push constant block, or a member run outside the member table
    InvalidEntryPointConstants = 55,
    // two stages put push constant blocks at offset 0, and the members of the shorter block are not the first
    // members of the longer block
    MergedLayoutPushConstantConflict = 56,
    // a binding sets a role bit that `ResourceRoleFlags` does not define
    InvalidBindingRoles = 57,
    // a binding's tag run is outside the tag table, names a string past the string table, or repeats a tag
    InvalidBindingTags = 58,
    // a binding's footprint axis mask sets a bit past the module's axes
    InvalidFootprintAxes = 59,
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
    Count
};
//NOLINTEND(readability-enum-initial-value, performance-enum-size)

/**@brief Contains contextual information on an error found during
 * the parsing or validation of a shader manifest. Using the record
 * index and the offset + the code, we can generate useful error
 * messages. */
struct ErrorState
{
    ErrorCode Code{ ErrorCode::Invalid };
    ShaderManifestTable Table{ ShaderManifestTable::Invalid };
    // index of the offending record within the table
    uint32_t RecordIndex{ 0u };
    // offending value, or bound, or version: varies based on error code and table
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

/** @brief A human-readable, one-line description of a manifest error, for a log or the console. It
 * includes the table, record index, and `Detail` field, so that the message shows where the file is
 * malformed. */
std::string DescribeShaderManifestError(const ErrorState& error);

/**@brief A run in an index table. Kind of like a span: it specifies a contiguous subrange of entries.
*  This goes in the root Manifest scope since it's used by multiple tables. */
struct alignas(8) Run
{
    uint32_t First{ 0u };
    uint32_t Count{ 0u };
};

/**@brief Much like run above, but actually embedded in the table rather than being an on-demand structure. */
struct alignas(8) TableRef
{
    uint32_t Offset{ 0u };
    uint32_t Count{ 0u };
};

/** @brief A 64 bit version of the above, for spans that could exceed the limits of 32-bit offsets. */
struct alignas(8) TableRef64
{
    uint64_t Offset{ 0u };
    uint64_t Count{ 0u };
};

/** @brief The whole-cook header, at offset zero. Every offset in it is absolute.
 *
 * The file is ordered by profile, then by module. The header region (`HeaderSize` bytes) contains this
 * record, the whole-cook tables, the module headers, and the environment directory. Readers load the
 * header region first, then one contiguous run of environment extents for their profile. */
struct alignas(8) Header
{
    uint32_t Magic{ 0u };
    uint32_t Version{ 0u };
    // HeaderSize first, as we use that to just open and read *only* the header when applicable
    uint64_t HeaderSize{ 0u };
    // But whole file size is still important for validation
    uint64_t FileSize{ 0u };

    // module entries begin immediately after the end of this header:
    // offset not needed because it's based on this objects size
    uint64_t ModuleCount{ 0u };
    TableRef64 Profiles{ 0u, 0u };
    /** @brief A dense grid of `ProfileCount * ModuleCount` entries. The entry for (profile P, module M)
     * is at `P * ModuleCount + M`. */
    uint64_t EnvironmentDirectoryOffset{ 0u };

    TableRef64 Strings{ 0u, 0u };
    TableRef64 StringBlobs{ 0u, 0u };
    TableRef64 Axes{ 0u, 0u };
    TableRef64 AxesValues{ 0u, 0u };
};

/** @brief A string in the whole-cook string blob. */
struct alignas(8) StringRef
{
    uint32_t Offset{ 0u };
    uint32_t Length{ 0u };
};

/** @brief A source text in the source blob of one environment extent. `Offset` is relative to the
 * start of that blob. */
struct alignas(8) SourceRef
{
    uint32_t Offset{ 0u };
    uint32_t Length{ 0u };
};

/** @brief One cooked form: a target and an access model. The "capability floor" is currently 
  * unused until we better identify how we want to define and leverage that. */
struct alignas(8) Profile
{
    uint32_t TargetNameString{ 0u };
    uint32_t CapabilityFloor{ 0u };
    PlacementKind AccessModel{ PlacementKind::None };
    ShaderCodeFormat CodeFormat{ ShaderCodeFormat::None };
    /** @brief A set of `ProfileFlags` bits. */
    uint16_t Flags{ 0u };
    uint32_t Reserved2{ 0u };
};

/** @brief The bits of `Profile::Flags`. */
enum class ProfileFlags : uint16_t
{
    None = 0u,
    /** The cook read the sampler and texture pairs from this profile's code. When the bit is clear, every pair
     * list is empty because nothing read the pairs, and an empty list does not mean "no pairs". */
    SamplerTexturePairsRecorded = 1u << 0u,
};

/** @brief Where one (profile, module) environment sits in the file. A size of zero means the module
 * was not cooked for that profile. 
 * @note Profile is considered first, as renderers will want all the modules for a given profile together.*/
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
    // a bitmask of the indices of the values live for this axis in *this module*
    uint32_t LiveValuesMask{ 0u };
    // the default the shader declares for this axis, as a digit of this module's key (below the radix)
    uint32_t DefaultDigit{ 0u };
    uint32_t Reserved{ 0u };
};

// A module has *one* core module root header, which describes the data that is environment-agnostic
// and doesn't change for different targets and profiles.
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

/** @brief The first record of one environment extent: one module, cooked for one profile.
 *
 * Every offset is relative to the start of the extent, so that each extent can be loaded into its own
 * buffer without fixups. Three tables store no count, since other counts determine their size:
 * - Variant keys and variant records share a size
 * - The axis mask table contains one bitmask per variant. The length of the individual records
 *   is somewhat unique though: ceil(ModuleAxisCount / 64) words per variant, since it's a bitmask
 * - The EntryPointInstance table is `VariantCount * EntryPointCount` records.
 *   Thus, the entry point instance for (variant V, entry point E) is at `V * EntryPointCount + E`.
 * - The axis relevance table has one bitmask for each entry point, in the same word layout as the axis mask
 *   table: `EntryPointCount * ceil(ModuleAxisCount / 64)` words. Bit k is set when axis k can change that
 *   entry point's instance in this environment, so a consumer must pin it.
 * - The footprint axis table has one bitmask for each binding record, in the same word layout:
 *   `Bindings.Count * ceil(ModuleAxisCount / 64)` words. Bit k is set when axis k changes the footprint (the
 *   element count or the extent) of that resource in this environment. Every record of one resource (one scope and
 *   name) holds the same mask. An axis with one cooked value cannot change anything, so it is never set. */
struct alignas(8) EnvironmentHeader
{
    uint32_t VariantCount{ 0u };
    uint32_t VariantKeyTableOffset{ 0u };
    uint32_t VariantTableOffset{ 0u };
    uint32_t AxisMaskTableOffset{ 0u };
    uint32_t EntryPointInstanceTableOffset{ 0u };
    uint32_t AxisRelevanceTableOffset{ 0u };
    uint32_t FootprintAxisTableOffset{ 0u };

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
};

/** @brief One variant of one environment. Its key is at the same position in the key table. The
 * capability requirement is unused, as mentioned earlier, while we wait to specify and build that */
struct alignas(8) Variant
{
    uint32_t SuffixString{ 0u };
    uint32_t ResourceList{ 0u };
    uint32_t FootprintList{ 0u };
    uint32_t CapabilityRequirement{ 0u };
};

/** @brief The location of one resource, as the shader accesses it. `Binding::PlacementKind` selects how
 * to read it:
 * - Bound: `Word0` is the group, and `Word1` is the binding.
 * - Indexed: `Word0` is the heap index. `Word1` is zero.
 * - Pointer: `Word0` is the low half and `Word1` the high half of a byte offset.
 */
struct alignas(8) PlacementPayload
{
    uint32_t Word0{ 0u };
    uint32_t Word1{ 0u };
};

/**@brief One resource binding. uint64_t/uint32_t members frontloaded so that we 
 * don't get any sneaky padding inserted by the compiler. */
struct alignas(8) Binding
{
    uint64_t ByteSize{ 0u };
    PlacementPayload Placement{};
    uint32_t NameString{ 0u };
    uint32_t ScopeString{ 0u };
    uint32_t ElementStride{ 0u };
    uint32_t ArrayCount{ 1u };
    uint32_t StorageFormat{ 0u };
    uint32_t UniformMemberCount{ 0u };
    uint32_t FirstUniformMember{ 0u };
    // a run in the tag table: string indices, one for each tag, in the order of the tags' text
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
    uint32_t Reserved2{ 0u };
};

/**@brief "Footprint" refers to the memory footprint of a resource, insofar as we can declare it. `Kind`
* specifies if this is a buffer, texture, or invalid. `ElementCount` is *only* valid for buffers, and
* `ExtentX/Y/Z` is only valid for a texture. The latter is NOT a byte size: it is pixel dims.*/
// todo-ship: Union ExtentX w ElementCount, or just replace ElementCount with ExtentX. That's what a buffer
// length is anyways. This gets us to a round 16 bytes, which is nice and aligned vs 24 now
struct alignas(8) Footprint
{
    uint64_t ElementCount{ 0u };
    uint32_t ExtentX{ 0u };
    uint32_t ExtentY{ 0u };
    uint32_t ExtentZ{ 0u };
    uint32_t Kind{ 0u };
};

/** @brief One texture and one sampler that an entry point samples together. Each field is a position in the
 * entry point's visibility list, so `EntryPointInstanceView::Layout()[Texture]` is the texture. `Open` checks
 * that `Texture` names a texture and `Sampler` a sampler, and that each list is sorted and unique. */
struct alignas(8) SamplerTexturePair
{
    uint32_t Texture{ 0u };
    uint32_t Sampler{ 0u };
};

/** @brief The `uniform` parameters of ordinary data on one entry point instance, in the layout of this profile's
 * target: the entry point constants. `Delivery` is an `EntryPointConstantsDelivery`. For a uniform buffer,
 * `Binding` is a position in the instance's visibility list, so `EntryPointInstanceView::Layout()[Binding]` is that
 * buffer. A push constant block has no placement, and `Binding` is 0. An entry point that reads no such data refers
 * to a record with the delivery `None` and no members. */
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

/** @brief What one entry point of one variant resolves to. */
struct alignas(8) EntryPointInstance
{
    uint32_t Source{ 0u };
    /** @brief Which of the variant's resources this entry point reads. */
    uint32_t Visibility{ 0u };
    uint32_t WorkgroupX{ 1u };
    uint32_t WorkgroupY{ 1u };
    uint32_t WorkgroupZ{ 1u };
    uint32_t Raster{ 0u };
    uint32_t SamplerPairs{ 0u };
    /** @brief An index into the entry point constants table of the environment. */
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

/** @brief One specialization constant (a WGSL `override`). Reserved: the cooker does not write this
 * table yet. `DefaultBits` stores the default value as raw bits of `ScalarType`. */
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

/** @brief Runs of vertex inputs and color targets. Compute entry points refer to a raster record with
* both counts at zero, so that every EntryPointInstance has a raster record and accessors do not need to
* check the stage. */
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
