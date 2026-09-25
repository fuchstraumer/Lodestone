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
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone::manifest
{

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
            "a profile has an unknown code format",
            "a SPIR-V source does not start on a 4-byte boundary, or its length is not a multiple of 4",
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
    
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    std::span<const RecordType> Map(std::span<const std::byte> bytes, TableRef table_ref) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + table_ref.Offset),
                                            static_cast<size_t>(table_ref.Count) };
    }
    
    template<typename RecordType> requires(std::is_trivially_copyable_v<RecordType> && !std::is_pointer_v<RecordType>)
    std::span<const RecordType> Map(std::span<const std::byte> bytes, TableRef64 table_ref) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + table_ref.Offset),
                                            static_cast<size_t>(table_ref.Count) };
    }
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    /**@brief In multiple locations, we store ranges of data in "runs". Each run specifies a contiguous block
     * of payloads, which could be themselves simple indices or POD structs. This is just a more succinct
     * accessor for those cases  */
    template<typename PayloadType, typename RunIndexType>
    std::span<const PayloadType> RunOf(std::span<const Run> runs,
                                       std::span<const PayloadType> payloads,
                                       RunIndexType run_index) noexcept
    {
        const Run& run = runs[static_cast<size_t>(run_index)];
        return payloads.subspan(run.First, run.Count);
    }

    /** One table to bounds-check: `Count` records of `RecordSize` bytes, starting at `Offset`. */
    struct Section64
    {
        ShaderManifestTable Table{ ShaderManifestTable::Invalid };
        TableRef64 Loc{ 0u, 0u };
        size_t RecordSize{ 0u };
    };

    struct Section
    {
        ShaderManifestTable Table{ ShaderManifestTable::Invalid };
        TableRef Loc{ 0u, 0u };
        size_t RecordSize{ 0u };
    };

    /** The facts one extent's validators need from the bundle, with the extent itself. */
    struct EnvironmentContext
    {
        //NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
        const EnvironmentHeader& Environment;
        std::span<const std::byte> Extent;
        uint64_t StringCount{ 0u };
        uint64_t EntryPointCount{ 0u };
        uint32_t AxisMaskWordCount{ 0u };
        uint32_t ModuleAxisCount{ 0u };
        ShaderCodeFormat CodeFormat{ ShaderCodeFormat::None };
    };

    using BundleValidator = ErrorState (*)(const Header&, std::span<const std::byte>) noexcept;
    using EnvironmentValidator = ErrorState (*)(const EnvironmentContext&) noexcept;

    /** True when a table of `count` records of `record_size` bytes starts at `offset` and stays inside a
     * region of `region_size` bytes. An empty table at any offset is in bounds. */
    bool TableIsInBounds(TableRef loc, size_t record_size, uint64_t region_size) noexcept;
    bool TableIsInBounds(TableRef64 loc, size_t record_size, uint64_t region_size) noexcept;
    /** True when a grid of `rows * columns` records fits a region, checked without an overflow. */
    bool GridFitsRegion(uint64_t rows, uint64_t columns, size_t record_size, uint64_t region_size) noexcept;
    ErrorState ValidateSections(std::span<const Section> sections, uint64_t region_size) noexcept;
    ErrorState ValidateSections(std::span<const Section64> sections, uint64_t region_size) noexcept;
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
    //NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    view.header = reinterpret_cast<const Header*>(bytes.data());
    view.strings = Map<StringRef>(bytes, parsed.Strings);
    view.axes = Map<Axis>(bytes, parsed.Axes);
    view.axisValues = Map<AxisValueType>(bytes, parsed.AxesValues);
    view.profiles = Map<Profile>(bytes, parsed.Profiles);
    const TableRef64 rootLoc{ .Offset = sizeof(Header), .Count = parsed.ModuleCount };
    view.moduleHeaders = Map<ModuleRootHeader>(bytes, rootLoc);
    const TableRef64 envLoc{ .Offset = parsed.EnvironmentDirectoryOffset, .Count = parsed.Profiles.Count * parsed.ModuleCount };
    view.directory = Map<EnvironmentDirectoryEntry>(bytes, envLoc);
    return view;
}

