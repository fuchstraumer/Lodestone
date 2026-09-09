#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_HPP
#define LODESTONE_SHADER_MANIFEST_HPP
#include "ShaderLibraryTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

/**
 * @brief A read-only view over one cooked shader module, stored as a flat byte span.
 *
 * The generated C++ library and this manifest hold the same data. The library compiles into the
 * program. The manifest arrives as bytes, so a running program can accept a new one.
 *
 * Every cross-reference in the file is a uint32 index, never a pointer. The reader is therefore a set
 * of spans over one byte span. It allocates nothing to open a file, and it relocates nothing.
 *
 * The byte span must outlive every view and every provider that reads it. Names and shader text point
 * into that span.
 */
namespace lodestone
{

inline constexpr uint32_t k_ShaderManifestMagic = 0x48535856u;
inline constexpr uint32_t k_ShaderManifestVersion = 2u;

// clang-tidy complains about enums being too big, but uint32_t means
// the error struct is 16bytes, which is great alignment and still compact
// NOLINTBEGIN(readability-enum-initial-value, performance-enum-size)
enum class ShaderManifestErrorCode : uint32_t
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
    VariantKeyVariantCountMismatch = 23,
    VariantSlotOutOfRange = 24,
    InvalidRasterVertexInputRange = 25,
    InvalidRasterColorTargetRange = 26,
    InvalidVertexInput = 27,
    InvalidSlotVisiblityIndex = 28,
    InvalidVariantFootprintListIndex = 29,
    InvalidUniformMember = 30,
    Count
};

enum class ShaderManifestTable : uint32_t
{
    Invalid,
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
    Axes,
    AxisValues,
    Rasters,
    VertexInputs,
    ColorTargets,
    UniformMembers
};
//NOLINTEND(readability-enum-initial-value, performance-enum-size)

/**@brief Contains contextual information on an error found during
 * the parsing or validation of a shader manifest. Using the record
 * index and the offset + the code, we can generate useful error
 * messages. */
struct ShaderManifestError
{
    ShaderManifestErrorCode Code{ ShaderManifestErrorCode::Invalid };
    ShaderManifestTable Table{ ShaderManifestTable::Invalid };
    // index of the offending record within the table
    uint32_t RecordIndex{ 0u };
    // offending value, or bound, or version: varies based on error code and table
    uint32_t Detail{ 0u };
};

template<typename T>
using ManifestResult = std::expected<T, ShaderManifestError>;

std::string_view ToString(ShaderManifestErrorCode error) noexcept;

/** @brief Fixed header at offset zero. Each section is an offset from the start of the file and a
 * count of records. All values are little-endian. */
struct alignas(8) ShaderManifestHeader
{
    uint32_t Magic{ 0u };
    uint32_t Version{ 0u };
    uint32_t FileSize{ 0u };
    uint32_t ModuleNameString{ 0u };

    uint32_t StringTableOffset{ 0u };
    uint32_t StringCount{ 0u };
    uint32_t StringBlobOffset{ 0u };
    uint32_t StringBlobSize{ 0u };

    uint32_t SourceTableOffset{ 0u };
    uint32_t SourceCount{ 0u };
    uint32_t SourceBlobOffset{ 0u };
    uint32_t SourceBlobSize{ 0u };

    uint32_t BindingTableOffset{ 0u };
    uint32_t BindingCount{ 0u };
    uint32_t ResourceListTableOffset{ 0u };
    uint32_t ResourceListCount{ 0u };

    uint32_t ResourceIndexTableOffset{ 0u };
    uint32_t ResourceIndexCount{ 0u };
    uint32_t FootprintTableOffset{ 0u };
    uint32_t FootprintCount{ 0u };

    uint32_t FootprintListTableOffset{ 0u };
    uint32_t FootprintListCount{ 0u };
    uint32_t VisibilityListTableOffset{ 0u };
    uint32_t VisibilityListCount{ 0u };

    uint32_t VisibilityIndexTableOffset{ 0u };
    uint32_t VisibilityIndexCount{ 0u };

    uint32_t EntryPointTableOffset{ 0u };
    uint32_t EntryPointCount{ 0u };
    uint32_t SlotTableOffset{ 0u };
    uint32_t SlotCount{ 0u };

    uint32_t VariantTableOffset{ 0u };
    uint32_t VariantCount{ 0u };
    uint32_t VariantKeyTableOffset{ 0u };
    uint32_t VariantKeyCount{ 0u };

