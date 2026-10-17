#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_HPP
#define LODESTONE_SHADER_MANIFEST_HPP
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>

/**
 * @brief A read-only view over one cook bundle, stored as a flat byte span.
 *
 * Every cross-reference in the file is an index or an offset, not a pointer. The header region uses
 * absolute 64-bit offsets, and environment extents use 32-bit offsets relative to their own start. This
 * lets the reader work as a set of spans over one byte span, so opening a file needs no allocation and
 * no pointer fixup.
 *
 * The byte span must outlive every view and every provider that reads it. Names and shader text point
 * into that span.
 */
namespace lodestone::manifest
{

inline constexpr uint32_t k_ShaderManifestMagic = 0x48535856u;
inline constexpr uint32_t k_ShaderManifestVersion = 9u;

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
 *   entry point's instance in this environment, so a consumer must pin it. */
struct alignas(8) EnvironmentHeader
{
    uint32_t VariantCount{ 0u };
    uint32_t VariantKeyTableOffset{ 0u };
    uint32_t VariantTableOffset{ 0u };
    uint32_t AxisMaskTableOffset{ 0u };
    uint32_t EntryPointInstanceTableOffset{ 0u };
    uint32_t AxisRelevanceTableOffset{ 0u };

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
    uint8_t PlacementKind{ 0u };
    uint8_t Kind{ 0u };
    uint8_t Shape{ 0u };
    uint8_t IsComparisonSampler{ 0u };
    uint8_t Access{ 0u };
    uint8_t SampleType{ 0u };
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
    uint32_t Source{ 0u };
    /** @brief Which of the variant's resources this entry point reads. */
    uint32_t Visibility{ 0u };
    uint32_t WorkgroupX{ 1u };
    uint32_t WorkgroupY{ 1u };
    uint32_t WorkgroupZ{ 1u };
    uint32_t Raster{ 0u };
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

class ModuleView;
class EnvironmentView;

/** One member of a uniform block. Use it to check that a CPU struct matches the shader layout. */
class UniformMemberView
{
public:
    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] uint32_t Offset() const noexcept;
    [[nodiscard]] uint32_t Size() const noexcept;
    [[nodiscard]] uint32_t ArrayCount() const noexcept;
    [[nodiscard]] uint32_t ElementStride() const noexcept;
    [[nodiscard]] MatrixLayout Layout() const noexcept;
    [[nodiscard]] const UniformMember& Record() const noexcept;

private:
    friend class UniformMemberRange;
    UniformMemberView(const EnvironmentView* _environment, const UniformMember* _record) noexcept;
    const EnvironmentView* environment{ nullptr };
    const UniformMember* record{ nullptr };
};

/** The members of one uniform block, in declaration order. Empty for every other binding kind. */
class UniformMemberRange
{
public:
    class Iterator
    {
    public:
        Iterator() noexcept = default;
        explicit Iterator(const UniformMemberRange* _range, uint32_t _position) noexcept
            : range(_range), position(_position) {}

        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = UniformMemberView;
        using reference = UniformMemberView;
        using pointer = void;

        [[nodiscard]] UniformMemberView operator*() const noexcept
        {
            return range->operator[](position);
        }

        Iterator& operator++() noexcept
        {
            ++position;
            return *this;
        }

        Iterator operator++(int) noexcept
        {
            Iterator previous = *this;
            ++position;
            return previous;
        }

        [[nodiscard]] bool operator==(const Iterator& other) const noexcept
        {
            return range == other.range && position == other.position;
        }

    private:
        const UniformMemberRange* range{ nullptr };
        uint32_t position{ 0u };
    };

    [[nodiscard]] Iterator begin() const noexcept
    {
        return Iterator{ this, 0u };
    }

    [[nodiscard]] Iterator end() const noexcept
    {
        return Iterator{ this, Size() };
    }

    [[nodiscard]] uint32_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] UniformMemberView operator[](uint32_t position) const noexcept;

private:
    friend class ResolvedResource;
    UniformMemberRange(const EnvironmentView* _environment, std::span<const UniformMember> _members) noexcept;
    const EnvironmentView* environment{ nullptr };
    std::span<const UniformMember> members;
};

