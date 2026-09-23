#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_HPP
#define LODESTONE_SHADER_MANIFEST_HPP
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

/**
 * @brief A read-only view over one cook bundle, stored as a flat byte span.
 *
 * Every cross-reference in the file is an index or an offset, never a pointer. The header region uses
 * absolute 64-bit offsets. An environment extent uses 32-bit offsets relative to its own start. The
 * reader is therefore a set of spans over one byte span. It allocates nothing to open a file, and it
 * relocates nothing.
 *
 * The byte span must outlive every view and every provider that reads it. Names and shader text point
 * into that span.
 */
namespace lodestone::manifest
{

inline constexpr uint32_t k_ShaderManifestMagic = 0x48535856u;
inline constexpr uint32_t k_ShaderManifestVersion = 4u;

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
    InvalidSlotSourceIndex = 19,
    InvalidSlotVisibilityIndex = 20,
    InvalidSlotRasterIndex = 21,
    InvalidVariantKeyOrder = 22,
    SlotGridSizeMismatch = 23,
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
    Count
};

enum class ShaderManifestTable : uint32_t
{
    Invalid,
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
    Slots,
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
 * folds in the table, record index, and `Detail` field, so the reader states where the file is bad */
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
 * The file is ordered by profile, then by module. The header region (`HeaderSize` bytes) holds this
 * record, the whole-cook tables, the module headers, and the environment directory. A reader loads the
 * header region, then one contiguous run of environment extents. */
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
    uint64_t ProfileCount{ 0u };
    uint64_t ProfileTableOffset{ 0u };
    /** @brief A dense grid of `ProfileCount * ModuleCount` entries. The entry for (profile P, module M)
     * is at `P * ModuleCount + M`. */
    uint64_t EnvironmentDirectoryOffset{ 0u };

    uint64_t StringCount{ 0u };
    uint64_t StringTableOffset{ 0u };
    uint64_t StringBlobSize{ 0u };
    uint64_t StringBlobOffset{ 0u };

    uint64_t AxisCount{ 0u };
    uint64_t AxisTableOffset{ 0u };
    uint64_t AxisValueCount{ 0u };
    uint64_t AxisValueTableOffset{ 0u };
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
    uint8_t Reserved0{ 0u };
    uint16_t Reserved1{ 0u };
    uint32_t Reserved2{ 0u };
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
};

// A module has *one* core module root header, which describes the data that is environment-agnostic
// and doesn't change for different targets and profiles.
struct alignas(8) ModuleRootHeader
{
    uint32_t ModuleNameString{ 0u };
    uint32_t Reserved{ 0u };
    uint64_t EntryPointCount{ 0u };
    uint64_t EntryPointTableOffset{ 0u };
    uint64_t ModuleAxisCount{ 0u };
    uint64_t ModuleAxisTableOffset{ 0u };
};

struct alignas(8) EntryPoint
{
    uint32_t NameString{ 0u };
    uint32_t Stage{ 0u };
};

/** @brief The first record of one environment extent: one module, cooked for one profile.
 *
 * Every offset is relative to the start of the extent, so an extent can load into its own buffer with
 * no fixup. Three tables have no count, because the data already fixes their size:
 * - Variant keys and variant records share a size
 * - The axis mask table holds one bitmask per variant. The length of the individual records
 *   is somewhat unique though: ceil(ModuleAxisCount / 64) words per variant, since it's a bitmask
 * - The slot table (a slot being a distinct entrypoint instance) is `VariantCount * EntryPointCount` records.
 *   Thus, the slot for (variant V, entry point E) is at `V * EntryPointCount + E`. */
struct alignas(8) EnvironmentHeader
{
    uint32_t VariantCount{ 0u };
    uint32_t VariantKeyTableOffset{ 0u };
    uint32_t VariantTableOffset{ 0u };
    uint32_t AxisMaskTableOffset{ 0u };
    uint32_t SlotTableOffset{ 0u };
    uint32_t Reserved{ 0u };

    uint32_t SourceCount{ 0u };
    uint32_t SourceTableOffset{ 0u };
    uint32_t SourceBlobSize{ 0u };
    uint32_t SourceBlobOffset{ 0u };