    uint32_t AxisTableOffset{ 0u };
    uint32_t AxisCount{ 0u };
    uint32_t AxisValueTableOffset{ 0u };
    uint32_t AxisValueCount{ 0u };

    uint32_t RasterTableOffset{ 0u };
    uint32_t RasterCount{ 0u };
    uint32_t VertexInputTableOffset{ 0u };
    uint32_t VertexInputCount{ 0u };
    uint32_t ColorTargetTableOffset{ 0u };
    uint32_t ColorTargetCount{ 0u };
    uint32_t UniformMemberTableOffset{ 0u };
    uint32_t UniformMemberCount{ 0u };
};

struct alignas(8) ManifestStringRef
{
    uint32_t Offset{ 0u };
    uint32_t Length{ 0u };
};

/** @brief One resource binding. Field order puts the 8-byte members first, so the record needs no
 * padding on any target and its size stays the same on every compiler. */
struct alignas(8) ManifestBinding
{
    uint64_t ByteSize{ 0u };
    uint32_t NameString{ 0u };
    uint32_t ScopeString{ 0u };
    uint32_t Group{ 0u };
    uint32_t Binding{ 0u };
    uint32_t ElementStride{ 0u };
    uint32_t ArrayCount{ 1u };
    uint32_t StorageFormat{ 0u };
    uint32_t FirstUniformMember{ 0u };
    uint32_t UniformMemberCount{ 0u };
    uint32_t Reserved2{ 0u };
    uint8_t PlacementKind{ 0u };
    uint8_t Kind{ 0u };
    uint8_t Shape{ 0u };
    uint8_t SampleType{ 0u };
    uint8_t StorageAccess{ 0u };
    uint8_t SamplerType{ 0u };
    uint8_t Reserved0{ 0u };
    uint8_t Reserved1{ 0u };
};

/**@brief "Footprint" refers to the memory footprint of a resource, insofar as we can declare it. `Kind`
 * specifies if this is a buffer, texture, or invalid. `ElementCount` is *only* valid fo buffers, and
 * `ExtentX/Y/Z` is only valid for a texture. The latter is NOT a byte size: it is pixel dims.*/
// todo-ship: Union ExtentX w ElementCount, or just replace ElementCount with ExtentX. That's what a buffer
// length is anyways. This gets us to a round 16 bytes, which is nice and aligned vs 24 now
struct alignas(8) ManifestFootprint
{
    uint64_t ElementCount{ 0u };
    uint32_t ExtentX{ 0u };
    uint32_t ExtentY{ 0u };
    uint32_t ExtentZ{ 0u };
    uint32_t Kind{ 0u };
};

/** @brief A run in an index table. Used for a resource list and for a visibility list. Variants can have
 *  different counts of resources, so this allows us to compact them efficiently in the binary schema. */
struct alignas(8) ManifestRun
{
    uint32_t First{ 0u };
    uint32_t Count{ 0u };
};

struct alignas(8) ManifestEntryPoint
{
    uint32_t NameString{ 0u };
    uint32_t Stage{ 0u };
};

/** @brief What one entry point of one variant resolves to. */
struct alignas(8) ManifestSlot
{
    uint32_t SourceIndex{ 0u };
    /** @brief Index into visibility list table: which of the variant's resources this entry point reads.*/
    uint32_t VisibilityIndex{ 0u };
    uint32_t WorkgroupX{ 1u };
    uint32_t WorkgroupY{ 1u };
    uint32_t WorkgroupZ{ 1u };
    uint32_t RasterIndex{ 0u };
};

struct alignas(8) ManifestVertexInput
{
    uint32_t SemanticNameString{ 0u };
    uint32_t SemanticIndex{ 0u };
    uint32_t Location{ 0u };
    uint32_t ScalarType{ 0u };
    uint32_t ComponentCount{ 0u };
    uint32_t Reserved{ 0u };
};

struct alignas(8) ManifestUniformMember
{
    uint32_t NameString{ 0u };
    uint32_t Offset{ 0u };
    uint32_t Size{ 0u };
    uint32_t ArrayCount{ 1u };
};

struct alignas(8) ManifestColorTarget
{
    uint32_t Location{ 0u };
    uint32_t ScalarType{ 0u };
    uint32_t ComponentCount{ 0u };
    uint32_t Reserved{ 0u };
};