/** A resource as seen by one entry point: its binding record and its footprint.
 * @note Views point into their `EnvironmentView`, so keep that environment alive and do not move it
 * while a view exists. */
class ResolvedResource
{
public:
    [[nodiscard]] std::string_view Name() const noexcept;
    [[nodiscard]] std::string_view ScopeName() const noexcept;
    [[nodiscard]] PlacementKind Placement() const noexcept;
    [[nodiscard]] PlacementPayload PlacementValue() const noexcept;
    [[nodiscard]] BindingKind Kind() const noexcept;
    [[nodiscard]] ResourceShape Shape() const noexcept;
    [[nodiscard]] ResourceAccess Access() const noexcept;
    [[nodiscard]] TextureFormat StorageFormat() const noexcept;
    /** @brief How a shader reads a sampled texture. Invalid for every other resource. */
    [[nodiscard]] TextureSampleType SampleType() const noexcept;
    [[nodiscard]] bool IsComparisonSampler() const noexcept;
    /** @brief The size of one structured buffer element, in bytes. Zero for a texture or a sampler. */
    [[nodiscard]] uint32_t ElementStride() const noexcept;
    [[nodiscard]] uint32_t ArrayCount() const noexcept;
    /** @brief The size of a uniform block, in bytes. Zero for every other binding kind. */
    [[nodiscard]] uint64_t ByteSize() const noexcept;
    /** @brief The element count from `[ls_element_count]`, for this variant. Zero when the shader does not
     * declare one, or when the resource has no footprint. In that case, the caller must provide the size. */
    [[nodiscard]] uint64_t ElementCount() const noexcept;
    /** @brief The texture extent from `[ls_extent_2d]` or `[ls_extent_3d]`. Zero when the shader does not
     * declare one, or when the resource has no footprint. In that case, the caller must provide the size. */
    [[nodiscard]] uint32_t ExtentX() const noexcept;
    [[nodiscard]] uint32_t ExtentY() const noexcept;
    [[nodiscard]] uint32_t ExtentZ() const noexcept;
    [[nodiscard]] UniformMemberRange Members() const noexcept;
    /** @brief The position of the binding record in `EnvironmentView::Bindings()`. */
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] const Binding& Record() const noexcept;
    /** @brief Null when the footprint list is shorter than the resource list. */
    [[nodiscard]] const Footprint* FootprintRecord() const noexcept;
    [[nodiscard]] bool operator==(const ResolvedResource& rhs) const noexcept;
    [[nodiscard]] bool operator!=(const ResolvedResource& rhs) const noexcept;
private:
    friend class LayoutRange;
    ResolvedResource(const EnvironmentView* _environment,
                     const Binding* _record,
                     const Footprint* _footprint) noexcept;
    const EnvironmentView* environment{ nullptr };
    const Binding* record{ nullptr };
    const Footprint* footprint{ nullptr };
};

/** One resource of a merged layout, and the stages that use it. */
struct MergedResource
{
    /** The view of the first stage that reads the resource, in the order given to `Merge`. */
    ResolvedResource Resource;
    ShaderStageKind Stages{ ShaderStageKind::Invalid };
};

/** The resources used by one entry point of one variant, in placement order. `EnvironmentView::Open`
 * validates this order: Bound resources come first, in strictly ascending (group, binding) order. */
class LayoutRange
{
public:
    /** An empty range. This is used by `MergedLayoutView`, which stores a fixed array of ranges. */
    LayoutRange() noexcept = default;

    class Iterator
    {
    public:
        // std::ranges::range needs a default constructor: end() must be semiregular.
        Iterator() noexcept = default;
        explicit Iterator(const LayoutRange* _range, uint32_t _position) noexcept
            : range(_range), position(_position) {}

        // contiguous iterator is not a valid tag, because this is a 
        // a proxy iterator that returns ResolvedResource by value, not by reference.
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = ResolvedResource;
        using pointer = value_type*;
        using reference = value_type&;

        // because ResolvedResource is always returned by value, operator-> is strictly invalid
        // and we need to make sure it's not possible to make this iterator return an address
        // of a temporary ResolvedResource
        void* operator->() const noexcept = delete;

        // these are all defined in the header to aid with inlining/lto across 
        // translation units, since they're more likely to be hot

