#include "ShaderManifest.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone::manifest
{

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace
{

    constexpr ErrorState k_ManifestOk{ .Code = ErrorCode::Success };

    // expanded descriptions for manifest error codes, since making these clear to users is important
    // static_assert will break compile if new code is added without updating the descriptions array
    // make sure ordering is respected, though
    // std::to_array deduces the size from the list. A sized std::array would pad a short list with empty
    // views, and the static_assert below could then never fire.
    constexpr auto k_ErrorDescriptions = std::to_array<std::string_view>({
            "no error code was set",
            "no error",
            "the byte span is smaller than the manifest header",
            "the file does not begin with the manifest magic number",
            "the manifest version does not match this reader",
            "the header's file size does not match the byte span",
            "a table or blob section runs past the end of its region",
            "an index points past the end of its table",
            "the byte span does not start on an 8-byte boundary",
            "a string reference points past the string blob",
            "a source reference points past the source blob",
            "a binding names a string past the string table",
            "a binding's uniform member range runs past the uniform member table",
            "an entry point names a string past the string table",
            "an entry point has an unknown shader stage",
            "a resource index points past the binding table",
            "a resource list run is out of range",
            "a footprint list run is out of range",
            "a visibility list run is out of range",
            "a slot's source index points past the source table",
            "a slot's visibility index points past the visibility list table",
            "a slot's raster index points past the raster table",
            "the variant keys are not strictly ascending",
            "the slot table does not hold one slot for each variant and entry point",
            "a raster's vertex input range is out of range",
            "a raster's color target range is out of range",
            "a vertex input names a string past the string table",
            "a variant's footprint list index points past the footprint list table",
            "a uniform member names a string past the string table",
            "an axis names a string past the string table",
            "an axis's value range runs past the axis value table",
            "a type axis value names a string past the string table",
            "a module header names a string past the string table",
            "a profile names a target string past the string table",
            "an environment extent runs past the end of the file",
            "a module axis names an axis past the axis table",
            "a module axis's value mask is empty, or sets a bit at or past the axis value count",
            "a specialization constant names a string past the string table",
            "the environment directory names no extent for this profile and module",
            "a variant's axis mask sets a bit past the module's axis count",
            "a profile has an unknown access model",
            "a variant names a suffix string past the string table",
        });

    static_assert(k_ErrorDescriptions.size() == static_cast<size_t>(ErrorCode::Count),
                  "every ErrorCode needs a description; add a line when you add a code");

    /**The reader reinterprets manifest bytes as records, so a record must be a bag of bytes.
    * This static assert ensures that types with objects, pointers, etc cannot make it into
    * the manifest data.*/
    template<typename RecordType>
    inline constexpr bool k_IsManifestRecord =
        std::is_trivially_copyable_v<RecordType> && alignof(RecordType) <= 8u;

    static_assert(k_IsManifestRecord<Run>);
    static_assert(k_IsManifestRecord<TableRef>);
    static_assert(k_IsManifestRecord<TableRef64>);
    static_assert(k_IsManifestRecord<Header>);
    static_assert(k_IsManifestRecord<StringRef>);
    static_assert(k_IsManifestRecord<SourceRef>);
    static_assert(k_IsManifestRecord<Profile>);
    static_assert(k_IsManifestRecord<EnvironmentDirectoryEntry>);
    static_assert(k_IsManifestRecord<Axis>);
    static_assert(k_IsManifestRecord<ModuleAxis>);
    static_assert(k_IsManifestRecord<ModuleRootHeader>);
    static_assert(k_IsManifestRecord<EntryPoint>);
    static_assert(k_IsManifestRecord<EnvironmentHeader>);
    static_assert(k_IsManifestRecord<Variant>);
    static_assert(k_IsManifestRecord<PlacementPayload>);
    static_assert(k_IsManifestRecord<Binding>);
    static_assert(k_IsManifestRecord<Footprint>);
    static_assert(k_IsManifestRecord<EntryPointInstance>);
    static_assert(k_IsManifestRecord<VertexInput>);
    static_assert(k_IsManifestRecord<UniformMember>);
    static_assert(k_IsManifestRecord<SpecializationConstant>);
    static_assert(k_IsManifestRecord<ColorTarget>);
    static_assert(k_IsManifestRecord<RasterState>);

    constexpr size_t k_AxisMaskWordBits = std::numeric_limits<uint64_t>::digits;

    template<typename RecordType>
    std::span<const RecordType> MakeTable(std::span<const std::byte> bytes,
                                          uint64_t offset,
                                          uint64_t count) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + offset),
                                            static_cast<size_t>(count) };
    }

    /**@brief In multiple locations, we store ranges of data in "runs". Each run specifies a contiguous block
     * of payloads, which could be themselves simple indices or POD structs. This is just a more succinct
     * accessor for those cases  */
    template<typename PayloadType>
    std::span<const PayloadType> RunOf(std::span<const Run> runs,
                                       std::span<const PayloadType> payloads,
                                       uint32_t run_index) noexcept
    {
        const Run& run = runs[static_cast<size_t>(run_index)];
        return payloads.subspan(run.First, run.Count);
    }

    /** One table to bounds-check: `Count` records of `RecordSize` bytes, starting at `Offset`. */
    struct Section
    {
        ShaderManifestTable Table;
        uint64_t Offset;
        uint64_t Count;
        size_t RecordSize;
    };

    /** The facts one extent's validators need from the bundle, with the extent itself. */
    struct EnvironmentContext
    {
        //NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
        const EnvironmentHeader& Environment;
        std::span<const std::byte> Extent;
        uint64_t StringCount;
        uint64_t EntryPointCount;
        uint32_t AxisMaskWordCount;
        uint32_t ModuleAxisCount;
    };

    using BundleValidator = ErrorState (*)(const Header&, std::span<const std::byte>) noexcept;
    using EnvironmentValidator = ErrorState (*)(const EnvironmentContext&) noexcept;

    /** True when a table of `count` records of `record_size` bytes starts at `offset` and stays inside a
     * region of `region_size` bytes. An empty table at any offset is in bounds. */
    bool TableIsInBounds(uint64_t offset, uint64_t count, size_t record_size, uint64_t region_size) noexcept;
    /** True when a grid of `rows * columns` records fits a region, checked without an overflow. */
    bool GridFitsRegion(uint64_t rows, uint64_t columns, size_t record_size, uint64_t region_size) noexcept;
    ErrorState ValidateSections(std::span<const Section> sections, uint64_t region_size) noexcept;
    /** The bit position of the set bit of rank `rank` in `mask`, counted from the least significant bit. */
    uint32_t SelectSetBit(uint32_t mask, uint32_t rank) noexcept;
    uint32_t MaskWordCountForAxes(uint64_t axis_count) noexcept;
    ErrorState OpenEnvironmentExtent(const BundleView& bundle,
                                     uint32_t profile_index,
                                     uint32_t module_index,
                                     std::span<const std::byte> extent_bytes) noexcept;

    ErrorState ValidateBundleHeader(const Header& parsed, size_t span_size) noexcept;
    ErrorState ValidateBundleSections(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateStrings(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateProfiles(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateModuleHeaders(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateEntryPoints(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateModuleAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept;
    ErrorState ValidateDirectory(const Header& parsed, std::span<const std::byte> bytes) noexcept;

    ErrorState ValidateEnvironmentSections(const EnvironmentContext& context) noexcept;
    ErrorState ValidateSources(const EnvironmentContext& context) noexcept;
    ErrorState ValidateBindings(const EnvironmentContext& context) noexcept;
    ErrorState ValidateResourceIndices(const EnvironmentContext& context) noexcept;
    ErrorState ValidateRunTables(const EnvironmentContext& context) noexcept;
    ErrorState ValidateSlots(const EnvironmentContext& context) noexcept;
    ErrorState ValidateVariantKeys(const EnvironmentContext& context) noexcept;
    ErrorState ValidateVariants(const EnvironmentContext& context) noexcept;
    ErrorState ValidateAxisMasks(const EnvironmentContext& context) noexcept;
    ErrorState ValidateRasterStates(const EnvironmentContext& context) noexcept;
    ErrorState ValidateVertexInputs(const EnvironmentContext& context) noexcept;
    ErrorState ValidateUniformMembers(const EnvironmentContext& context) noexcept;
    ErrorState ValidateSpecializationConstants(const EnvironmentContext& context) noexcept;

    // Yes, I know how branchy all of this is. But we pay it once when opening the manifest and then there's
    // no branches during actual queries, and the error type here can hold rich information that makes
    // debugging easier. This is mainly intended to catch errors in the manifest file itself, like corruption
    // or truncation: a rare issue, but a huge pain to debug if no infrastructure exists for catching it.
    // You should read this code as "I once spent a weekend debugging corrupted shader variants when trying
    // to ship a game, and it messed me up so bad I'm willing to write this BS"
    // The order matters: each validator may read what the validators before it proved in bounds.
    constexpr std::array<BundleValidator, 8u> k_BundleValidators{
        &ValidateBundleSections, &ValidateStrings,      &ValidateAxes,       &ValidateProfiles,
        &ValidateModuleHeaders,  &ValidateEntryPoints,  &ValidateModuleAxes, &ValidateDirectory,
    };

    constexpr std::array<EnvironmentValidator, 13u> k_EnvironmentValidators{
        &ValidateEnvironmentSections, &ValidateSources,      &ValidateBindings,
        &ValidateResourceIndices,     &ValidateRunTables,    &ValidateSlots,
        &ValidateVariantKeys,         &ValidateVariants,     &ValidateAxisMasks,
        &ValidateRasterStates,        &ValidateVertexInputs, &ValidateUniformMembers,
        &ValidateSpecializationConstants,
    };

} // namespace

std::string_view ToString(ErrorCode error) noexcept
{
    return magic_enum::enum_name(error);
}

std::string DescribeShaderManifestError(const ErrorState& error)
{
    const size_t codeIndex = static_cast<size_t>(error.Code);
    const std::string_view description = codeIndex < k_ErrorDescriptions.size()
                                             ? k_ErrorDescriptions[codeIndex]
                                             : std::string_view{ "unknown error code" };

    std::string out = std::format("{}: {}", magic_enum::enum_name(error.Code), description);

    if (error.Table != ShaderManifestTable::Invalid)
    {
        out += std::format(" [table {}, record {}]", magic_enum::enum_name(error.Table), error.RecordIndex);
    }

    // `Detail` is only implemented for a few codes, so we only handle it for a few specific error codes
    switch (error.Code)
    {
    case ErrorCode::VersionMismatch:
        out += std::format(" (file is version {}, reader expects {})", error.Detail, k_ShaderManifestVersion);
        break;
    case ErrorCode::SizeMismatch:
        out += std::format(" (header claims {} bytes)", error.Detail);
        break;
    case ErrorCode::TooSmall:
        out += std::format(" (span is only {} bytes)", error.Detail);
        break;
    case ErrorCode::SlotGridSizeMismatch:
        out += std::format(" (variant count {})", error.Detail);
        break;
    default:
        if (error.Table != ShaderManifestTable::Invalid)
        {
            out += std::format(" (offending value {})", error.Detail);
        }
        break;
    }

    return out;
}

BundleView::BundleView() noexcept = default;

ManifestResult<BundleView> BundleView::Open(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < sizeof(Header))
    {
        return std::unexpected(ErrorState{ .Code = ErrorCode::TooSmall,
                                           .Detail = static_cast<uint32_t>(bytes.size()) });
    }

    if ((reinterpret_cast<uintptr_t>(bytes.data()) % 8u) != 0u)
    {
        return std::unexpected(ErrorState{ .Code = ErrorCode::Misaligned });
    }

    Header parsed{};
    std::memcpy(&parsed, bytes.data(), sizeof(Header));

    const ErrorState headerValid = ValidateBundleHeader(parsed, bytes.size());
    if (!headerValid)
    {
        return std::unexpected(headerValid);
    }

    for (const BundleValidator validator : k_BundleValidators)
    {
        const ErrorState result = validator(parsed, bytes);
        if (!result)
        {
            return std::unexpected(result);
        }
    }

    BundleView view;
    view.bytes = bytes;
    view.header = reinterpret_cast<const Header*>(bytes.data());
    view.strings = MakeTable<StringRef>(bytes, parsed.StringTableOffset, parsed.StringCount);
    view.axes = MakeTable<Axis>(bytes, parsed.AxisTableOffset, parsed.AxisCount);
    view.axisValues = MakeTable<AxisValueType>(bytes, parsed.AxisValueTableOffset, parsed.AxisValueCount);
    view.profiles = MakeTable<Profile>(bytes, parsed.ProfileTableOffset, parsed.ProfileCount);
    view.moduleHeaders = MakeTable<ModuleRootHeader>(bytes, sizeof(Header), parsed.ModuleCount);
    view.directory = MakeTable<EnvironmentDirectoryEntry>(bytes,
                                                          parsed.EnvironmentDirectoryOffset,
                                                          parsed.ProfileCount * parsed.ModuleCount);
    return view;
}

std::string_view BundleView::String(uint32_t string_index) const noexcept
{
    assert(header != nullptr && string_index < strings.size());
    const StringRef& reference = strings[string_index];
    const char* base = reinterpret_cast<const char*>(bytes.data() + header->StringBlobOffset);
    return std::string_view{ base + reference.Offset, reference.Length };
}

uint32_t BundleView::StringCount() const noexcept
{
    return static_cast<uint32_t>(strings.size());
}

std::span<const Axis> BundleView::Axes() const noexcept
{
    return axes;
}

const Axis& BundleView::AxisData(uint32_t axis_index) const noexcept
{
    return axes[axis_index];
}

std::span<const AxisValueType> BundleView::AxisValues(uint32_t axis_index) const noexcept
{
    const Axis& axis = axes[axis_index];
    return axisValues.subspan(axis.FirstValue, axis.ValueCount);
}

std::span<const Profile> BundleView::Profiles() const noexcept
{
    return profiles;
}

uint32_t BundleView::ModuleCount() const noexcept
{
    return static_cast<uint32_t>(moduleHeaders.size());
}

ModuleView BundleView::Module(uint32_t module_index) const noexcept
{
    return ModuleView{ *this, module_index };
}

int32_t BundleView::FindModule(std::string_view module_name) const noexcept
{
    for (const auto&& [moduleIndex, moduleHeader] : std::views::enumerate(moduleHeaders))
    {
        if (String(moduleHeader.ModuleNameString) == module_name)
        {
            return static_cast<int32_t>(moduleIndex);
        }
    }

    return -1;
}

int32_t BundleView::FindProfile(std::string_view target_name) const noexcept
{
    for (const auto&& [profileIndex, profile] : std::views::enumerate(profiles))
    {
        if (String(profile.TargetNameString) == target_name)
        {
            return static_cast<int32_t>(profileIndex);
        }
    }

    return -1;
}

const EnvironmentDirectoryEntry& BundleView::Environment(uint32_t profile_index,
                                                         uint32_t module_index) const noexcept
{
    return directory[(static_cast<size_t>(profile_index) * moduleHeaders.size()) + module_index];
}

ManifestResult<EnvironmentView> BundleView::OpenEnvironment(uint32_t profile_index,
                                                            uint32_t module_index) const noexcept
{
    // The indices come from the caller, so this is an ingestion surface and it checks them.
    if (profile_index >= profiles.size() || module_index >= moduleHeaders.size())
    {
        return std::unexpected(ErrorState{ .Code = ErrorCode::IndexOutOfBounds,
                                           .Table = ShaderManifestTable::Environments,
                                           .Detail = module_index });
    }

    const EnvironmentDirectoryEntry& entry = Environment(profile_index, module_index);
    if (entry.ExtentSize == 0u)
    {
        return std::unexpected(ErrorState{ .Code = ErrorCode::EnvironmentNotCooked,
                                           .Table = ShaderManifestTable::Environments,
                                           .RecordIndex = profile_index,
                                           .Detail = module_index });
    }

    if (entry.ExtentOffset > bytes.size() || entry.ExtentSize > bytes.size() - entry.ExtentOffset)
    {
        return std::unexpected(ErrorState{ .Code = ErrorCode::EnvironmentExtentOutOfBounds,
                                           .Table = ShaderManifestTable::Environments,
                                           .RecordIndex = profile_index,
                                           .Detail = module_index });
    }

    return EnvironmentView::Open(*this,
                                 profile_index,
                                 module_index,
                                 bytes.subspan(static_cast<size_t>(entry.ExtentOffset),
                                               static_cast<size_t>(entry.ExtentSize)));
}

ModuleView::ModuleView() noexcept = default;

ModuleView::ModuleView(const BundleView& _bundle, uint32_t module_index) noexcept :
    bundle{ _bundle },
    moduleIndex{ module_index }
{
    const ModuleRootHeader& moduleHeader = bundle.moduleHeaders[module_index];
    entryPoints = MakeTable<EntryPoint>(bundle.bytes, moduleHeader.EntryPointTableOffset, moduleHeader.EntryPointCount);
    moduleAxes = MakeTable<ModuleAxis>(bundle.bytes, moduleHeader.ModuleAxisTableOffset, moduleHeader.ModuleAxisCount);
}

const BundleView& ModuleView::Bundle() const noexcept
{
    return bundle;
}

uint32_t ModuleView::Index() const noexcept
{
    return moduleIndex;
}

std::string_view ModuleView::Name() const noexcept
{
    return bundle.String(bundle.moduleHeaders[moduleIndex].ModuleNameString);
}

std::span<const EntryPoint> ModuleView::EntryPoints() const noexcept
{
    return entryPoints;
}

std::span<const ModuleAxis> ModuleView::ModuleAxes() const noexcept
{
    return moduleAxes;
}

uint32_t ModuleView::AxisCount() const noexcept
{
    return static_cast<uint32_t>(moduleAxes.size());
}

const Axis& ModuleView::AxisData(uint32_t local_axis) const noexcept
{
    return bundle.AxisData(moduleAxes[local_axis].AxisIndex);
}

uint32_t ModuleView::AxisValueCount(uint32_t local_axis) const noexcept
{
    return static_cast<uint32_t>(std::popcount(moduleAxes[local_axis].LiveValuesMask));
}

AxisValueType ModuleView::AxisValue(uint32_t local_axis, uint32_t digit) const noexcept
{
    const ModuleAxis& moduleAxis = moduleAxes[local_axis];
    const uint32_t rootValueIndex = SelectSetBit(moduleAxis.LiveValuesMask, digit);
    return bundle.AxisValues(moduleAxis.AxisIndex)[rootValueIndex];
}

uint32_t ModuleView::AxisMaskWordCount() const noexcept
{
    return MaskWordCountForAxes(moduleAxes.size());
}

EnvironmentView::EnvironmentView() noexcept = default;

ManifestResult<EnvironmentView> EnvironmentView::Open(const BundleView& bundle,
                                                      uint32_t profile_index,
                                                      uint32_t module_index,
                                                      std::span<const std::byte> extent_bytes) noexcept
{
    const ErrorState extentValid = OpenEnvironmentExtent(bundle, profile_index, module_index, extent_bytes);
    if (!extentValid)
    {
        return std::unexpected(extentValid);
    }

    EnvironmentHeader parsed{};
    std::memcpy(&parsed, extent_bytes.data(), sizeof(EnvironmentHeader));

    EnvironmentView view;
    view.module = bundle.Module(module_index);
    view.profileIndex = profile_index;
    view.extent = extent_bytes;

    const EnvironmentContext context{ .Environment = parsed,
                                      .Extent = extent_bytes,
                                      .StringCount = bundle.StringCount(),
                                      .EntryPointCount = view.module.EntryPoints().size(),
                                      .AxisMaskWordCount = view.module.AxisMaskWordCount(),
                                      .ModuleAxisCount = view.module.AxisCount() };

    for (const EnvironmentValidator validator : k_EnvironmentValidators)
    {
        const ErrorState result = validator(context);
        if (!result)
        {
            return std::unexpected(result);
        }
    }

    const uint64_t slotCount = static_cast<uint64_t>(parsed.VariantCount) * context.EntryPointCount;
    const uint64_t maskWordCount = static_cast<uint64_t>(parsed.VariantCount) * context.AxisMaskWordCount;

    view.variantKeys = MakeTable<VariantKey>(extent_bytes, parsed.VariantKeyTableOffset, parsed.VariantCount);
    view.variants = MakeTable<Variant>(extent_bytes, parsed.VariantTableOffset, parsed.VariantCount);
    view.axisMasks = MakeTable<uint64_t>(extent_bytes, parsed.AxisMaskTableOffset, maskWordCount);
    view.slots = MakeTable<EntryPointInstance>(extent_bytes, parsed.SlotTableOffset, slotCount);
    view.sources = MakeTable<SourceRef>(extent_bytes, parsed.SourceTableOffset, parsed.SourceCount);
    view.sourceBlob = MakeTable<char>(extent_bytes, parsed.SourceBlobOffset, parsed.SourceBlobSize);
    view.bindings = MakeTable<Binding>(extent_bytes, parsed.BindingTableOffset, parsed.BindingCount);
    view.resourceLists = MakeTable<Run>(extent_bytes, parsed.ResourceListTableOffset, parsed.ResourceListCount);
    view.resourceIndices =
        MakeTable<uint32_t>(extent_bytes, parsed.ResourceIndexTableOffset, parsed.ResourceIndexCount);
    view.footprints = MakeTable<Footprint>(extent_bytes, parsed.FootprintTableOffset, parsed.FootprintCount);
    view.footprintLists = MakeTable<Run>(extent_bytes, parsed.FootprintListTableOffset, parsed.FootprintListCount);
    view.visibilityLists =
        MakeTable<Run>(extent_bytes, parsed.VisibilityListTableOffset, parsed.VisibilityListCount);
    view.visibilityIndices =
        MakeTable<uint32_t>(extent_bytes, parsed.VisibilityIndexTableOffset, parsed.VisibilityIndexCount);
    view.rasterStates = MakeTable<RasterState>(extent_bytes, parsed.RasterTableOffset, parsed.RasterCount);
    view.vertexInputs = MakeTable<VertexInput>(extent_bytes, parsed.VertexInputTableOffset, parsed.VertexInputCount);
    view.colorTargets = MakeTable<ColorTarget>(extent_bytes, parsed.ColorTargetTableOffset, parsed.ColorTargetCount);
    view.uniformMembers =
        MakeTable<UniformMember>(extent_bytes, parsed.UniformMemberTableOffset, parsed.UniformMemberCount);
    view.specializationConstants = MakeTable<SpecializationConstant>(
        extent_bytes, parsed.SpecializationConstantTableOffset, parsed.SpecializationConstantCount);
    return view;
}

const ModuleView& EnvironmentView::Module() const noexcept
{
    return module;
}

uint32_t EnvironmentView::ProfileIndex() const noexcept
{
    return profileIndex;
}

const Profile& EnvironmentView::ProfileRecord() const noexcept
{
    return module.Bundle().Profiles()[profileIndex];
}

std::string_view EnvironmentView::String(uint32_t string_index) const noexcept
{
    return module.Bundle().String(string_index);
}

std::string_view EnvironmentView::Source(uint32_t source_index) const noexcept
{
    // if this assert fires on source_index, caller provided invalid index
    assert(source_index < sources.size());
    const SourceRef& reference = sources[source_index];
    return std::string_view{ sourceBlob.data() + reference.Offset, reference.Length };
}

std::span<const VariantKey> EnvironmentView::VariantKeys() const noexcept
{
    return variantKeys;
}

std::span<const Variant> EnvironmentView::Variants() const noexcept
{
    return variants;
}

int32_t EnvironmentView::FindVariant(VariantKey key) const noexcept
{
    // the key table is sorted and parallel to the variant table, so the position of a key *is* the index of
    // its variant
    const auto keyIter = std::ranges::lower_bound(variantKeys, key);
    if (keyIter == variantKeys.end() || *keyIter != key) [[unlikely]]
    {
        return -1;
    }

    return static_cast<int32_t>(std::distance(variantKeys.begin(), keyIter));
}

std::span<const uint64_t> EnvironmentView::AxisMask(uint32_t variant_index) const noexcept
{
    const size_t wordCount = module.AxisMaskWordCount();
    return axisMasks.subspan(static_cast<size_t>(variant_index) * wordCount, wordCount);
}

bool EnvironmentView::IsAxisActive(uint32_t variant_index, uint32_t local_axis) const noexcept
{
    const std::span<const uint64_t> mask = AxisMask(variant_index);
    const uint64_t word = mask[local_axis / k_AxisMaskWordBits];
    return ((word >> (local_axis % k_AxisMaskWordBits)) & 1u) != 0u;
}

std::span<const EntryPointInstance> EnvironmentView::SlotTable() const noexcept
{
    return slots;
}

std::span<const EntryPointInstance> EnvironmentView::VariantSlots(uint32_t variant_index) const noexcept
{
    const size_t entryPointCount = module.EntryPoints().size();
    return slots.subspan(static_cast<size_t>(variant_index) * entryPointCount, entryPointCount);
}

const EntryPointInstance* EnvironmentView::FindSlot(uint32_t entry_point, VariantKey variant_key) const noexcept
{
    const int32_t variantIndex = FindVariant(variant_key);
    if (variantIndex < 0) [[unlikely]]
    {
        return nullptr;
    }

    return &VariantSlots(static_cast<uint32_t>(variantIndex))[entry_point];
}

std::span<const Binding> EnvironmentView::Bindings() const noexcept
{
    return bindings;
}

std::span<const uint32_t> EnvironmentView::ResourceList(uint32_t list_index) const noexcept
{
    return RunOf(resourceLists, resourceIndices, list_index);
}

std::span<const Footprint> EnvironmentView::FootprintList(uint32_t list_index) const noexcept
{
    return RunOf(footprintLists, footprints, list_index);
}

std::span<const uint32_t> EnvironmentView::VisibilityList(uint32_t list_index) const noexcept
{
    return RunOf(visibilityLists, visibilityIndices, list_index);
}

std::span<const UniformMember> EnvironmentView::UniformMembers(const Binding& binding) const noexcept
{
    return uniformMembers.subspan(binding.FirstUniformMember, binding.UniformMemberCount);
}

std::span<const VertexInput> EnvironmentView::VertexInputs(uint32_t raster_index) const noexcept
{
    assert(raster_index < rasterStates.size());
    const RasterState& raster = rasterStates[raster_index];
    return vertexInputs.subspan(raster.FirstVertexInput, raster.VertexInputCount);
}

std::span<const ColorTarget> EnvironmentView::ColorTargets(uint32_t raster_index) const noexcept
{
    assert(raster_index < rasterStates.size());
    const RasterState& raster = rasterStates[raster_index];
    return colorTargets.subspan(raster.FirstColorTarget, raster.ColorTargetCount);
}

bool EnvironmentView::WritesFragDepth(uint32_t raster_index) const noexcept
{
    assert(raster_index < rasterStates.size());
    return rasterStates[raster_index].WritesFragDepth != 0u;
}

std::span<const SpecializationConstant> EnvironmentView::SpecializationConstants() const noexcept
{
    return specializationConstants;
}

ShaderSourceProvider::ShaderSourceProvider(EnvironmentView _view,
                                           uint64_t _generation) noexcept
    : view{ _view },
      generation{ _generation }
{
    const std::span<const Binding> records = view.Bindings();
    bindingInfos.reserve(records.size());

    size_t totalMembers = 0u;
    for (const Binding& record : records)
    {
        totalMembers += record.UniformMemberCount;
    }

    memberInfos.reserve(totalMembers);
    for (const Binding& record : records)
    {
        for (const UniformMember& member : view.UniformMembers(record))
        {
            UniformMemberInfo info;
            info.Name = view.String(member.NameString);
            info.Offset = member.Offset;
            info.Size = member.Size;
            info.ArrayCount = member.ArrayCount;
            info.ElementStride = static_cast<uint16_t>(member.ElementStride);
            info.Layout = static_cast<MatrixLayout>(member.MatrixLayout);
            memberInfos.push_back(info);
        }
    }

    // Where each resource's members start, so a gathered binding can point at them.
    std::vector<uint32_t> memberOffsets;
    memberOffsets.reserve(records.size());
    uint32_t memberCursor = 0u;
    for (const Binding& record : records)
    {
        memberOffsets.push_back(memberCursor);
        memberCursor += record.UniformMemberCount;
    }

    const std::span<const EntryPointInstance> allSlots = view.SlotTable();
    slotFirstBinding.assign(allSlots.size(), 0u);
    slotBindingCount.assign(allSlots.size(), 0u);

    for (uint32_t variantIndex = 0u; variantIndex < view.Variants().size(); ++variantIndex)
    {
        GatherVariantBindings(variantIndex, memberOffsets);
    }
}

void ShaderSourceProvider::GatherVariantBindings(uint32_t variant_index,
                                                 const std::vector<uint32_t>& member_offsets)
{
    const Variant& variant = view.Variants()[variant_index];
    const std::span<const Binding> records = view.Bindings();
    const std::span<const uint32_t> resources = view.ResourceList(variant.ResourceListIndex);
    const std::span<const Footprint> footprints = view.FootprintList(variant.FootprintListIndex);
    const std::span<const EntryPointInstance> variantSlots = view.VariantSlots(variant_index);
    // the slot grid puts this variant's slots in one row, so the row start is the first slot's table index
    const size_t firstSlotIndex = static_cast<size_t>(variantSlots.data() - view.SlotTable().data());

    for (const auto&& [entryPointIndex, slot] : std::views::enumerate(variantSlots))
    {
        const size_t slotIndex = firstSlotIndex + static_cast<size_t>(entryPointIndex);
        const std::span<const uint32_t> visible = view.VisibilityList(slot.VisibilityIndex);

        slotFirstBinding[slotIndex] = static_cast<uint32_t>(bindingInfos.size());
        slotBindingCount[slotIndex] = static_cast<uint32_t>(visible.size());

        for (const uint32_t local : visible)
        {
            bindingInfos.push_back(MakeBindingInfo(records[resources[local]],
                                                   local < footprints.size() ? &footprints[local] : nullptr,
                                                   member_offsets[resources[local]]));
        }
    }
}

BindingInfo ShaderSourceProvider::MakeBindingInfo(const Binding& record,
                                                  const Footprint* footprint,
                                                  uint32_t member_offset) const noexcept
{
    BindingInfo info;
    info.Name = view.String(record.NameString);
    info.ScopeName = view.String(record.ScopeString);
    // todo-ship: BindingInfo still carries a fixed group and binding. Give it the placement kind and the
    // payload, so an Indexed or Pointer placement reaches a renderer.
    if (record.PlacementKind == static_cast<uint8_t>(PlacementKind::Bound))
    {
        info.Group = record.Placement.Word0;
        info.Binding = record.Placement.Word1;
    }
    info.Kind = static_cast<BindingKind>(record.Kind);
    info.ElementStride = record.ElementStride;
    info.ByteSize = record.ByteSize;
    info.ArrayCount = record.ArrayCount;
    info.Shape = static_cast<ResourceShape>(record.Shape);
    info.IsComparisonSampler = static_cast<bool>(record.IsComparisonSampler);
    info.StorageFormat = static_cast<TextureFormat>(record.StorageFormat);
    info.Access = static_cast<ResourceAccess>(record.Access);

    if (footprint != nullptr)
    {
        info.DerivedElementCount = footprint->ElementCount;
        info.DerivedExtentX = footprint->ExtentX;
        info.DerivedExtentY = footprint->ExtentY;
        info.DerivedExtentZ = footprint->ExtentZ;
    }

    if (record.UniformMemberCount != 0u)
    {
        info.Members = std::span<const UniformMemberInfo>{ memberInfos.data() + member_offset,
                                                           record.UniformMemberCount };
    }

    return info;
}

std::string_view ShaderSourceProvider::Source(uint32_t entry_point,
                                              VariantKey variant) const noexcept
{
    const EntryPointInstance* slot = view.FindSlot(entry_point, variant);
    assert(slot != nullptr);
    return view.Source(slot->SourceIndex);
}

std::span<const BindingInfo> ShaderSourceProvider::Bindings(uint32_t entry_point,
                                                            VariantKey variant) const noexcept
{
    const EntryPointInstance* slot = view.FindSlot(entry_point, variant);
    assert(slot != nullptr);
    const size_t slotIndex = static_cast<size_t>(slot - view.SlotTable().data());
    return std::span<const BindingInfo>{ bindingInfos.data() + slotFirstBinding[slotIndex],
                                         slotBindingCount[slotIndex] };
}

WorkgroupSize ShaderSourceProvider::Workgroup(uint32_t entry_point,
                                              VariantKey variant) const noexcept
{
    const EntryPointInstance* slot = view.FindSlot(entry_point, variant);
    assert(slot != nullptr);
    return WorkgroupSize{ .X = slot->WorkgroupX, .Y = slot->WorkgroupY, .Z = slot->WorkgroupZ };
}

uint64_t ShaderSourceProvider::Generation() const noexcept
{
    return generation;
}

const EnvironmentView& ShaderSourceProvider::View() const noexcept
{
    return view;
}

namespace
{

    bool TableIsInBounds(uint64_t offset, uint64_t count, size_t record_size, uint64_t region_size) noexcept
    {
        if (count == 0u)
        {
            return true;
        }

        // divide first, so a huge count cannot overflow the multiply
        if (count > region_size / record_size)
        {
            return false;
        }

        const uint64_t span = count * record_size;
        return offset <= region_size && span <= region_size - offset;
    }

    bool GridFitsRegion(uint64_t rows, uint64_t columns, size_t record_size, uint64_t region_size) noexcept
    {
        if (rows == 0u || columns == 0u)
        {
            return true;
        }

        return rows <= (region_size / record_size) / columns;
    }

    ErrorState ValidateSections(std::span<const Section> sections, uint64_t region_size) noexcept
    {
        for (const Section& section : sections)
        {
            if (!TableIsInBounds(section.Offset, section.Count, section.RecordSize, region_size))
            {
                return { .Code = ErrorCode::SectionOutOfBounds, .Table = section.Table };
            }
        }

        return k_ManifestOk;
    }

    uint32_t SelectSetBit(uint32_t mask, uint32_t rank) noexcept
    {
        for (uint32_t cleared = 0u; cleared < rank; ++cleared)
        {
            mask &= mask - 1u;
        }

        return static_cast<uint32_t>(std::countr_zero(mask));
    }

    uint32_t MaskWordCountForAxes(uint64_t axis_count) noexcept
    {
        return static_cast<uint32_t>((axis_count + k_AxisMaskWordBits - 1u) / k_AxisMaskWordBits);
    }

    ErrorState OpenEnvironmentExtent(const BundleView& bundle,
                                     uint32_t profile_index,
                                     uint32_t module_index,
                                     std::span<const std::byte> extent_bytes) noexcept
    {
        // The indices and the bytes come from the caller, so this is an ingestion surface and it checks them.
        if (profile_index >= bundle.Profiles().size() || module_index >= bundle.ModuleCount())
        {
            return { .Code = ErrorCode::IndexOutOfBounds,
                     .Table = ShaderManifestTable::Environments,
                     .Detail = module_index };
        }

        const EnvironmentDirectoryEntry& entry = bundle.Environment(profile_index, module_index);
        if (entry.ExtentSize == 0u)
        {
            return { .Code = ErrorCode::EnvironmentNotCooked,
                     .Table = ShaderManifestTable::Environments,
                     .RecordIndex = profile_index,
                     .Detail = module_index };
        }

        if (extent_bytes.size() != entry.ExtentSize)
        {
            return { .Code = ErrorCode::SizeMismatch, .Detail = static_cast<uint32_t>(entry.ExtentSize) };
        }

        if ((reinterpret_cast<uintptr_t>(extent_bytes.data()) % 8u) != 0u)
        {
            return { .Code = ErrorCode::Misaligned };
        }

        return k_ManifestOk;
    }

    ErrorState ValidateBundleHeader(const Header& parsed, size_t span_size) noexcept
    {
        if (parsed.Magic != k_ShaderManifestMagic)
        {
            return { .Code = ErrorCode::BadMagic };
        }

        if (parsed.Version != k_ShaderManifestVersion)
        {
            return { .Code = ErrorCode::VersionMismatch, .Detail = parsed.Version };
        }

        if (parsed.HeaderSize < sizeof(Header) || (parsed.HeaderSize % 8u) != 0u ||
            parsed.HeaderSize > parsed.FileSize)
        {
            return { .Code = ErrorCode::SizeMismatch, .Detail = static_cast<uint32_t>(parsed.HeaderSize) };
        }

        // A caller can hold the header region alone, or the whole file. Anything past the file is wrong.
        if (span_size < parsed.HeaderSize)
        {
            return { .Code = ErrorCode::TooSmall, .Detail = static_cast<uint32_t>(span_size) };
        }

        if (span_size > parsed.FileSize)
        {
            return { .Code = ErrorCode::SizeMismatch, .Detail = static_cast<uint32_t>(parsed.FileSize) };
        }

        return k_ManifestOk;
    }

    //NOLINTBEGIN(modernize-use-designated-initializers)
    ErrorState ValidateBundleSections(const Header& parsed, [[maybe_unused]] std::span<const std::byte> bytes) noexcept
    {
        // every whole-cook table lives inside the header region, so a reader of that region alone can trust it
        const std::array<Section, 6u> sections{
            Section{ ShaderManifestTable::Modules, sizeof(Header), parsed.ModuleCount, sizeof(ModuleRootHeader) },
            Section{ ShaderManifestTable::Profiles, parsed.ProfileTableOffset, parsed.ProfileCount, sizeof(Profile) },
            Section{ ShaderManifestTable::Strings, parsed.StringTableOffset, parsed.StringCount, sizeof(StringRef) },
            Section{ ShaderManifestTable::Strings, parsed.StringBlobOffset, parsed.StringBlobSize, 1u },
            Section{ ShaderManifestTable::Axes, parsed.AxisTableOffset, parsed.AxisCount, sizeof(Axis) },
            Section{ ShaderManifestTable::AxisValues,
                     parsed.AxisValueTableOffset,
                     parsed.AxisValueCount,
                     sizeof(AxisValueType) },
        };

        const ErrorState sectionsValid = ValidateSections(sections, parsed.HeaderSize);
        if (!sectionsValid)
        {
            return sectionsValid;
        }

        // the directory is a grid, so check the product without overflowing it
        if (!GridFitsRegion(parsed.ProfileCount, parsed.ModuleCount, sizeof(EnvironmentDirectoryEntry), parsed.HeaderSize) ||
            !TableIsInBounds(parsed.EnvironmentDirectoryOffset,
                             parsed.ProfileCount * parsed.ModuleCount,
                             sizeof(EnvironmentDirectoryEntry),
                             parsed.HeaderSize))
        {
            return { .Code = ErrorCode::SectionOutOfBounds, .Table = ShaderManifestTable::Environments };
        }

        return k_ManifestOk;
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ErrorState ValidateStrings(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const StringRef> stringSpan =
            MakeTable<StringRef>(bytes, parsed.StringTableOffset, parsed.StringCount);
        for (uint32_t i = 0u; i < stringSpan.size(); ++i)
        {
            const StringRef& reference = stringSpan[i];
            if (reference.Length > parsed.StringBlobSize || reference.Offset > parsed.StringBlobSize - reference.Length)
            {
                return { .Code = ErrorCode::StringOutOfBounds, .Table = ShaderManifestTable::Strings, .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        // Each axis names a string and owns a run of values in the axis value table. A literal value has
        // nothing to reference, so only the name, the run, and a named value need checking.
        const std::span<const Axis> axisSpan = MakeTable<Axis>(bytes, parsed.AxisTableOffset, parsed.AxisCount);
        const std::span<const AxisValueType> allAxesValueSpan =
            MakeTable<AxisValueType>(bytes, parsed.AxisValueTableOffset, parsed.AxisValueCount);
        for (uint32_t i = 0u; i < axisSpan.size(); ++i)
        {
            const Axis& axis = axisSpan[i];
            if (axis.NameString >= parsed.StringCount)
            {
                return { .Code = ErrorCode::InvalidAxisName,
                         .Table = ShaderManifestTable::Axes,
                         .RecordIndex = i,
                         .Detail = axis.NameString };
            }

            if (static_cast<uint64_t>(axis.FirstValue) + static_cast<uint64_t>(axis.ValueCount) > parsed.AxisValueCount)
            {
                return { .Code = ErrorCode::InvalidAxisValueRange,
                         .Table = ShaderManifestTable::Axes,
                         .RecordIndex = i,
                         .Detail = axis.ValueCount };
            }

            // both types and enums have values stored in string blob, verify integrity of the string indices
            if (axis.Domain != AxisValueDomain::Type && axis.Domain != AxisValueDomain::Enum)
            {
                continue;
            }

            for (const AxisValueType value : allAxesValueSpan.subspan(axis.FirstValue, axis.ValueCount))
            {
                if (value >= parsed.StringCount)
                {
                    return { .Code = ErrorCode::InvalidAxisTypeStrIndex,
                             .Table = ShaderManifestTable::Axes,
                             .RecordIndex = i,
                             .Detail = value };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateProfiles(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const Profile> profileSpan =
            MakeTable<Profile>(bytes, parsed.ProfileTableOffset, parsed.ProfileCount);
        for (uint32_t i = 0u; i < profileSpan.size(); ++i)
        {
            const Profile& profile = profileSpan[i];
            if (profile.TargetNameString >= parsed.StringCount)
            {
                return { .Code = ErrorCode::InvalidProfileTargetName,
                         .Table = ShaderManifestTable::Profiles,
                         .RecordIndex = i,
                         .Detail = profile.TargetNameString };
            }

            if (profile.AccessModel < PlacementKind::Bound || profile.AccessModel > PlacementKind::Pointer)
            {
                return { .Code = ErrorCode::InvalidProfileAccessModel,
                         .Table = ShaderManifestTable::Profiles,
                         .RecordIndex = i,
                         .Detail = static_cast<uint32_t>(profile.AccessModel) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateModuleHeaders(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const ModuleRootHeader> moduleSpan =
            MakeTable<ModuleRootHeader>(bytes, sizeof(Header), parsed.ModuleCount);
        for (uint32_t i = 0u; i < moduleSpan.size(); ++i)
        {
            const ModuleRootHeader& moduleHeader = moduleSpan[i];
            if (moduleHeader.ModuleNameString >= parsed.StringCount)
            {
                return { .Code = ErrorCode::InvalidModuleNameString,
                         .Table = ShaderManifestTable::Modules,
                         .RecordIndex = i,
                         .Detail = moduleHeader.ModuleNameString };
            }

            if (!TableIsInBounds(moduleHeader.EntryPointTableOffset,
                                 moduleHeader.EntryPointCount,
                                 sizeof(EntryPoint),
                                 parsed.HeaderSize))
            {
                return { .Code = ErrorCode::SectionOutOfBounds,
                         .Table = ShaderManifestTable::EntryPoints,
                         .RecordIndex = i };
            }

            if (!TableIsInBounds(moduleHeader.ModuleAxisTableOffset,
                                 moduleHeader.ModuleAxisCount,
                                 sizeof(ModuleAxis),
                                 parsed.HeaderSize))
            {
                return { .Code = ErrorCode::SectionOutOfBounds,
                         .Table = ShaderManifestTable::ModuleAxes,
                         .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateEntryPoints(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        for (const ModuleRootHeader& moduleHeader : MakeTable<ModuleRootHeader>(bytes, sizeof(Header), parsed.ModuleCount))
        {
            const std::span<const EntryPoint> entryPointSpan =
                MakeTable<EntryPoint>(bytes, moduleHeader.EntryPointTableOffset, moduleHeader.EntryPointCount);
            for (uint32_t i = 0u; i < entryPointSpan.size(); ++i)
            {
                const EntryPoint& entryPoint = entryPointSpan[i];
                if (entryPoint.NameString >= parsed.StringCount)
                {
                    return { .Code = ErrorCode::EntryPointInvalidName,
                             .Table = ShaderManifestTable::EntryPoints,
                             .RecordIndex = i,
                             .Detail = entryPoint.NameString };
                }

                if (entryPoint.Stage >= static_cast<uint32_t>(ShaderStageKind::Count) ||
                    entryPoint.Stage == static_cast<uint32_t>(ShaderStageKind::Invalid))
                {
                    return { .Code = ErrorCode::EntryPointInvalidStage,
                             .Table = ShaderManifestTable::EntryPoints,
                             .RecordIndex = i,
                             .Detail = entryPoint.Stage };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateModuleAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const Axis> axisSpan = MakeTable<Axis>(bytes, parsed.AxisTableOffset, parsed.AxisCount);
        for (const ModuleRootHeader& moduleHeader : MakeTable<ModuleRootHeader>(bytes, sizeof(Header), parsed.ModuleCount))
        {
            const std::span<const ModuleAxis> moduleAxisSpan =
                MakeTable<ModuleAxis>(bytes, moduleHeader.ModuleAxisTableOffset, moduleHeader.ModuleAxisCount);
            for (uint32_t i = 0u; i < moduleAxisSpan.size(); ++i)
            {
                const ModuleAxis& moduleAxis = moduleAxisSpan[i];
                if (moduleAxis.AxisIndex >= axisSpan.size())
                {
                    return { .Code = ErrorCode::InvalidModuleAxisIndex,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = i,
                             .Detail = moduleAxis.AxisIndex };
                }

                // An empty mask gives the axis a radix of zero, and a bit past the root values selects nothing.
                const uint32_t valueCount = axisSpan[moduleAxis.AxisIndex].ValueCount;
                const bool maskPastValues = valueCount < 32u && (moduleAxis.LiveValuesMask >> valueCount) != 0u;
                if (moduleAxis.LiveValuesMask == 0u || maskPastValues)
                {
                    return { .Code = ErrorCode::InvalidModuleAxisValueMask,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = i,
                             .Detail = moduleAxis.LiveValuesMask };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateDirectory(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const EnvironmentDirectoryEntry> directorySpan = MakeTable<EnvironmentDirectoryEntry>(
            bytes, parsed.EnvironmentDirectoryOffset, parsed.ProfileCount * parsed.ModuleCount);
        for (uint32_t i = 0u; i < directorySpan.size(); ++i)
        {
            const EnvironmentDirectoryEntry& entry = directorySpan[i];
            if (entry.ExtentSize == 0u)
            {
                continue;
            }

            // An extent starts past the header region, on an 8-byte boundary, and stays inside the file. It
            // checks against the size the header states, because the span may hold the header region alone.
            const bool valid = entry.ExtentOffset >= parsed.HeaderSize && (entry.ExtentOffset % 8u) == 0u &&
                               entry.ExtentSize >= sizeof(EnvironmentHeader) &&
                               entry.ExtentOffset <= parsed.FileSize &&
                               entry.ExtentSize <= parsed.FileSize - entry.ExtentOffset;
            if (!valid)
            {
                return { .Code = ErrorCode::EnvironmentExtentOutOfBounds,
                         .Table = ShaderManifestTable::Environments,
                         .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    //NOLINTBEGIN(modernize-use-designated-initializers)
    ErrorState ValidateEnvironmentSections(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const uint64_t extentSize = context.Extent.size();

        if (!GridFitsRegion(environment.VariantCount, context.EntryPointCount, sizeof(EntryPointInstance), extentSize) ||
            !TableIsInBounds(environment.SlotTableOffset,
                             static_cast<uint64_t>(environment.VariantCount) * context.EntryPointCount,
                             sizeof(EntryPointInstance),
                             extentSize))
        {
            return { .Code = ErrorCode::SlotGridSizeMismatch,
                     .Table = ShaderManifestTable::Slots,
                     .Detail = environment.VariantCount };
        }

        const std::array<Section, 17u> sections{
            Section{ ShaderManifestTable::VariantKeys,
                     environment.VariantKeyTableOffset,
                     environment.VariantCount,
                     sizeof(VariantKey) },
            Section{ ShaderManifestTable::Variants, environment.VariantTableOffset, environment.VariantCount, sizeof(Variant) },
            Section{ ShaderManifestTable::AxisMasks,
                     environment.AxisMaskTableOffset,
                     static_cast<uint64_t>(environment.VariantCount) * context.AxisMaskWordCount,
                     sizeof(uint64_t) },
            Section{ ShaderManifestTable::Sources, environment.SourceTableOffset, environment.SourceCount, sizeof(SourceRef) },
            Section{ ShaderManifestTable::Sources, environment.SourceBlobOffset, environment.SourceBlobSize, 1u },
            Section{ ShaderManifestTable::Bindings, environment.BindingTableOffset, environment.BindingCount, sizeof(Binding) },
            Section{ ShaderManifestTable::ResourceLists,
                     environment.ResourceListTableOffset,
                     environment.ResourceListCount,
                     sizeof(Run) },
            Section{ ShaderManifestTable::ResourceIndices,
                     environment.ResourceIndexTableOffset,
                     environment.ResourceIndexCount,
                     sizeof(uint32_t) },
            Section{ ShaderManifestTable::Footprints,
                     environment.FootprintTableOffset,
                     environment.FootprintCount,
                     sizeof(Footprint) },
            Section{ ShaderManifestTable::FootprintLists,
                     environment.FootprintListTableOffset,
                     environment.FootprintListCount,
                     sizeof(Run) },
            Section{ ShaderManifestTable::VisibilityLists,
                     environment.VisibilityListTableOffset,
                     environment.VisibilityListCount,
                     sizeof(Run) },
            Section{ ShaderManifestTable::VisibilityIndices,
                     environment.VisibilityIndexTableOffset,
                     environment.VisibilityIndexCount,
                     sizeof(uint32_t) },
            Section{ ShaderManifestTable::Rasters, environment.RasterTableOffset, environment.RasterCount, sizeof(RasterState) },
            Section{ ShaderManifestTable::VertexInputs,
                     environment.VertexInputTableOffset,
                     environment.VertexInputCount,
                     sizeof(VertexInput) },
            Section{ ShaderManifestTable::ColorTargets,
                     environment.ColorTargetTableOffset,
                     environment.ColorTargetCount,
                     sizeof(ColorTarget) },
            Section{ ShaderManifestTable::UniformMembers,
                     environment.UniformMemberTableOffset,
                     environment.UniformMemberCount,
                     sizeof(UniformMember) },
            Section{ ShaderManifestTable::SpecializationConstants,
                     environment.SpecializationConstantTableOffset,
                     environment.SpecializationConstantCount,
                     sizeof(SpecializationConstant) },
        };

        return ValidateSections(sections, extentSize);
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ErrorState ValidateSources(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const SourceRef> sourceSpan =
            MakeTable<SourceRef>(context.Extent, environment.SourceTableOffset, environment.SourceCount);
        for (uint32_t i = 0u; i < sourceSpan.size(); ++i)
        {
            const SourceRef& reference = sourceSpan[i];
            if (reference.Length > environment.SourceBlobSize ||
                reference.Offset > environment.SourceBlobSize - reference.Length)
            {
                return { .Code = ErrorCode::SourceOutOfBounds, .Table = ShaderManifestTable::Sources, .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateBindings(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const Binding> bindingSpan =
            MakeTable<Binding>(context.Extent, environment.BindingTableOffset, environment.BindingCount);
        for (uint32_t i = 0u; i < bindingSpan.size(); ++i)
        {
            const Binding& binding = bindingSpan[i];
            if (binding.NameString >= context.StringCount || binding.ScopeString >= context.StringCount)
            {
                return { .Code = ErrorCode::ManifestBindingInvalidName,
                         .Table = ShaderManifestTable::Bindings,
                         .RecordIndex = i,
                         .Detail = binding.NameString };
            }

            if (binding.FirstUniformMember > environment.UniformMemberCount ||
                binding.UniformMemberCount > environment.UniformMemberCount - binding.FirstUniformMember)
            {
                return { .Code = ErrorCode::ManifestBindingInvalidUniforms,
                         .Table = ShaderManifestTable::Bindings,
                         .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateResourceIndices(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const uint32_t> resourceIndexList =
            MakeTable<uint32_t>(context.Extent, environment.ResourceIndexTableOffset, environment.ResourceIndexCount);
        for (uint32_t i = 0u; i < resourceIndexList.size(); ++i)
        {
            if (resourceIndexList[i] >= environment.BindingCount)
            {
                return { .Code = ErrorCode::InvalidResourceBindingIndex,
                         .Table = ShaderManifestTable::ResourceIndices,
                         .RecordIndex = i,
                         .Detail = resourceIndexList[i] };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateRunTables(const EnvironmentContext& context) noexcept
    {
        struct RunTable
        {
            ShaderManifestTable Table;
            ErrorCode Code;
            uint32_t Offset;
            uint32_t Count;
            uint32_t PayloadCount;
        };

        const EnvironmentHeader& environment = context.Environment;
        const std::array<RunTable, 3u> runTables{
            RunTable{ ShaderManifestTable::ResourceLists,
                      ErrorCode::InvalidResourceListRun,
                      environment.ResourceListTableOffset,
                      environment.ResourceListCount,
                      environment.ResourceIndexCount },
            RunTable{ ShaderManifestTable::FootprintLists,
                      ErrorCode::InvalidFootprintListRun,
                      environment.FootprintListTableOffset,
                      environment.FootprintListCount,
                      environment.FootprintCount },
            RunTable{ ShaderManifestTable::VisibilityLists,
                      ErrorCode::InvalidVisibilityListRun,
                      environment.VisibilityListTableOffset,
                      environment.VisibilityListCount,
                      environment.VisibilityIndexCount },
        };

        for (const RunTable& runTable : runTables)
        {
            const std::span<const Run> runSpan = MakeTable<Run>(context.Extent, runTable.Offset, runTable.Count);
            for (uint32_t i = 0u; i < runSpan.size(); ++i)
            {
                // <= on the sum for the edge case: an empty trailing list starts at the payload count
                const Run& run = runSpan[i];
                if (static_cast<uint64_t>(run.First) + static_cast<uint64_t>(run.Count) > runTable.PayloadCount)
                {
                    return { .Code = runTable.Code, .Table = runTable.Table, .RecordIndex = i };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSlots(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const EntryPointInstance> slotSpan =
            MakeTable<EntryPointInstance>(context.Extent,
                                          environment.SlotTableOffset,
                                          static_cast<uint64_t>(environment.VariantCount) * context.EntryPointCount);
        for (uint32_t i = 0u; i < slotSpan.size(); ++i)
        {
            const EntryPointInstance& slot = slotSpan[i];
            if (slot.SourceIndex >= environment.SourceCount)
            {
                return { .Code = ErrorCode::InvalidSlotSourceIndex,
                         .Table = ShaderManifestTable::Slots,
                         .RecordIndex = i,
                         .Detail = slot.SourceIndex };
            }

            if (slot.VisibilityIndex >= environment.VisibilityListCount)
            {
                return { .Code = ErrorCode::InvalidSlotVisibilityIndex,
                         .Table = ShaderManifestTable::Slots,
                         .RecordIndex = i,
                         .Detail = slot.VisibilityIndex };
            }

            if (slot.RasterIndex >= environment.RasterCount)
            {
                return { .Code = ErrorCode::InvalidSlotRasterIndex,
                         .Table = ShaderManifestTable::Slots,
                         .RecordIndex = i,
                         .Detail = slot.RasterIndex };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVariantKeys(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const VariantKey> variantKeySpan =
            MakeTable<VariantKey>(context.Extent, environment.VariantKeyTableOffset, environment.VariantCount);
        // adjacent_find lets us combine is_sorted and the uniqueness check
        const auto outOfOrder = std::ranges::adjacent_find(variantKeySpan, std::greater_equal<VariantKey>{});
        if (outOfOrder != variantKeySpan.end())
        {
            return { .Code = ErrorCode::InvalidVariantKeyOrder,
                     .Table = ShaderManifestTable::VariantKeys,
                     .RecordIndex = static_cast<uint32_t>(std::distance(variantKeySpan.begin(), outOfOrder)) };
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVariants(const EnvironmentContext& context) noexcept
    {
        // the most complex and annoying pass to handle, but the most beneficial: after this, all
        // cross-references between variants, resource lists, and visibility lists are validated
        // so that all accessors can run unchecked
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const std::byte> extent = context.Extent;
        const std::span<const Variant> variantSpan =
            MakeTable<Variant>(extent, environment.VariantTableOffset, environment.VariantCount);
        const std::span<const Run> resourceLists =
            MakeTable<Run>(extent, environment.ResourceListTableOffset, environment.ResourceListCount);
        const std::span<const uint32_t> resourceIndexList =
            MakeTable<uint32_t>(extent, environment.ResourceIndexTableOffset, environment.ResourceIndexCount);
        const std::span<const Run> visibilityLists =
            MakeTable<Run>(extent, environment.VisibilityListTableOffset, environment.VisibilityListCount);
        const std::span<const uint32_t> visibilityIndices =
            MakeTable<uint32_t>(extent, environment.VisibilityIndexTableOffset, environment.VisibilityIndexCount);
        const std::span<const EntryPointInstance> slotSpan =
            MakeTable<EntryPointInstance>(extent,
                                          environment.SlotTableOffset,
                                          static_cast<uint64_t>(environment.VariantCount) * context.EntryPointCount);
        const size_t entryPointCount = static_cast<size_t>(context.EntryPointCount);

        for (uint32_t vi = 0u; vi < variantSpan.size(); ++vi)
        {
            const Variant& variant = variantSpan[vi];
            if (variant.SuffixString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidVariantSuffixString,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = vi,
                         .Detail = variant.SuffixString };
            }

            if (variant.ResourceListIndex >= environment.ResourceListCount)
            {
                return { .Code = ErrorCode::InvalidResourceListRun,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = vi,
                         .Detail = variant.ResourceListIndex };
            }

            // further open question for footprint lists: should we change it so that the null check
            // is no longer needed? We should have a sentinel value that indicates an empty or null
            // footprint for a resource, since that is still a valid case
            if (variant.FootprintListIndex >= environment.FootprintListCount)
            {
                return { .Code = ErrorCode::InvalidVariantFootprintListIndex,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = vi,
                         .Detail = variant.FootprintListIndex };
            }

            const std::span<const uint32_t> variantResourceIndices =
                RunOf(resourceLists, resourceIndexList, variant.ResourceListIndex);
            const std::span<const EntryPointInstance> variantSlots =
                slotSpan.subspan(static_cast<size_t>(vi) * entryPointCount, entryPointCount);
            for (const EntryPointInstance& slot : variantSlots)
            {
                const std::span<const uint32_t> slotVisibilityIndices =
                    RunOf(visibilityLists, visibilityIndices, slot.VisibilityIndex);
                // absolute offset of this run into the visibility index table, so a bad entry names its row
                const uint32_t runOffset = static_cast<uint32_t>(slotVisibilityIndices.data() - visibilityIndices.data());

                for (uint32_t j = 0u; j < slotVisibilityIndices.size(); ++j)
                {
                    // the entry indexes the variant's resource list, and the resource list was checked
                    // against the binding table already
                    const uint32_t local = slotVisibilityIndices[j];
                    if (local >= variantResourceIndices.size())
                    {
                        return { .Code = ErrorCode::InvalidSlotVisibilityIndex,
                                 .Table = ShaderManifestTable::VisibilityIndices,
                                 .RecordIndex = runOffset + j,
                                 .Detail = local };
                    }
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateAxisMasks(const EnvironmentContext& context) noexcept
    {
        const uint32_t wordCount = context.AxisMaskWordCount;
        const auto usedBitsInLastWord = static_cast<uint32_t>(context.ModuleAxisCount % k_AxisMaskWordBits);
        if (wordCount == 0u || usedBitsInLastWord == 0u)
        {
            return k_ManifestOk;
        }

        const EnvironmentHeader& environment = context.Environment;
        const std::span<const uint64_t> maskSpan =
            MakeTable<uint64_t>(context.Extent,
                                environment.AxisMaskTableOffset,
                                static_cast<uint64_t>(environment.VariantCount) * wordCount);
        // only the last word of each mask can hold a bit past the module's axes
        for (uint32_t vi = 0u; vi < environment.VariantCount; ++vi)
        {
            const uint64_t lastWord = maskSpan[(static_cast<size_t>(vi) * wordCount) + wordCount - 1u];
            if ((lastWord >> usedBitsInLastWord) != 0u)
            {
                return { .Code = ErrorCode::InvalidVariantAxisMask, .Table = ShaderManifestTable::AxisMasks, .RecordIndex = vi };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateRasterStates(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const RasterState> rasterSpan =
            MakeTable<RasterState>(context.Extent, environment.RasterTableOffset, environment.RasterCount);
        for (uint32_t i = 0u; i < rasterSpan.size(); ++i)
        {
            const RasterState& raster = rasterSpan[i];
            if (static_cast<uint64_t>(raster.FirstVertexInput) + raster.VertexInputCount > environment.VertexInputCount)
            {
                return { .Code = ErrorCode::InvalidRasterVertexInputRange,
                         .Table = ShaderManifestTable::Rasters,
                         .RecordIndex = i };
            }

            if (static_cast<uint64_t>(raster.FirstColorTarget) + raster.ColorTargetCount > environment.ColorTargetCount)
            {
                return { .Code = ErrorCode::InvalidRasterColorTargetRange,
                         .Table = ShaderManifestTable::Rasters,
                         .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVertexInputs(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const VertexInput> vertexInputSpan =
            MakeTable<VertexInput>(context.Extent, environment.VertexInputTableOffset, environment.VertexInputCount);
        for (uint32_t i = 0u; i < vertexInputSpan.size(); ++i)
        {
            if (vertexInputSpan[i].SemanticNameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidVertexInput,
                         .Table = ShaderManifestTable::VertexInputs,
                         .RecordIndex = i,
                         .Detail = vertexInputSpan[i].SemanticNameString };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateUniformMembers(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const UniformMember> uniformMemberSpan = MakeTable<UniformMember>(
            context.Extent, environment.UniformMemberTableOffset, environment.UniformMemberCount);
        for (uint32_t i = 0u; i < uniformMemberSpan.size(); ++i)
        {
            if (uniformMemberSpan[i].NameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidUniformMember,
                         .Table = ShaderManifestTable::UniformMembers,
                         .RecordIndex = i,
                         .Detail = uniformMemberSpan[i].NameString };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSpecializationConstants(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const SpecializationConstant> constantSpan =
            MakeTable<SpecializationConstant>(context.Extent,
                                              environment.SpecializationConstantTableOffset,
                                              environment.SpecializationConstantCount);
        for (uint32_t i = 0u; i < constantSpan.size(); ++i)
        {
            if (constantSpan[i].NameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidSpecializationConstant,
                         .Table = ShaderManifestTable::SpecializationConstants,
                         .RecordIndex = i,
                         .Detail = constantSpan[i].NameString };
            }
        }

        return k_ManifestOk;
    }

} // namespace

#ifdef __clang__
#pragma clang diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

} // namespace lodestone::manifest