std::string_view BundleView::String(uint32_t string_index) const noexcept
{
    const StringRef& reference = strings[string_index];
    const char* base = reinterpret_cast<const char*>(bytes.data() + header->StringBlobs.Offset);
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
    entryPoints = Map<EntryPoint>(bundle.bytes, moduleHeader.EntryPoints);
    moduleAxes = Map<ModuleAxis>(bundle.bytes, moduleHeader.ModuleAxes);
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
                                      .ModuleAxisCount = view.module.AxisCount(),
                                      .CodeFormat = bundle.Profiles()[profile_index].CodeFormat };

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
    view.variantKeys = Map<VariantKey>(extent_bytes, TableRef{ .Offset = parsed.VariantKeyTableOffset, .Count = parsed.VariantCount });
    view.variants = Map<Variant>(extent_bytes, TableRef{ .Offset = parsed.VariantTableOffset, .Count = parsed.VariantCount });
    const TableRef64 axisMaskTableRef{ .Offset = static_cast<uint64_t>(parsed.AxisMaskTableOffset), .Count = maskWordCount };
    view.axisMasks = Map<uint64_t>(extent_bytes, axisMaskTableRef);
    const TableRef64 slotTableRef{ .Offset = static_cast<uint64_t>(parsed.SlotTableOffset), .Count = slotCount };
    view.slots = Map<EntryPointInstance>(extent_bytes, slotTableRef);
    view.sources = Map<SourceRef>(extent_bytes, parsed.Sources);
    view.sourceBlob = Map<char>(extent_bytes, parsed.SourceBlob);
    view.bindings = Map<Binding>(extent_bytes, parsed.Bindings);
    view.resourceLists = Map<Run>(extent_bytes, parsed.ResourceLists);
    view.resourceIndices = Map<uint32_t>(extent_bytes, parsed.ResourceIndices);
    view.footprints = Map<Footprint>(extent_bytes, parsed.Footprints);
    view.footprintLists = Map<Run>(extent_bytes, parsed.FootprintLists);
    view.visibilityLists = Map<Run>(extent_bytes, parsed.VisibilityLists);
    view.visibilityIndices = Map<uint32_t>(extent_bytes, parsed.VisibilityIndices);
    view.rasterStates = Map<RasterState>(extent_bytes, parsed.Rasters);
    view.vertexInputs = Map<VertexInput>(extent_bytes, parsed.VertexInputs);
    view.colorTargets = Map<ColorTarget>(extent_bytes, parsed.ColorTargets);
    view.uniformMembers = Map<UniformMember>(extent_bytes, parsed.UniformMembers);
    view.specializationConstants = Map<SpecializationConstant>(extent_bytes, parsed.SpecConstants);
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

ShaderCodeFormat EnvironmentView::CodeFormat() const noexcept
{
    return ProfileRecord().CodeFormat;
}

std::string_view EnvironmentView::Source(uint32_t source_index) const noexcept
{
    // if this assert fires on source_index, caller provided invalid index
    assert(source_index < sources.size());
    assert(CodeFormat() == ShaderCodeFormat::Wgsl);
    const SourceRef& reference = sources[source_index];
    return std::string_view{ sourceBlob.data() + reference.Offset, reference.Length };
}

std::span<const uint32_t> EnvironmentView::SpirvWords(uint32_t source_index) const noexcept
{
    assert(source_index < sources.size());
    assert(CodeFormat() == ShaderCodeFormat::Spirv);
    const SourceRef& reference = sources[source_index];
    return std::span<const uint32_t>{ reinterpret_cast<const uint32_t*>(sourceBlob.data() + reference.Offset),
                                      reference.Length / sizeof(uint32_t) };
}

std::span<const VariantKey> EnvironmentView::VariantKeys() const noexcept
{
    return variantKeys;
}

std::span<const Variant> EnvironmentView::Variants() const noexcept
{
    return variants;
}

std::optional<uint32_t> EnvironmentView::FindVariant(VariantKey key) const noexcept
{
    // the key table is sorted and parallel to the variant table, so the position of a key *is* the index of
    // its variant
    const auto keyIter = std::ranges::lower_bound(variantKeys, key);
    if (keyIter == variantKeys.end() || *keyIter != key) [[unlikely]]
    {
        return std::nullopt;
    }

    return static_cast<uint32_t>(std::distance(variantKeys.begin(), keyIter));
}

std::optional<VariantView> EnvironmentView::VariantByKey(VariantKey key) const noexcept
{
    const std::optional<uint32_t> variantIndex = FindVariant(key);
    if (!variantIndex.has_value()) [[unlikely]]
    {
        return std::nullopt;
    }

    return VariantView{ this, variantIndex.value() };
}

VariantView EnvironmentView::VariantAt(uint32_t variant_index) const noexcept
{
    assert(variant_index < variants.size());
    return VariantView{ this, variant_index };
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
    const std::optional<uint32_t> variantIndex = FindVariant(variant_key);
    if (!variantIndex.has_value()) [[unlikely]]
    {
        return nullptr;
    }

    return &VariantSlots(variantIndex.value())[entry_point];
}

std::span<const Binding> EnvironmentView::Bindings() const noexcept
{
    return bindings;
}

const Binding& EnvironmentView::BindingRecord(uint32_t binding_index) const noexcept
{
    return bindings[binding_index];
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
    const RasterState& raster = rasterStates[raster_index];
    return vertexInputs.subspan(raster.FirstVertexInput, raster.VertexInputCount);
}

std::span<const ColorTarget> EnvironmentView::ColorTargets(uint32_t raster_index) const noexcept
{
    const RasterState& raster = rasterStates[raster_index];
    return colorTargets.subspan(raster.FirstColorTarget, raster.ColorTargetCount);
}

bool EnvironmentView::WritesFragDepth(uint32_t raster_index) const noexcept
{
    return rasterStates[raster_index].WritesFragDepth != 0u;
}

std::span<const SpecializationConstant> EnvironmentView::SpecializationConstants() const noexcept
{
    return specializationConstants;
}

static_assert(std::input_iterator<LayoutRange::Iterator>);
static_assert(std::ranges::input_range<LayoutRange>);
static_assert(std::input_iterator<UniformMemberRange::Iterator>);
static_assert(std::ranges::input_range<UniformMemberRange>);

UniformMemberView::UniformMemberView(const EnvironmentView* _environment, const UniformMember* _record) noexcept
    : environment{ _environment },
      record{ _record }
{
}

std::string_view UniformMemberView::Name() const noexcept
{
    return environment->String(record->NameString);
}

uint32_t UniformMemberView::Offset() const noexcept
{
    return record->Offset;
}

uint32_t UniformMemberView::Size() const noexcept
{
    return record->Size;
}