    uint32_t BindingCount{ 0u };
    uint32_t BindingTableOffset{ 0u };
    uint32_t ResourceListCount{ 0u };
    uint32_t ResourceListTableOffset{ 0u };
    uint32_t ResourceIndexCount{ 0u };
    uint32_t ResourceIndexTableOffset{ 0u };
    uint32_t FootprintCount{ 0u };
    uint32_t FootprintTableOffset{ 0u };
    uint32_t FootprintListCount{ 0u };
    uint32_t FootprintListTableOffset{ 0u };
    uint32_t VisibilityListCount{ 0u };
    uint32_t VisibilityListTableOffset{ 0u };
    uint32_t VisibilityIndexCount{ 0u };
    uint32_t VisibilityIndexTableOffset{ 0u };
    uint32_t RasterCount{ 0u };
    uint32_t RasterTableOffset{ 0u };
    uint32_t VertexInputCount{ 0u };
    uint32_t VertexInputTableOffset{ 0u };
    uint32_t ColorTargetCount{ 0u };
    uint32_t ColorTargetTableOffset{ 0u };
    uint32_t UniformMemberCount{ 0u };
    uint32_t UniformMemberTableOffset{ 0u };
    // Currently unused, but reserved as we are trying to get it up asap
    uint32_t SpecializationConstantCount{ 0u };
    uint32_t SpecializationConstantTableOffset{ 0u };
};

/** @brief One variant of one environment. Its key is at the same position in the key table. The
 * capability requirement is unused, as mentioned earlier, while we wait to specify and build that */
struct alignas(8) Variant
{
    uint32_t SuffixString{ 0u };
    uint32_t ResourceListIndex{ 0u };
    uint32_t FootprintListIndex{ 0u };
    uint32_t CapabilityRequirement{ 0u };
};

/** @brief Where a shader reaches one resource. `Binding::PlacementKind` selects how to read it:
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
    uint8_t PlacementKind{ 0u };
    uint8_t Kind{ 0u };
    uint8_t Shape{ 0u };
    uint8_t IsComparisonSampler{ 0u };
    uint8_t Access{ 0u };
    uint8_t Reserved0{ 0u };
    uint16_t Reserved1{ 0u };
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

/** @brief What one entry point of one variant resolves to. */
struct alignas(8) EntryPointInstance
{
    uint32_t SourceIndex{ 0u };
    /** @brief Index into visibility list table: which of the variant's resources this entry point reads.*/
    uint32_t VisibilityIndex{ 0u };
    uint32_t WorkgroupX{ 1u };
    uint32_t WorkgroupY{ 1u };
    uint32_t WorkgroupZ{ 1u };
    uint32_t RasterIndex{ 0u };
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
 * table yet. `DefaultBits` holds the default value as raw bits of `ScalarType`. */
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

/** @brief Runs of vertex inputs and color targets. A compute entry point names a raster record whose
* counts are both zero, so every slot can name one and no accessor needs a stage test. */
struct alignas(8) RasterState
{
    uint32_t FirstVertexInput{ 0u };
    uint32_t VertexInputCount{ 0u };
    uint32_t FirstColorTarget{ 0u };
    uint32_t ColorTargetCount{ 0u };
    uint32_t WritesFragDepth{ 0u };
    uint32_t Reserved{ 0u };
};

class ModuleView;
class EnvironmentView;

/**
* @brief Spans over the header region of one cook bundle, checked once when it opens.
*
* The header region holds every whole-cook table and every module header: strings, axes, profiles, the
* environment directory, and each module's entry points and axis runs. Open() checks all of it, so every
* accessor after it runs unchecked (effectively meaning it's branch-free).
*
* The span given to Open() can hold the whole file, or only the header region. A renderer that reads the
* header region first, and then reads the extents of one profile, opens each extent with
* `EnvironmentView::Open`. A caller that holds the whole file uses `OpenEnvironment`.
* @note This stays purely in the vocabulary of the manifest itself. For the vocabulary of authorship,
* use `ManifestIndex`.
*
* todo: File-reading open implementation using a better fileread backend (not ifstream)
*/
class BundleView
{
public:
    BundleView() noexcept;