/** @brief Runs of vertex inputs and color targets. A compute entry point names a raster record whose
 * counts are both zero, so every slot can name one and no accessor needs a stage test. */
struct alignas(8) ManifestRaster
{
    uint32_t FirstVertexInput{ 0u };
    uint32_t VertexInputCount{ 0u };
    uint32_t FirstColorTarget{ 0u };
    uint32_t ColorTargetCount{ 0u };
    uint32_t WritesFragDepth{ 0u };
    uint32_t Reserved{ 0u };
};

struct alignas(8) ManifestVariant
{
    uint32_t Index{ 0u };
    uint32_t FirstSlot{ 0u };
    uint32_t SlotCount{ 0u };
    uint32_t SuffixString{ 0u };
    /** @brief What this variant declares, and how much of each. Both are per variant. */
    uint32_t ResourceListIndex{ 0u };
    uint32_t FootprintListIndex{ 0u };
};

struct alignas(8) ManifestAxis
{
    uint32_t NameString{ 0u };
    uint32_t FirstValue{ 0u };
    uint32_t ValueCount{ 0u };
    uint32_t Reserved{ 0u };
};

/** The reader reinterprets manifest bytes as records, so a record must be a bag of bytes.
 *
 * A record that held a pointer, a `std::string`, or a virtual table would make the reader read a
 * pointer out of a file. Nothing else in this repository catches that.
 *
 * Record sizes and layouts are not pinned yet, as we're still building out this library.*/
// todo-ship: Better versioning system, graceful extension of fields, converters between versions
template<typename RecordType>
inline constexpr bool k_IsManifestRecord =
    std::is_trivially_copyable_v<RecordType> && alignof(RecordType) <= 8u;

static_assert(k_IsManifestRecord<ShaderManifestHeader>);
static_assert(k_IsManifestRecord<ManifestStringRef>);
static_assert(k_IsManifestRecord<ManifestBinding>);
static_assert(k_IsManifestRecord<ManifestRun>);
static_assert(k_IsManifestRecord<ManifestFootprint>);
static_assert(k_IsManifestRecord<ManifestEntryPoint>);
static_assert(k_IsManifestRecord<ManifestSlot>);
static_assert(k_IsManifestRecord<ManifestVertexInput>);
static_assert(k_IsManifestRecord<ManifestUniformMember>);
static_assert(k_IsManifestRecord<ManifestColorTarget>);
static_assert(k_IsManifestRecord<ManifestRaster>);
static_assert(k_IsManifestRecord<ManifestVariant>);
static_assert(k_IsManifestRecord<ManifestAxis>);

/**
 * @brief Spans over one manifest byte span, checked once when it opens.
 *
 * Open() checks the magic, the version, and that every section lies inside the file. After it returns
 * a view, no accessor can read outside the span, so the accessors stay branch-light.
 */
class ShaderManifestView
{
public:
    ShaderManifestView() noexcept;

    static ManifestResult<ShaderManifestView> Open(std::span<const std::byte> bytes) noexcept;

    [[nodiscard]] std::string_view ModuleName() const noexcept;
    [[nodiscard]] std::string_view String(uint32_t string_index) const noexcept;
    [[nodiscard]] std::string_view Source(uint32_t source_index) const noexcept;

    [[nodiscard]] std::span<const ManifestBinding> Bindings() const noexcept;
    /** @brief The resources one variant declares. Indices into Bindings(). */
    [[nodiscard]] std::span<const uint32_t> ResourceList(uint32_t list_index) const noexcept;
    /** @brief How much of each resource, in the same order as the resource list. */
    [[nodiscard]] std::span<const ManifestFootprint> FootprintList(uint32_t list_index) const noexcept;
    /** @brief Which of a variant's resources one entry point reads. Indices into the resource list. */
    [[nodiscard]] std::span<const uint32_t> VisibilityList(uint32_t list_index) const noexcept;
    [[nodiscard]] std::span<const ManifestEntryPoint> EntryPoints() const noexcept;
    [[nodiscard]] std::span<const ManifestVariant> Variants() const noexcept;
    [[nodiscard]] std::span<const uint64_t> VariantKeys() const noexcept;
    [[nodiscard]] std::span<const ManifestAxis> Axes() const noexcept;
    [[nodiscard]] std::span<const int64_t> AxisValues(uint32_t axis_index) const noexcept;