uint32_t UniformMemberView::ArrayCount() const noexcept
{
    return record->ArrayCount;
}

uint32_t UniformMemberView::ElementStride() const noexcept
{
    return record->ElementStride;
}

MatrixLayout UniformMemberView::Layout() const noexcept
{
    return static_cast<MatrixLayout>(record->MatrixLayout);
}

const UniformMember& UniformMemberView::Record() const noexcept
{
    return *record;
}

UniformMemberRange::UniformMemberRange(const EnvironmentView* _environment,
                                       std::span<const UniformMember> _members) noexcept
    : environment{ _environment },
      members{ _members }
{
}

uint32_t UniformMemberRange::Size() const noexcept
{
    return static_cast<uint32_t>(members.size());
}

bool UniformMemberRange::Empty() const noexcept
{
    return members.empty();
}

UniformMemberView UniformMemberRange::operator[](uint32_t position) const noexcept
{
    return UniformMemberView{ environment, &members[position] };
}

ResolvedResource::ResolvedResource(const EnvironmentView* _environment,
                                   const Binding* _record,
                                   const Footprint* _footprint) noexcept
    : environment{ _environment },
      record{ _record },
      footprint{ _footprint }
{
}

std::string_view ResolvedResource::Name() const noexcept
{
    return environment->String(record->NameString);
}

std::string_view ResolvedResource::ScopeName() const noexcept
{
    return environment->String(record->ScopeString);
}

PlacementKind ResolvedResource::Placement() const noexcept
{
    return static_cast<PlacementKind>(record->PlacementKind);
}

PlacementPayload ResolvedResource::PlacementValue() const noexcept
{
    return record->Placement;
}

BindingKind ResolvedResource::Kind() const noexcept
{
    return static_cast<BindingKind>(record->Kind);
}

ResourceShape ResolvedResource::Shape() const noexcept
{
    return static_cast<ResourceShape>(record->Shape);
}

ResourceAccess ResolvedResource::Access() const noexcept
{
    return static_cast<ResourceAccess>(record->Access);
}

TextureFormat ResolvedResource::StorageFormat() const noexcept
{
    return static_cast<TextureFormat>(record->StorageFormat);
}

TextureSampleType ResolvedResource::SampleType() const noexcept
{
    return static_cast<TextureSampleType>(record->SampleType);
}

bool ResolvedResource::IsComparisonSampler() const noexcept
{
    return record->IsComparisonSampler != 0u;
}

uint32_t ResolvedResource::ElementStride() const noexcept
{
    return record->ElementStride;
}

uint32_t ResolvedResource::ArrayCount() const noexcept
{
    return record->ArrayCount;
}

uint64_t ResolvedResource::ByteSize() const noexcept
{
    return record->ByteSize;
}

uint64_t ResolvedResource::ElementCount() const noexcept
{
    return footprint != nullptr ? footprint->ElementCount : 0u;
}

uint32_t ResolvedResource::ExtentX() const noexcept
{
    return footprint != nullptr ? footprint->ExtentX : 0u;
}

uint32_t ResolvedResource::ExtentY() const noexcept
{
    return footprint != nullptr ? footprint->ExtentY : 0u;
}

uint32_t ResolvedResource::ExtentZ() const noexcept
{
    return footprint != nullptr ? footprint->ExtentZ : 0u;
}

UniformMemberRange ResolvedResource::Members() const noexcept
{
    return UniformMemberRange{ environment, environment->UniformMembers(*record) };
}

uint32_t ResolvedResource::Index() const noexcept
{
    // the record points into the binding table, so its distance from the table start is its index

    return static_cast<uint32_t>(record - environment->Bindings().data());
}

const Binding& ResolvedResource::Record() const noexcept
{
    return *record;
}

const Footprint* ResolvedResource::FootprintRecord() const noexcept
{
    return footprint;
}

LayoutRange::LayoutRange(const EnvironmentView* _environment,
                         std::span<const uint32_t> _visible,
                         std::span<const uint32_t> _resources,
                         std::span<const Footprint> _footprints) noexcept
    : environment{ _environment },
      visible{ _visible },
      resources{ _resources },
      footprints{ _footprints }
{
}

uint32_t LayoutRange::Size() const noexcept
{
    return static_cast<uint32_t>(visible.size());
}

bool LayoutRange::Empty() const noexcept
{
    return visible.empty();
}

ResolvedResource LayoutRange::operator[](uint32_t position) const noexcept
{
    // A visibility entry is a position in the resource list of the variant, and the footprint list has
    // the same order. This is the one place that resolves that chain.
    const uint32_t local = visible[position];
    const Footprint* footprint = local < footprints.size() ? &footprints[local] : nullptr;
    return ResolvedResource{ environment, &environment->BindingRecord(resources[local]), footprint };
}

EntryPointInstanceView::EntryPointInstanceView(const EnvironmentView* _environment,
                                               const EntryPointInstance* _slot,
                                               const Variant* _variant) noexcept
    : environment{ _environment },
      slot{ _slot },
      variant{ _variant }
{
}

std::string_view EntryPointInstanceView::Source() const noexcept
{
    return environment->Source(slot->Source);
}

std::span<const uint32_t> EntryPointInstanceView::SpirvWords() const noexcept
{
    return environment->SpirvWords(slot->Source);
}

WorkgroupSize EntryPointInstanceView::Workgroup() const noexcept
{
    return WorkgroupSize{ .X = slot->WorkgroupX, .Y = slot->WorkgroupY, .Z = slot->WorkgroupZ };
}