    static ManifestResult<BundleView> Open(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::string_view String(uint32_t string_index) const noexcept;
    [[nodiscard]] uint32_t StringCount() const noexcept;
    [[nodiscard]] std::span<const Axis> Axes() const noexcept;
    [[nodiscard]] const Axis& AxisData(uint32_t axis_index) const noexcept;
    [[nodiscard]] std::span<const AxisValueType> AxisValues(uint32_t axis_index) const noexcept;
    [[nodiscard]] std::span<const Profile> Profiles() const noexcept;
    [[nodiscard]] uint32_t ModuleCount() const noexcept;
    [[nodiscard]] ModuleView Module(uint32_t module_index) const noexcept;
    /** @brief The index of the module with this name, or -1 when no module has it. */
    [[nodiscard]] int32_t FindModule(std::string_view module_name) const noexcept;
    /** @brief The index of the first profile for this target, or -1 when no profile has it. */
    [[nodiscard]] int32_t FindProfile(std::string_view target_name) const noexcept;
    [[nodiscard]] const EnvironmentDirectoryEntry& Environment(uint32_t profile_index,
                                                               uint32_t module_index) const noexcept;
    /** @brief Opens one environment out of the span this view was opened with. That span must reach the
     * extent, so a view over the header region alone returns `EnvironmentExtentOutOfBounds`. */
    [[nodiscard]] ManifestResult<EnvironmentView> OpenEnvironment(uint32_t profile_index,
                                                                  uint32_t module_index) const noexcept;

private:
    friend class ModuleView;
    std::span<const std::byte> bytes;
    const Header* header{ nullptr };
    std::span<const StringRef> strings;
    std::span<const Axis> axes;
    std::span<const AxisValueType> axisValues;
    std::span<const Profile> profiles;
    std::span<const ModuleRootHeader> moduleHeaders;
    std::span<const EnvironmentDirectoryEntry> directory;
};

/**
* @brief One module of a bundle: its name, its entry points, and its axes. Nothing here varies by profile.
*
* A module axis is a view of one root axis through the module's value mask. Digit D of a variant key
* selects the D-th set bit of that mask, so the radix of the axis in this module is the count of set bits.
*/
class ModuleView
{
public:
    ModuleView() noexcept;

    [[nodiscard]] const BundleView& Bundle() const noexcept;
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] std::span<const EntryPoint> EntryPoints() const noexcept;
    [[nodiscard]] std::span<const ModuleAxis> ModuleAxes() const noexcept;
    [[nodiscard]] uint32_t AxisCount() const noexcept;
    /** @brief The root axis record behind module axis `local_axis`. */
    [[nodiscard]] const Axis& AxisData(uint32_t local_axis) const noexcept;
    /** @brief How many values this module uses on one axis. This is also the radix of that axis. */
    [[nodiscard]] uint32_t AxisValueCount(uint32_t local_axis) const noexcept;
    [[nodiscard]] AxisValueType AxisValue(uint32_t local_axis, uint32_t digit) const noexcept;
    /** @brief The 64-bit words in one variant's axis-active mask: `ceil(AxisCount() / 64)`. */
    [[nodiscard]] uint32_t AxisMaskWordCount() const noexcept;

private:
    friend class BundleView;
    ModuleView(const BundleView& _bundle, uint32_t module_index) noexcept;
    BundleView bundle;
    uint32_t moduleIndex{ 0u };
    std::span<const EntryPoint> entryPoints;
    std::span<const ModuleAxis> moduleAxes;
};

/**
* @brief One module, cooked for one profile: spans over one environment extent, checked once when it opens.
*
* Every offset inside an extent is relative to the extent start, so the extent can sit in its own buffer.
* The variant keys, the variant records, and the axis masks are parallel: the position of a key is the
* index of its variant. The slot for (variant V, entry point E) is at `V * EntryPointCount + E`.
*/
class EnvironmentView
{
public:
    EnvironmentView() noexcept;

    static ManifestResult<EnvironmentView> Open(const BundleView& bundle,
                                                uint32_t profile_index,
                                                uint32_t module_index,
                                                std::span<const std::byte> extent_bytes) noexcept;

    [[nodiscard]] const ModuleView& Module() const noexcept;
    [[nodiscard]] uint32_t ProfileIndex() const noexcept;
    [[nodiscard]] const Profile& ProfileRecord() const noexcept;
    [[nodiscard]] std::string_view String(uint32_t string_index) const noexcept;
    [[nodiscard]] std::string_view Source(uint32_t source_index) const noexcept;

    [[nodiscard]] std::span<const VariantKey> VariantKeys() const noexcept;
    [[nodiscard]] std::span<const Variant> Variants() const noexcept;
    /** @brief The index of the variant with this key, or -1 when this environment did not cook it. */
    [[nodiscard]] int32_t FindVariant(VariantKey key) const noexcept;
    [[nodiscard]] std::span<const uint64_t> AxisMask(uint32_t variant_index) const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t variant_index, uint32_t local_axis) const noexcept;