        [[nodiscard]] ResolvedResource operator*() const noexcept
        {
            return range->operator[](position);
        }

        Iterator& operator++() noexcept
        {
            ++position;
            return *this;
        }

        Iterator operator++(int) noexcept
        {
            Iterator previous = *this;
            ++position;
            return previous;
        }

        Iterator operator+(difference_type n) const noexcept
        {
            return Iterator{ range, position + static_cast<uint32_t>(n) };
        }

        Iterator& operator+=(difference_type n) noexcept
        {
            position += static_cast<uint32_t>(n);
            return *this;
        }

        [[nodiscard]] bool operator==(const Iterator& other) const noexcept
        {
            return range == other.range && position == other.position;
        }

        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept
        {
            return !(*this == other);
        }

    private:
        friend class LayoutRange;
        const LayoutRange* range{ nullptr };
        uint32_t position{ 0u };
    };

    [[nodiscard]] Iterator begin() const noexcept
    {
        return Iterator{ this, 0u };
    }

    [[nodiscard]] Iterator end() const noexcept
    {
        return Iterator{ this, Size() };
    }

    [[nodiscard]] uint32_t Size() const noexcept;
    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] ResolvedResource operator[](uint32_t position) const noexcept;

private:
    friend class EntryPointInstanceView;
    LayoutRange(const EnvironmentView* _environment,
                std::span<const uint32_t> _visible,
                std::span<const uint32_t> _resources,
                std::span<const Footprint> _footprints) noexcept;
    const EnvironmentView* environment{ nullptr };
    std::span<const uint32_t> visible;
    std::span<const uint32_t> resources;
    std::span<const Footprint> footprints;
};

/** One entry point of one variant.
 * @note Iterators point into their range, so keep the range returned by `Layout()` alive while iterating. */
class EntryPointInstanceView
{
public:
    /** @brief The WGSL text. Only for a `Wgsl` profile. */
    [[nodiscard]] std::string_view Source() const noexcept;
    /** @brief The SPIR-V words. Only for a `Spirv` profile. */
    [[nodiscard]] std::span<const uint32_t> SpirvWords() const noexcept;
    [[nodiscard]] ShaderStageKind Stage() const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup() const noexcept;
    [[nodiscard]] LayoutRange Layout() const noexcept;
    //[[nodiscard]] RasterView Raster() const noexcept;
    [[nodiscard]] uint32_t Raster() const noexcept;
    [[nodiscard]] const EntryPointInstance& Record() const noexcept;

private:
    friend class VariantView;
    EntryPointInstanceView(const EnvironmentView* _environment,
                           const EntryPointInstance* entry_point_instance,
                           const Variant* _variant,
                           uint32_t entry_point) noexcept;
    const EnvironmentView* environment{ nullptr };
    const EntryPointInstance* entryPointInstance{ nullptr };
    const Variant* variant{ nullptr };
    // the position of the entry point in the module, which gives the stage
    uint32_t entryPoint{ 0u };
};

/** One variant of one environment. */
class VariantView
{
public:
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] VariantKey Key() const noexcept;
    [[nodiscard]] std::string_view Suffix() const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t local_axis) const noexcept;
    [[nodiscard]] uint32_t EntryPointCount() const noexcept;
    [[nodiscard]] EntryPointInstanceView EntryPoint(uint32_t entry_point) const noexcept;
    [[nodiscard]] const Variant& Record() const noexcept;

private:
    friend class EnvironmentView;
    VariantView(const EnvironmentView* _environment, uint32_t _index) noexcept;
    const EnvironmentView* environment{ nullptr };
    uint32_t index{};
};

/** @brief The merged resources of the entry point instances of one pipeline, in placement order.
 *
 * Pipeline layouts need each resource once, with every stage that reads it. `Merge` takes one instance
 * for each stage and validates every shared placement once, so that iteration afterwards runs unchecked.
 * Iteration returns one `MergedResource` for each (group, binding). Each change of group in the sequence
 * marks the start of the next bind group layout (or descriptor set layout).
 *
 * The instances can come from different variants, modules, or environments.
 * @note This view points into the environments of its instances, so keep them alive and do not move
 * them. Iterators point into the view, so keep the view alive while iterating. */