LayoutRange EntryPointInstanceView::Layout() const noexcept
{
    return LayoutRange{ environment,
                        environment->VisibilityList(slot->Visibility),
                        environment->ResourceList(variant->ResourceList),
                        environment->FootprintList(variant->FootprintList) };
}

uint32_t EntryPointInstanceView::Raster() const noexcept
{
    return slot->Raster;
}

const EntryPointInstance& EntryPointInstanceView::Record() const noexcept
{
    return *slot;
}

VariantView::VariantView(const EnvironmentView* _environment, uint32_t _index) noexcept
    : environment{ _environment },
      index{ _index }
{
}

uint32_t VariantView::Index() const noexcept
{
    return index;
}

VariantKey VariantView::Key() const noexcept
{
    return environment->VariantKeys()[index];
}

std::string_view VariantView::Suffix() const noexcept
{
    return environment->String(Record().SuffixString);
}

bool VariantView::IsAxisActive(uint32_t local_axis) const noexcept
{
    return environment->IsAxisActive(index, local_axis);
}

uint32_t VariantView::EntryPointCount() const noexcept
{
    return static_cast<uint32_t>(environment->Module().EntryPoints().size());
}

EntryPointInstanceView VariantView::EntryPoint(uint32_t entry_point) const noexcept
{
    return EntryPointInstanceView{ environment, &environment->VariantSlots(index)[entry_point], &Record() };
}

const Variant& VariantView::Record() const noexcept
{
    return environment->Variants()[index];
}

ShaderSourceProvider::ShaderSourceProvider(EnvironmentView _view,
                                           uint64_t _generation) noexcept
    : view{ _view },
      generation{ _generation }
{
}

std::string_view ShaderSourceProvider::Source(uint32_t entry_point,
                                              VariantKey variant) const noexcept
{
    const EntryPointInstance* slot = view.FindSlot(entry_point, variant);
    assert(slot != nullptr);
    return view.Source(slot->Source);
}

std::span<const uint32_t> ShaderSourceProvider::SpirvWords(uint32_t entry_point,
                                                           VariantKey variant) const noexcept
{
    const EntryPointInstance* slot = view.FindSlot(entry_point, variant);
    assert(slot != nullptr);
    return view.SpirvWords(slot->Source);
}