    [[nodiscard]] std::span<const EntryPointInstance> SlotTable() const noexcept;
    /** @brief One slot for each entry point of this variant, in entry point order. */
    [[nodiscard]] std::span<const EntryPointInstance> VariantSlots(uint32_t variant_index) const noexcept;
    /** @brief The entry-point specific information for one entry point of one variant, or null when this
     * environment did not cook the key. */
    [[nodiscard]] const EntryPointInstance* FindSlot(uint32_t entry_point, VariantKey variant) const noexcept;

    [[nodiscard]] std::span<const Binding> Bindings() const noexcept;
    /** @brief The resources one variant declares. Indices into Bindings(). */
    [[nodiscard]] std::span<const uint32_t> ResourceList(uint32_t list_index) const noexcept;
    /** @brief How much of each resource, in the same order as the resource list. */
    [[nodiscard]] std::span<const Footprint> FootprintList(uint32_t list_index) const noexcept;
    /** @brief Which of a variant's resources one entry point reads. Indices into the resource list. */
    [[nodiscard]] std::span<const uint32_t> VisibilityList(uint32_t list_index) const noexcept;
    [[nodiscard]] std::span<const UniformMember> UniformMembers(const Binding& binding) const noexcept;
    [[nodiscard]] std::span<const VertexInput> VertexInputs(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const ColorTarget> ColorTargets(uint32_t raster_index) const noexcept;
    [[nodiscard]] bool WritesFragDepth(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const SpecializationConstant> SpecializationConstants() const noexcept;

private:
    ModuleView module;
    uint32_t profileIndex{ 0u };
    std::span<const std::byte> extent;
    std::span<const VariantKey> variantKeys;
    std::span<const Variant> variants;
    std::span<const uint64_t> axisMasks;
    std::span<const EntryPointInstance> slots;
    std::span<const SourceRef> sources;
    std::span<const char> sourceBlob;
    std::span<const Binding> bindings;
    std::span<const Run> resourceLists;
    std::span<const uint32_t> resourceIndices;
    std::span<const Footprint> footprints;
    std::span<const Run> footprintLists;
    std::span<const Run> visibilityLists;
    std::span<const uint32_t> visibilityIndices;
    std::span<const RasterState> rasterStates;
    std::span<const VertexInput> vertexInputs;
    std::span<const ColorTarget> colorTargets;
    std::span<const UniformMember> uniformMembers;
    std::span<const SpecializationConstant> specializationConstants;
};

/**
 * @brief Serves shader sources out of one environment of a manifest.
 *
 * This is now the only implementation of ShaderSourceProvider, after removal of the old header path.
 * The constructor converts the manifest binding records into BindingInfo once. That is the only
 * allocation, and it is needed because BindingInfo holds string views while the file holds indices.
 *
 * `Generation()` is the future hot-reload hook. A provider for baked data will always return the same value,
 * but a live provider can increment the value when any source changes - allowing users to reload
 * shaders and reset state gracefully
 */
class ShaderSourceProvider
{
public:
    ShaderSourceProvider(EnvironmentView view, uint64_t generation) noexcept;

    [[nodiscard]] std::string_view Source(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    [[nodiscard]] std::span<const BindingInfo> Bindings(uint32_t entry_point,
                                                        VariantKey variant) const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    [[nodiscard]] uint64_t Generation() const noexcept;

    [[nodiscard]] const EnvironmentView& View() const noexcept;

private:
    EnvironmentView view;
    /** Built before bindingInfos and reserved to its final size, so the spans below stay valid. */
    std::vector<UniformMemberInfo> memberInfos;
    /** One entry for each slot, gathered from the resource list and the footprint list of the slot's
     * variant. A layout is a subset of what the variant declares, so it is not a run of the resource
     * table and has to be materialized. */
    std::vector<BindingInfo> bindingInfos;
    /** Where each slot's bindings begin in bindingInfos, and how many there are. */
    std::vector<uint32_t> slotFirstBinding;
    std::vector<uint32_t> slotBindingCount;

    void GatherVariantBindings(uint32_t variant_index, const std::vector<uint32_t>& member_offsets);
    [[nodiscard]] BindingInfo MakeBindingInfo(const Binding& record,
                                              const Footprint* footprint,
                                              uint32_t member_offset) const noexcept;
    uint64_t generation{ 0u };
};

} // namespace lodestone::manifest

#endif // !LODESTONE_SHADER_MANIFEST_HPP
