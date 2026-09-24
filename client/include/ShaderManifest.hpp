#pragma once
#ifndef LODESTONE_SHADER_MANIFEST_HPP
#define LODESTONE_SHADER_MANIFEST_HPP
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <optional>
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
inline constexpr uint32_t k_ShaderManifestVersion = 6u;

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

/** One resource as one entry point sees it: the binding record and its footprint.
 * @note A view points at its `EnvironmentView`. Keep that environment alive, and do not move it. */
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
     * give one, or when the resource has no footprint. Then the caller must give the size. */
    [[nodiscard]] uint64_t ElementCount() const noexcept;
    /** @brief The texture extent from `[ls_extent_2d]` or `[ls_extent_3d]`. Zero when the shader does not
     * give one, or when the resource has no footprint. Then the caller must give the size. */
    [[nodiscard]] uint32_t ExtentX() const noexcept;
    [[nodiscard]] uint32_t ExtentY() const noexcept;
    [[nodiscard]] uint32_t ExtentZ() const noexcept;
    [[nodiscard]] UniformMemberRange Members() const noexcept;
    /** @brief The position of the binding record in `EnvironmentView::Bindings()`. */
    [[nodiscard]] uint32_t Index() const noexcept;
    [[nodiscard]] const Binding& Record() const noexcept;
    /** @brief Null when the footprint list is shorter than the resource list. */
    [[nodiscard]] const Footprint* FootprintRecord() const noexcept;

private:
    friend class LayoutRange;
    ResolvedResource(const EnvironmentView* _environment,
                     const Binding* _record,
                     const Footprint* _footprint) noexcept;
    const EnvironmentView* environment{ nullptr };
    const Binding* record{ nullptr };
    const Footprint* footprint{ nullptr };
};

/** The resources one entry point of one variant reads, in binding order. */
class LayoutRange
{
public:
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
 * @note An iterator points at its range. Keep the range from `Layout()` alive while you iterate it. */
class EntryPointInstanceView
{
public:
    [[nodiscard]] std::string_view Source() const noexcept;
    [[nodiscard]] WorkgroupSize Workgroup() const noexcept;
    [[nodiscard]] LayoutRange Layout() const noexcept;
    //[[nodiscard]] RasterView Raster() const noexcept;
    [[nodiscard]] uint32_t Raster() const noexcept;
    [[nodiscard]] const EntryPointInstance& Record() const noexcept;

private:
    friend class VariantView;
    EntryPointInstanceView(const EnvironmentView* _environment,
                           const EntryPointInstance* _slot,
                           const Variant* _variant) noexcept;
    const EnvironmentView* environment{ nullptr };
    const EntryPointInstance* slot{ nullptr };
    const Variant* variant{ nullptr };
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
    /** @brief The index of the variant with this key, or no value when this environment did not cook it. */
    [[nodiscard]] std::optional<uint32_t> FindVariant(VariantKey key) const noexcept;
    /** @brief The variant with this key, or no value when this environment did not cook it. */
    [[nodiscard]] std::optional<VariantView> VariantByKey(VariantKey key) const noexcept;
    [[nodiscard]] VariantView VariantAt(uint32_t variant_index) const noexcept;
    [[nodiscard]] std::span<const uint64_t> AxisMask(uint32_t variant_index) const noexcept;
    [[nodiscard]] bool IsAxisActive(uint32_t variant_index, uint32_t local_axis) const noexcept;

    [[nodiscard]] std::span<const EntryPointInstance> SlotTable() const noexcept;
    /** @brief One slot for each entry point of this variant, in entry point order. */
    [[nodiscard]] std::span<const EntryPointInstance> VariantSlots(uint32_t variant_index) const noexcept;
    /** @brief The entry-point specific information for one entry point of one variant, or null when this
     * environment did not cook the key. */
    [[nodiscard]] const EntryPointInstance* FindSlot(uint32_t entry_point, VariantKey variant) const noexcept;

    [[nodiscard]] std::span<const Binding> Bindings() const noexcept;
    [[nodiscard]] const Binding& BindingRecord(uint32_t binding_index) const noexcept;
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
 * The provider allocates nothing. Each accessor reads the environment when you call it.
 * `Bindings()` returns a range that points into this provider, so do not move the provider while you
 * hold a range from it.
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