LayoutRange ShaderSourceProvider::Bindings(uint32_t entry_point, VariantKey variant) const noexcept
{
    const std::optional<VariantView> variantView = view.VariantByKey(variant);
    assert(variantView.has_value());
    return variantView->EntryPoint(entry_point).Layout();
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

    bool TableIsInBounds(TableRef loc, size_t record_size, uint64_t region_size) noexcept
    {
        if (loc.Count == 0u)
        {
            return true;
        }

        // divide first, so a huge count cannot overflow the multiply
        if (loc.Count > region_size / record_size)
        {
            return false;
        }

        const uint64_t span = loc.Count * record_size;
        return loc.Offset <= region_size && span <= region_size - loc.Offset;
    }

    bool TableIsInBounds(TableRef64 loc, size_t record_size, uint64_t region_size) noexcept
    {
        if (loc.Count == 0u)
        {
            return true;
        }

        // divide first, so a huge count cannot overflow the multiply
        if (loc.Count > region_size / record_size)
        {
            return false;
        }

        const uint64_t span = loc.Count * record_size;
        return loc.Offset <= region_size && span <= region_size - loc.Offset;
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
            if (!TableIsInBounds(section.Loc, section.RecordSize, region_size))
            {
                return { .Code = ErrorCode::SectionOutOfBounds, .Table = section.Table };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSections(std::span<const Section64> sections, uint64_t region_size) noexcept
    {
        for (const Section64& section : sections)
        {
            if (!TableIsInBounds(section.Loc, section.RecordSize, region_size))
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
        const TableRef64 moduleLoc{ sizeof(Header), parsed.ModuleCount };
        const std::array<Section64, 6u> sections
        {
            Section64{ ShaderManifestTable::Modules, moduleLoc, sizeof(ModuleRootHeader) },
            Section64{ ShaderManifestTable::Profiles, parsed.Profiles, sizeof(Profile) },
            Section64{ ShaderManifestTable::Strings, parsed.Strings, sizeof(StringRef) },
            Section64{ ShaderManifestTable::Strings, parsed.StringBlobs, 1u },
            Section64{ ShaderManifestTable::Axes, parsed.Axes, sizeof(Axis) },
            Section64{ ShaderManifestTable::AxisValues, parsed.AxesValues, sizeof(AxisValueType) },
        };

        const ErrorState sectionsValid = ValidateSections(sections, parsed.HeaderSize);
        if (!sectionsValid)
        {
            return sectionsValid;
        }

        // the directory is a grid, so check the product without overflowing it
        const bool gridFits = GridFitsRegion(parsed.Profiles.Count,
                                             parsed.ModuleCount,
                                             sizeof(EnvironmentDirectoryEntry),
                                             parsed.HeaderSize);
        const TableRef64 envLoc{ parsed.EnvironmentDirectoryOffset, parsed.Profiles.Count * parsed.ModuleCount };
        const bool tableInBounds = TableIsInBounds(envLoc,
                                                   sizeof(EnvironmentDirectoryEntry),
                                                   parsed.HeaderSize);
        if (!gridFits || !tableInBounds)
        {
            return { .Code = ErrorCode::SectionOutOfBounds, .Table = ShaderManifestTable::Environments };
        }

        return k_ManifestOk;
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ErrorState ValidateStrings(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const StringRef> stringSpan = Map<StringRef>(bytes, parsed.Strings);
        for (int32_t i = 0; std::cmp_less(i, stringSpan.size()); ++i)
        {
            const StringRef& reference = stringSpan[i];
            if (reference.Length > parsed.StringBlobs.Count || reference.Offset > parsed.StringBlobs.Count - reference.Length)
            {
                return { .Code = ErrorCode::StringOutOfBounds,
                         .Table = ShaderManifestTable::Strings,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        // Each axis names a string and owns a run of values in the axis value table. A literal value has
        // nothing to reference, so only the name, the run, and a named value need checking.
        const std::span<const Axis> axisSpan = Map<Axis>(bytes, parsed.Axes);
        const std::span<const AxisValueType> allAxesValueSpan = Map<AxisValueType>(bytes, parsed.AxesValues);
        for (uint32_t i = 0; i < axisSpan.size(); ++i)
        {
            const Axis& axis = axisSpan[i];
            if (axis.NameString >= parsed.Strings.Count)
            {
                return { .Code = ErrorCode::InvalidAxisName,
                         .Table = ShaderManifestTable::Axes,
                         .RecordIndex = i,
                         .Detail = axis.NameString };
            }

            if (static_cast<uint64_t>(axis.FirstValue) + static_cast<uint64_t>(axis.ValueCount) > parsed.AxesValues.Count)
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
                if (value >= parsed.Strings.Count)
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
        const std::span<const Profile> profileSpan = Map<Profile>(bytes, parsed.Profiles);
        for (uint32_t i = 0; i < profileSpan.size(); ++i)
        {
            const Profile& profile = profileSpan[i];
            if (profile.TargetNameString >= parsed.Strings.Count)
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

            if (profile.CodeFormat < ShaderCodeFormat::Wgsl || profile.CodeFormat > ShaderCodeFormat::Spirv)
            {
                return { .Code = ErrorCode::InvalidProfileCodeFormat,
                         .Table = ShaderManifestTable::Profiles,
                         .RecordIndex = i,
                         .Detail = static_cast<uint32_t>(profile.CodeFormat) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateModuleHeaders(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const TableRef64 modulesLoc{ sizeof(Header), parsed.ModuleCount };
        const std::span<const ModuleRootHeader> moduleSpan = Map<ModuleRootHeader>(bytes, modulesLoc);
        for (int32_t i = 0; std::cmp_less(i, moduleSpan.size()); ++i)
        {
            const ModuleRootHeader& moduleHeader = moduleSpan[i];
            if (moduleHeader.ModuleNameString >= parsed.Strings.Count)
            {
                return { .Code = ErrorCode::InvalidModuleNameString,
                         .Table = ShaderManifestTable::Modules,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = moduleHeader.ModuleNameString };
            }

            if (!TableIsInBounds(moduleHeader.EntryPoints,
                                 sizeof(EntryPoint),
                                 parsed.HeaderSize))
            {
                return { .Code = ErrorCode::SectionOutOfBounds,
                         .Table = ShaderManifestTable::EntryPoints,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }

            if (!TableIsInBounds(moduleHeader.ModuleAxes,
                                 sizeof(ModuleAxis),
                                 parsed.HeaderSize))
            {
                return { .Code = ErrorCode::SectionOutOfBounds,
                         .Table = ShaderManifestTable::ModuleAxes,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateEntryPoints(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const TableRef64 modulesLoc{ sizeof(Header), parsed.ModuleCount };
        for (const ModuleRootHeader& moduleHeader : Map<ModuleRootHeader>(bytes, modulesLoc))
        {
            const std::span<const EntryPoint> entryPoints = Map<EntryPoint>(bytes, moduleHeader.EntryPoints);
            for (int32_t i = 0; std::cmp_less(i, entryPoints.size()); ++i)
            {
                const EntryPoint& entryPoint = entryPoints[i];
                if (entryPoint.NameString >= parsed.Strings.Count)
                {
                    return { .Code = ErrorCode::EntryPointInvalidName,
                             .Table = ShaderManifestTable::EntryPoints,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = entryPoint.NameString };
                }

                if (entryPoint.Stage >= static_cast<uint32_t>(ShaderStageKind::Count) ||
                    entryPoint.Stage == static_cast<uint32_t>(ShaderStageKind::Invalid))
                {
                    return { .Code = ErrorCode::EntryPointInvalidStage,
                             .Table = ShaderManifestTable::EntryPoints,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = entryPoint.Stage };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateModuleAxes(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const std::span<const Axis> axisSpan = Map<Axis>(bytes, parsed.Axes);
        const TableRef64 modulesLoc{ sizeof(Header), parsed.ModuleCount };
        for (const ModuleRootHeader& moduleHeader : Map<ModuleRootHeader>(bytes, modulesLoc))
        {
            const std::span<const ModuleAxis> moduleAxisSpan = Map<ModuleAxis>(bytes, moduleHeader.ModuleAxes);
            for (int32_t i = 0u; std::cmp_less(i, moduleAxisSpan.size()); ++i)
            {
                const ModuleAxis& moduleAxis = moduleAxisSpan[i];
                if (moduleAxis.AxisIndex >= axisSpan.size())
                {
                    return { .Code = ErrorCode::InvalidModuleAxisIndex,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = moduleAxis.AxisIndex };
                }

                // An empty mask gives the axis a radix of zero, and a bit past the root values selects nothing.
                const uint32_t valueCount = axisSpan[moduleAxis.AxisIndex].ValueCount;
                const bool maskPastValues = valueCount < 32u && (moduleAxis.LiveValuesMask >> valueCount) != 0u;
                if (moduleAxis.LiveValuesMask == 0u || maskPastValues)
                {
                    return { .Code = ErrorCode::InvalidModuleAxisValueMask,
                             .Table = ShaderManifestTable::ModuleAxes,
                             .RecordIndex = static_cast<uint32_t>(i),
                             .Detail = moduleAxis.LiveValuesMask };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateDirectory(const Header& parsed, std::span<const std::byte> bytes) noexcept
    {
        const uint64_t dirCount = parsed.Profiles.Count * parsed.ModuleCount;
        const TableRef64 directoryLoc{ parsed.EnvironmentDirectoryOffset, dirCount };
        const std::span<const EnvironmentDirectoryEntry> directorySpan =
            Map<EnvironmentDirectoryEntry>(bytes, directoryLoc);
        for (int32_t i = 0; std::cmp_less(i, directorySpan.size()); ++i)
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
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    //NOLINTBEGIN(modernize-use-designated-initializers)
    ErrorState ValidateEnvironmentSections(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const uint64_t extentSize = context.Extent.size();
        const bool validGrid = GridFitsRegion(environment.VariantCount,
                                              context.EntryPointCount,
                                              sizeof(EntryPointInstance),
                                              extentSize);
        const TableRef64 slotTableLoc{ environment.SlotTableOffset,
                                       static_cast<uint64_t>(environment.VariantCount) * context.EntryPointCount };
        const bool tableInBounds = TableIsInBounds(slotTableLoc,
                                                   sizeof(EntryPointInstance),
                                                   extentSize);

        if (!validGrid || !tableInBounds)
        {
            return { .Code = ErrorCode::SlotGridSizeMismatch,
                     .Table = ShaderManifestTable::Slots,
                     .Detail = environment.VariantCount };
        }

        const TableRef varKeysLoc{ environment.VariantKeyTableOffset, environment.VariantCount };
        const TableRef varsLoc{ environment.VariantTableOffset, environment.VariantCount };
        const TableRef axisMasksLoc{ environment.AxisMaskTableOffset,
                                     environment.VariantCount * context.AxisMaskWordCount };
        const std::array<Section, 17u> sections
        {
            Section{ ShaderManifestTable::VariantKeys, varKeysLoc, sizeof(VariantKey) },
            Section{ ShaderManifestTable::Variants, varsLoc, sizeof(Variant) },
            Section{ ShaderManifestTable::AxisMasks, axisMasksLoc, sizeof(uint64_t) },
            Section{ ShaderManifestTable::Sources, environment.Sources, sizeof(SourceRef) },
            Section{ ShaderManifestTable::Sources, environment.Sources, 1u },
            Section{ ShaderManifestTable::Bindings, environment.Bindings, sizeof(Binding) },
            Section{ ShaderManifestTable::ResourceLists, environment.ResourceLists, sizeof(Run) },
            Section{ ShaderManifestTable::ResourceIndices, environment.ResourceIndices, sizeof(uint32_t) },
            Section{ ShaderManifestTable::Footprints, environment.Footprints, sizeof(Footprint) },
            Section{ ShaderManifestTable::FootprintLists, environment.FootprintLists, sizeof(Run) },
            Section{ ShaderManifestTable::VisibilityLists, environment.VisibilityLists, sizeof(Run) },
            Section{ ShaderManifestTable::VisibilityIndices, environment.VisibilityIndices, sizeof(uint32_t) },
            Section{ ShaderManifestTable::Rasters, environment.Rasters, sizeof(RasterState) },
            Section{ ShaderManifestTable::VertexInputs, environment.VertexInputs, sizeof(VertexInput) },
            Section{ ShaderManifestTable::ColorTargets, environment.ColorTargets, sizeof(ColorTarget) },
            Section{ ShaderManifestTable::UniformMembers, environment.UniformMembers, sizeof(UniformMember) },
            Section{ ShaderManifestTable::SpecializationConstants, environment.SpecConstants, sizeof(SpecializationConstant) },
        };

        return ValidateSections(sections, extentSize);
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ErrorState ValidateSources(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const SourceRef> sourceSpan = Map<SourceRef>(context.Extent, environment.Sources);
        // A SPIR-V source is read as words, so the blob, each offset, and each length must be multiples of 4.
        const bool readAsWords = context.CodeFormat == ShaderCodeFormat::Spirv;
        if (readAsWords && (environment.SourceBlob.Offset % sizeof(uint32_t)) != 0u)
        {
            return ErrorState{ .Code = ErrorCode::SpirvSourceMisaligned,
                               .Table = ShaderManifestTable::Sources,
                               .Detail = environment.SourceBlob.Offset };
        }

        for (int32_t i = 0; std::cmp_less(i, sourceSpan.size()); ++i)
        {
            const SourceRef& reference = sourceSpan[i];
            if (reference.Length > environment.SourceBlob.Count ||
                reference.Offset > environment.SourceBlob.Count - reference.Length)
            {
                return ErrorState{ .Code = ErrorCode::SourceOutOfBounds,
                                   .Table = ShaderManifestTable::Sources,
                                   .RecordIndex = static_cast<uint32_t>(i) };
            }

            if (readAsWords &&
                ((reference.Offset % sizeof(uint32_t)) != 0u || (reference.Length % sizeof(uint32_t)) != 0u))
            {
                return ErrorState{ .Code = ErrorCode::SpirvSourceMisaligned,
                                   .Table = ShaderManifestTable::Sources,
                                   .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateBindings(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const Binding> bindingSpan = Map<Binding>(context.Extent, environment.Bindings);
        for (int32_t i = 0; std::cmp_less(i, bindingSpan.size()); ++i)
        {
            const Binding& binding = bindingSpan[i];
            if (binding.NameString >= context.StringCount || binding.ScopeString >= context.StringCount)
            {
                return { .Code = ErrorCode::ManifestBindingInvalidName,
                         .Table = ShaderManifestTable::Bindings,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = binding.NameString };
            }

            if (binding.FirstUniformMember > environment.UniformMembers.Count ||
                binding.UniformMemberCount > environment.UniformMembers.Count - binding.FirstUniformMember)
            {
                return { .Code = ErrorCode::ManifestBindingInvalidUniforms,
                         .Table = ShaderManifestTable::Bindings,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateResourceIndices(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const uint32_t> resourceIndexList =
            Map<uint32_t>(context.Extent, environment.ResourceIndices);
        for (int32_t i = 0; std::cmp_less(i, resourceIndexList.size()); ++i)
        {
            if (resourceIndexList[i] >= environment.Bindings.Count)
            {
                return { .Code = ErrorCode::InvalidResourceBindingIndex,
                         .Table = ShaderManifestTable::ResourceIndices,
                         .RecordIndex = static_cast<uint32_t>(i),
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
            TableRef Loc;
            uint32_t PayloadCount;
        };

        const EnvironmentHeader& environment = context.Environment;
        const std::array<RunTable, 3u> runTables
        {
            RunTable{ ShaderManifestTable::ResourceLists,
                          ErrorCode::InvalidResourceListRun,
                          environment.ResourceLists,
                          environment.ResourceIndices.Count },
            RunTable{ ShaderManifestTable::FootprintLists,
                          ErrorCode::InvalidFootprintListRun,
                          environment.FootprintLists,
                          environment.Footprints.Count },
            RunTable{ ShaderManifestTable::VisibilityLists,
                          ErrorCode::InvalidVisibilityListRun,
                          environment.VisibilityLists,
                          environment.VisibilityIndices.Count },
        };

        for (const RunTable& runTable : runTables)
        {
            const std::span<const Run> runSpan = Map<Run>(context.Extent, runTable.Loc);
            for (int32_t i = 0; std::cmp_less(i, runSpan.size()); ++i)
            {
                // <= on the sum for the edge case: an empty trailing list starts at the payload count
                const Run& run = runSpan[i];
                if (static_cast<uint64_t>(run.First) + static_cast<uint64_t>(run.Count) > runTable.PayloadCount)
                {
                    return ErrorState{ .Code = runTable.Code,
                                       .Table = runTable.Table,
                                       .RecordIndex = static_cast<uint32_t>(i) };
                }
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSlots(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const uint64_t numSlots = context.EntryPointCount * environment.VariantCount;
        const TableRef64 slotsLoc{ environment.SlotTableOffset, numSlots };
        const std::span<const EntryPointInstance> slotSpan = Map<EntryPointInstance>(context.Extent,
                                                                                     slotsLoc);
        for (int32_t i = 0u; std::cmp_less(i, slotSpan.size()); ++i)
        {
            const EntryPointInstance& slot = slotSpan[i];
            if (slot.Source >= environment.Sources.Count)
            {
                return { .Code = ErrorCode::InvalidSlotSourceIndex,
                         .Table = ShaderManifestTable::Slots,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Source };
            }

            if (slot.Visibility >= environment.VisibilityLists.Count)
            {
                return { .Code = ErrorCode::InvalidSlotVisibilityIndex,
                         .Table = ShaderManifestTable::Slots,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Visibility };
            }

            if (slot.Raster >= environment.Rasters.Count)
            {
                return { .Code = ErrorCode::InvalidSlotRasterIndex,
                         .Table = ShaderManifestTable::Slots,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = slot.Raster };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVariantKeys(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const TableRef varKeysLoc{ environment.VariantKeyTableOffset, environment.VariantCount };
        const std::span<const VariantKey> variantKeySpan = Map<VariantKey>(context.Extent, varKeysLoc);
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
        const TableRef variantsLoc{ environment.VariantTableOffset, environment.VariantCount };
        const std::span<const Variant> variantSpan = Map<Variant>(extent, variantsLoc);
        const std::span<const Run> resourceLists = Map<Run>(extent, environment.ResourceLists);
        const std::span<const uint32_t> resourceIndexList = Map<uint32_t>(extent, environment.ResourceIndices);
        const std::span<const Run> visibilityLists = Map<Run>(extent, environment.VisibilityLists);
        const std::span<const uint32_t> visibilityIndices = Map<uint32_t>(extent, environment.VisibilityIndices);
        
        const uint64_t numSlots = context.EntryPointCount * environment.VariantCount;
        const TableRef64 slotsLoc{ environment.SlotTableOffset, numSlots };
        const std::span<const EntryPointInstance> slotSpan = Map<EntryPointInstance>(extent, slotsLoc);
        const size_t entryPointCount = static_cast<size_t>(context.EntryPointCount);

        for (int32_t variantIdx = 0; std::cmp_less(variantIdx, variantSpan.size()); ++variantIdx)
        {
            const Variant& variant = variantSpan[variantIdx];
            if (variant.SuffixString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidVariantSuffixString,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = static_cast<uint32_t>(variantIdx),
                         .Detail = variant.SuffixString };
            }

            if (variant.ResourceList >= environment.ResourceLists.Count)
            {
                return { .Code = ErrorCode::InvalidResourceListRun,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = static_cast<uint32_t>(variantIdx),
                         .Detail = variant.ResourceList };
            }

            // further open question for footprint lists: should we change it so that the null check
            // is no longer needed? We should have a sentinel value that indicates an empty or null
            // footprint for a resource, since that is still a valid case
            if (variant.FootprintList >= environment.FootprintLists.Count)
            {
                return { .Code = ErrorCode::InvalidVariantFootprintListIndex,
                         .Table = ShaderManifestTable::Variants,
                         .RecordIndex = static_cast<uint32_t>(variantIdx),
                         .Detail = variant.FootprintList };
            }

            const std::span<const uint32_t> variantResourceIndices =
                RunOf(resourceLists, resourceIndexList, variant.ResourceList);
            const std::span<const EntryPointInstance> variantSlots =
                slotSpan.subspan(static_cast<size_t>(variantIdx) * entryPointCount, entryPointCount);
            for (const EntryPointInstance& slot : variantSlots)
            {
                const std::span<const uint32_t> slotVisibilityIndices =
                    RunOf(visibilityLists, visibilityIndices, slot.Visibility);
                // absolute offset of this run into the visibility index table, so a bad entry names its row
                const uint32_t runOffset = static_cast<uint32_t>(slotVisibilityIndices.data() - visibilityIndices.data());

                for (int32_t j = 0; std::cmp_less(j, slotVisibilityIndices.size()); ++j)
                {
                    // the entry indexes the variant's resource list, and the resource list was checked
                    // against the binding table already
                    const uint32_t local = slotVisibilityIndices[j];
                    if (local >= variantResourceIndices.size())
                    {
                        return { .Code = ErrorCode::InvalidSlotVisibilityIndex,
                                 .Table = ShaderManifestTable::VisibilityIndices,
                                 .RecordIndex = runOffset + static_cast<uint32_t>(j),
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
        const TableRef axisMaskLoc{ environment.AxisMaskTableOffset,
                                    environment.VariantCount * wordCount };
        const std::span<const uint64_t> maskSpan = Map<uint64_t>(context.Extent, axisMaskLoc);
        // only the last word of each mask can hold a bit past the module's axes
        for (int32_t i = 0; std::cmp_less(i, environment.VariantCount); ++i)
        {
            const uint64_t lastWord = maskSpan[(static_cast<size_t>(i) * wordCount) + wordCount - 1u];
            if ((lastWord >> usedBitsInLastWord) != 0u)
            {
                return { .Code = ErrorCode::InvalidVariantAxisMask,
                         .Table = ShaderManifestTable::AxisMasks,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateRasterStates(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const RasterState> rasterSpan = Map<RasterState>(context.Extent, environment.Rasters);
        for (int32_t i = 0; std::cmp_less(i, rasterSpan.size()); ++i)
        {
            const RasterState& raster = rasterSpan[i];
            if (raster.FirstVertexInput + raster.VertexInputCount > environment.VertexInputs.Count)
            {
                return { .Code = ErrorCode::InvalidRasterVertexInputRange,
                         .Table = ShaderManifestTable::Rasters,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }

            if (raster.FirstColorTarget + raster.ColorTargetCount > environment.ColorTargets.Count)
            {
                return { .Code = ErrorCode::InvalidRasterColorTargetRange,
                         .Table = ShaderManifestTable::Rasters,
                         .RecordIndex = static_cast<uint32_t>(i) };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateVertexInputs(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const VertexInput> vertexInputSpan = Map<VertexInput>(context.Extent, environment.VertexInputs);
        for (int32_t i = 0; std::cmp_less(i, vertexInputSpan.size()); ++i)
        {
            if (vertexInputSpan[i].SemanticNameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidVertexInput,
                         .Table = ShaderManifestTable::VertexInputs,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = vertexInputSpan[i].SemanticNameString };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateUniformMembers(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const UniformMember> uniformMemberSpan = Map<UniformMember>(context.Extent, environment.UniformMembers);
        for (int32_t i = 0; std::cmp_less(i, uniformMemberSpan.size()); ++i)
        {
            if (uniformMemberSpan[i].NameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidUniformMember,
                         .Table = ShaderManifestTable::UniformMembers,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = uniformMemberSpan[i].NameString };
            }
        }

        return k_ManifestOk;
    }

    ErrorState ValidateSpecializationConstants(const EnvironmentContext& context) noexcept
    {
        const EnvironmentHeader& environment = context.Environment;
        const std::span<const SpecializationConstant> constantSpan =
            Map<SpecializationConstant>(context.Extent, environment.SpecConstants);
        for (int32_t i = 0; std::cmp_less(i, constantSpan.size()); ++i)
        {
            if (constantSpan[i].NameString >= context.StringCount)
            {
                return { .Code = ErrorCode::InvalidSpecializationConstant,
                         .Table = ShaderManifestTable::SpecializationConstants,
                         .RecordIndex = static_cast<uint32_t>(i),
                         .Detail = constantSpan[i].NameString };
            }
        }

        return k_ManifestOk;
    }

} // namespace

} // namespace lodestone::manifest