    [[nodiscard]] std::span<const ManifestVertexInput> VertexInputs(uint32_t raster_index) const noexcept;
    [[nodiscard]] std::span<const ManifestColorTarget> ColorTargets(uint32_t raster_index) const noexcept;
    [[nodiscard]] bool WritesFragDepth(uint32_t raster_index) const noexcept;

    [[nodiscard]] std::span<const ManifestUniformMember> UniformMembers(
        const ManifestBinding& binding) const noexcept;

    /** @brief The slot for one entry point of one variant, or nullptr when the pair does not exist.
     *
     * `entry_point` is the `EntryPointId` value, so it counts from one and zero is Invalid.
     * `variant_index` is the dense index, the same number the generated library uses. */
    [[nodiscard]] const ManifestSlot* FindSlot(uint32_t entry_point, uint64_t variant_key) const noexcept;
    /** @brief One slot for each entry point of this variant, in entry point order. */
    [[nodiscard]] std::span<const ManifestSlot> Slots(const ManifestVariant& variant) const noexcept;
    /** @brief Every slot, in file order. */
    [[nodiscard]] std::span<const ManifestSlot> SlotTable() const noexcept;

private:
    std::span<const std::byte> bytes;
    const ShaderManifestHeader* header{ nullptr };
    std::span<const ManifestStringRef> strings;
    std::span<const ManifestStringRef> sources;
    std::span<const ManifestBinding> bindings;
    std::span<const ManifestRun> resourceLists;
    std::span<const uint32_t> resourceIndices;
    std::span<const ManifestFootprint> footprints;
    std::span<const ManifestRun> footprintLists;
    std::span<const ManifestRun> visibilityLists;
    std::span<const uint32_t> visibilityIndices;
    std::span<const ManifestEntryPoint> entryPoints;
    std::span<const ManifestSlot> slots;
    std::span<const ManifestVariant> variants;
    std::span<const uint64_t> variantKeys;
    std::span<const ManifestAxis> axes;
    std::span<const int64_t> axisValues;
    std::span<const ManifestRaster> rasterStates;
    std::span<const ManifestVertexInput> vertexInputs;
    std::span<const ManifestColorTarget> colorTargets;
    std::span<const ManifestUniformMember> uniformMembers;
};

/**
 * @brief Serves shader sources out of a manifest instead of out of generated C++.
 *
 * This is the second implementation of ShaderSourceProvider, and the reason the interface exists. A
 * watch-and-serve cooker sends a new manifest, the caller builds a new provider, and Generation()
 * moves. Nothing in the rendergraph changes.
 *
 * The constructor converts the manifest binding records into BindingInfo once. That is the only
 * allocation, and it is needed because BindingInfo holds string views while the file holds indices.
 */
class ManifestShaderSourceProvider final : public ShaderSourceProvider
{
public:
    ManifestShaderSourceProvider(ShaderManifestView view, uint64_t generation) noexcept;
    ~ManifestShaderSourceProvider() override;

    [[nodiscard]] std::string_view Source(uint32_t entry_point,
                                          uint32_t variant_index) const noexcept override;
    [[nodiscard]] std::span<const BindingInfo> Bindings(uint32_t entry_point,
                                                        uint32_t variant_index) const noexcept override;
    [[nodiscard]] WorkgroupSize Workgroup(uint32_t entry_point,
                                          uint32_t variant_index) const noexcept override;
    [[nodiscard]] uint64_t Generation() const noexcept override;

    [[nodiscard]] const ShaderManifestView& View() const noexcept;

private:
    ShaderManifestView view;
    /** Built before bindingInfos and reserved to its final size, so the spans below stay valid. */
    std::vector<UniformMemberInfo> memberInfos;
    /** One entry for each slot, gathered from the resource list and the footprint list of the slot's
     * variant. A layout is a subset of what the variant declares, so it is not a run of the resource
     * table and has to be materialized. */
    std::vector<BindingInfo> bindingInfos;
    /** Where each slot's bindings begin in bindingInfos, and how many there are. */
    std::vector<uint32_t> slotFirstBinding;
    std::vector<uint32_t> slotBindingCount;

    void GatherVariantBindings(const ManifestVariant& variant, const std::vector<uint32_t>& member_offsets);
    [[nodiscard]] BindingInfo MakeBindingInfo(const ManifestBinding& record,
                                              const ManifestFootprint* footprint,
                                              uint32_t member_offset) const noexcept;
    uint64_t generation{ 0u };
};

} // namespace lodestone

#endif // !LODESTONE_SHADER_MANIFEST_HPP