class MergedLayoutView
{
public:
    // over what we'll actually need, but keeps things nicely rounded
    static constexpr uint32_t k_MaxPipelineStages = 8u;

    class Iterator
    {
    public:
        // std::ranges::range needs a default constructor
        Iterator() noexcept = default;

        // a proxy iterator, as LayoutRange::Iterator is: it returns MergedResource by value
        using iterator_concept = std::input_iterator_tag;
        using iterator_category = std::input_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = MergedResource;
        using pointer = value_type*;
        using reference = value_type&;

        void* operator->() const noexcept = delete;

        [[nodiscard]] MergedResource operator*() const noexcept;
        Iterator& operator++() noexcept;
        Iterator operator++(int) noexcept;
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept;
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept;

    private:
        friend class MergedLayoutView;
        Iterator(const MergedLayoutView* _layout,
                 const std::array<uint32_t, k_MaxPipelineStages>& _positions) noexcept;
        const MergedLayoutView* layout{ nullptr };
        // one position for each stage's range; a stage at the end of its range has position == size
        std::array<uint32_t, k_MaxPipelineStages> positions{};
    };

    /** @brief Merges the layouts of one pipeline's entry point instances, one instance for each stage.
     *
     * Fails when:
     * - there are more than `k_MaxPipelineStages` instances (`MergedLayoutTooManyStages`)
     * - two instances have the same stage (`MergedLayoutDuplicateStage`)
     * - a resource is not Bound (`MergedLayoutPlacementUnsupported`) (other binding/layout forms coming later)
     * - two resources at one (group, binding) differ in kind, shape, access, storage format, sample type,
     *   comparison, array count, or byte size (`MergedLayoutConflict`). Names are not compared.
     *
     * On failure, `RecordIndex` stores the index of the binding record, and `Detail` stores the position of
     * the instance. */
    [[nodiscard]] static ManifestResult<MergedLayoutView> Merge(
        std::span<const EntryPointInstanceView> entry_points) noexcept;

    [[nodiscard]] Iterator begin() const noexcept;
    [[nodiscard]] Iterator end() const noexcept;
    [[nodiscard]] uint32_t StageCount() const noexcept;

private:
    MergedLayoutView() noexcept = default;
    uint32_t stageCount{ 0u };
    std::array<LayoutRange, k_MaxPipelineStages> layoutRanges{};
    std::array<ShaderStageKind, k_MaxPipelineStages> shaderStages{};
};


/**
* @brief Spans over the header region of one cook bundle, checked once when it opens.
*
* The header region contains every whole-cook table and every module header: strings, axes, profiles, the
* environment directory, and each module's entry points and axis runs. Open() checks all of it, so every
* accessor after it runs unchecked (effectively meaning it's branch-free).
*
* The span given to Open() can contain the whole file, or only the header region. Renderers that read the
* header region first and then the extents of one profile open each extent with `EnvironmentView::Open`.
* Callers with the whole file in memory use `OpenEnvironment`.
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
* Module axes are views of one root axis through the module's value mask. Digit D of a variant key
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
    [[nodiscard]] ManifestResult<const EntryPoint*> FindEntryPoint(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const ModuleAxis> ModuleAxes() const noexcept;
    [[nodiscard]] uint32_t AxisCount() const noexcept;
    /** @brief The root axis record behind module axis `local_axis`. */
    [[nodiscard]] const Axis& AxisData(uint32_t local_axis) const noexcept;
    /** @brief How many values this module uses on one axis. This is also the radix of that axis. */
    [[nodiscard]] uint32_t AxisValueCount(uint32_t local_axis) const noexcept;
    [[nodiscard]] AxisValueType AxisValue(uint32_t local_axis, uint32_t digit) const noexcept;
    /** @brief Returns the digit of the default the shader declares for one axis. Graph code uses it to fill an
     * axis that it does not pin. `Open` checked that it is below `AxisValueCount(local_axis)`. */
    [[nodiscard]] uint32_t AxisDefaultDigit(uint32_t local_axis) const noexcept;
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
* Every offset inside an extent is relative to the extent start, so that each extent can be loaded into
* its own buffer.
* The variant keys, the variant records, and the axis masks are parallel: the position of a key is the
* index of its variant. The EntryPointInstance for (variant V, entry point E) is at `V * EntryPointCount + E`.
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
    [[nodiscard]] ShaderCodeFormat CodeFormat() const noexcept;
    /** @brief The WGSL text of one source. Only for a `Wgsl` profile. */
    [[nodiscard]] std::string_view Source(uint32_t source_index) const noexcept;
    /** @brief The SPIR-V words of one source. Only for a `Spirv` profile. `Open` checked the alignment. */
    [[nodiscard]] std::span<const uint32_t> SpirvWords(uint32_t source_index) const noexcept;

    [[nodiscard]] std::span<const VariantKey> VariantKeys() const noexcept;
    [[nodiscard]] std::span<const Variant> Variants() const noexcept;
    /** @brief The index of the variant with this key, or no value when this environment did not cook it. */
    [[nodiscard]] std::optional<uint32_t> FindVariant(VariantKey key) const noexcept;
    /** @brief The variant with this key, or no value when this environment did not cook it. */
    [[nodiscard]] std::optional<VariantView> VariantByKey(VariantKey key) const noexcept;
    [[nodiscard]] VariantView VariantAt(uint32_t variant_index) const noexcept;
    [[nodiscard]] std::span<const uint64_t> AxisMask(uint32_t variant_index) const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t variant_index, uint32_t local_axis) const noexcept;
    /** @brief The axis relevance mask of one entry point: bit k is set when axis k can change that entry point's
     * instance in this environment. An axis with only one cooked value counts as relevant. */
    [[nodiscard]] std::span<const uint64_t> AxisRelevance(uint32_t entry_point_index) const noexcept;
    [[nodiscard]] bool IsAxisRelevant(uint32_t entry_point_index, uint32_t local_axis) const noexcept;

    [[nodiscard]] std::span<const EntryPointInstance> EntryPointInstanceTable() const noexcept;
    [[nodiscard]] std::span<const EntryPointInstance> VariantEntryPoints(uint32_t variant_index) const noexcept;
    /** @brief The entry-point specific information for one entry point of one variant, or null when this
     * environment did not cook the key. */
    [[nodiscard]] const EntryPointInstance* FindEntryPointInstance(uint32_t entry_point, VariantKey variant) const noexcept;

    [[nodiscard]] std::span<const Binding> Bindings() const noexcept;
    [[nodiscard]] const Binding& BindingRecord(uint32_t binding_index) const noexcept;
    /** @brief The resources one variant declares. Indices into Bindings(). */
    [[nodiscard]] std::span<const uint32_t> ResourceList(uint32_t list_index) const noexcept;
    /** @brief The footprint of each resource, in the same order as the resource list. */
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
    std::span<const uint64_t> axisRelevance;
    std::span<const EntryPointInstance> entryPointInstances;
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
 * The provider does not allocate. Each accessor reads the environment when called. `Bindings()` returns
 * a range that points into this provider, so do not move the provider while a range from it is in use.
 *
 * `Generation()` is the future hot-reload hook. A provider for baked data will always return the same value,
 * but a live provider can increment the value when any source changes - allowing users to reload
 * shaders and reset state gracefully
 */
class ShaderSourceProvider
{
public:
    ShaderSourceProvider(EnvironmentView view, uint64_t generation) noexcept;

    /** @brief The WGSL text. Only for a `Wgsl` profile. */
    [[nodiscard]] std::string_view Source(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    /** @brief The SPIR-V words, ready for `VkShaderModuleCreateInfo::pCode`. Only for a `Spirv` profile. */
    [[nodiscard]] std::span<const uint32_t> SpirvWords(uint32_t entry_point,
                                                       VariantKey variant) const noexcept;
    [[nodiscard]] LayoutRange Bindings(uint32_t entry_point, VariantKey variant) const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup(uint32_t entry_point,
                                          VariantKey variant) const noexcept;
    [[nodiscard]] uint64_t Generation() const noexcept;
    [[nodiscard]] const EnvironmentView& View() const noexcept;

private:
    EnvironmentView view;
    uint64_t generation{ 0u };
};

} // namespace lodestone::manifest

#endif // !LODESTONE_SHADER_MANIFEST_HPP
