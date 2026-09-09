#include "ShaderManifest.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <magic_enum/magic_enum.hpp>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

namespace
{

    constexpr ShaderManifestError k_ManifestOk{ .Code = ShaderManifestErrorCode::Success };

    /** True when a table of `count` records of `record_size` bytes starts at `offset` and stays
     * inside a file of `file_size` bytes. An empty table at any offset is in bounds. */
    bool TableIsInBounds(uint32_t offset, uint32_t count, size_t record_size, size_t file_size) noexcept
    {
        if (count == 0u)
        {
            return true;
        }

        const size_t span = static_cast<size_t>(count) * record_size;
        return offset <= file_size && span <= file_size - offset;
    }

    template<typename RecordType>
    std::span<const RecordType> MakeTable(std::span<const std::byte> bytes,
                                          uint32_t offset,
                                          uint32_t count) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + offset),
                                            count };
    }

    /**@brief In multiple locations, we store ranges of data in "runs". Each run specifies a contiguous block
     * of payloads, which could be themselves simple indices or POD structs. This is just a more succinct
     * accessor for those cases  */
    template<typename PayloadType>
    std::span<const PayloadType> RunOf(std::span<const ManifestRun> runs,
                                       std::span<const PayloadType> payloads,
                                       uint32_t run_index) noexcept
    {
        const ManifestRun& run = runs[static_cast<size_t>(run_index)];
        return payloads.subspan(run.First, run.Count);
    }

    ShaderManifestError CheckManifestHeader(const ShaderManifestHeader& parsed,
                                            const size_t file_size) noexcept
    {
        if (parsed.Magic != k_ShaderManifestMagic)
        {
            return { .Code = ShaderManifestErrorCode::BadMagic };
        }

        if (parsed.Version != k_ShaderManifestVersion)
        {
            return { .Code = ShaderManifestErrorCode::VersionMismatch, .Detail = parsed.Version };
        }

        if (parsed.FileSize != file_size)
        {
            return { .Code = ShaderManifestErrorCode::SizeMismatch, .Detail = parsed.FileSize };
        }

        return k_ManifestOk;
    }

    //NOLINTBEGIN(modernize-use-designated-initializers)
    ShaderManifestError ValidateTablesInRange(const ShaderManifestHeader& parsed,
                                              std::span<const std::byte> bytes) noexcept
    {
        const size_t fileSize = bytes.size();

        // instead of a nasty chain of booleans and ifs on those booleans,
        // just build a table of sections and then validate each one in a loop
        struct Section
        {
            ShaderManifestTable Table;
            uint32_t Offset;
            uint32_t Count;
            size_t RecordSize;
        };

        const std::array<Section, static_cast<size_t>(ShaderManifestTable::Count) + 1> sections
        {
            Section{ ShaderManifestTable::Strings,
                     parsed.StringTableOffset,
                     parsed.StringCount,
                     sizeof(ManifestStringRef) },
            Section{ ShaderManifestTable::Strings,
                     parsed.StringBlobOffset,
                     parsed.StringBlobSize,
                     1u },
            Section{ ShaderManifestTable::Sources,
                     parsed.SourceTableOffset,
                     parsed.SourceCount,
                     sizeof(ManifestStringRef) },
            Section{ ShaderManifestTable::Sources,
                     parsed.SourceBlobOffset,
                     parsed.SourceBlobSize,
                     1u },
            Section{ ShaderManifestTable::Bindings,
                     parsed.BindingTableOffset,
                     parsed.BindingCount,
                     sizeof(ManifestBinding) },
            Section{ ShaderManifestTable::ResourceLists,
                     parsed.ResourceListTableOffset,
                     parsed.ResourceListCount,
                     sizeof(ManifestRun) },
            Section{ ShaderManifestTable::ResourceIndices,
                     parsed.ResourceIndexTableOffset,
                     parsed.ResourceIndexCount,
                     sizeof(uint32_t) },
            Section{ ShaderManifestTable::Footprints,
                     parsed.FootprintTableOffset,
                     parsed.FootprintCount,
                     sizeof(ManifestFootprint) },
            Section{ ShaderManifestTable::FootprintLists,
                     parsed.FootprintListTableOffset,
                     parsed.FootprintListCount,
                     sizeof(ManifestRun) },
            Section{ ShaderManifestTable::VisibilityLists,
                     parsed.VisibilityListTableOffset,
                     parsed.VisibilityListCount,
                     sizeof(ManifestRun) },
            Section{ ShaderManifestTable::VisibilityIndices,
                     parsed.VisibilityIndexTableOffset,
                     parsed.VisibilityIndexCount,
                     sizeof(uint32_t) },
            Section{ ShaderManifestTable::EntryPoints,
                     parsed.EntryPointTableOffset,
                     parsed.EntryPointCount,
                     sizeof(ManifestEntryPoint) },
            Section{ ShaderManifestTable::Slots,
                     parsed.SlotTableOffset,
                     parsed.SlotCount,
                     sizeof(ManifestSlot) },
            Section{ ShaderManifestTable::Variants,
                     parsed.VariantTableOffset,
                     parsed.VariantCount,
                     sizeof(ManifestVariant) },
            Section{ ShaderManifestTable::VariantKeys,
                     parsed.VariantKeyTableOffset,
                     parsed.VariantKeyCount,
                     sizeof(uint64_t) },
            Section{ ShaderManifestTable::Axes,
                     parsed.AxisTableOffset,
                     parsed.AxisCount,
                     sizeof(ManifestAxis) },
            Section{ ShaderManifestTable::AxisValues,
                     parsed.AxisValueTableOffset,
                     parsed.AxisValueCount,
                     sizeof(int64_t) },
            Section{ ShaderManifestTable::Rasters,
                     parsed.RasterTableOffset,
                     parsed.RasterCount,
                     sizeof(ManifestRaster) },
            Section{ ShaderManifestTable::VertexInputs,
                     parsed.VertexInputTableOffset,
                     parsed.VertexInputCount,
                     sizeof(ManifestVertexInput) },
            Section{ ShaderManifestTable::ColorTargets,
                     parsed.ColorTargetTableOffset,
                     parsed.ColorTargetCount,
                     sizeof(ManifestColorTarget) },
            Section{ ShaderManifestTable::UniformMembers,
                     parsed.UniformMemberTableOffset,
                     parsed.UniformMemberCount,
                     sizeof(ManifestUniformMember) },
        };

        for (const Section& section : sections)
        {
            if (!TableIsInBounds(section.Offset, section.Count, section.RecordSize, fileSize))
            {
                return { .Code = ShaderManifestErrorCode::SectionOutOfBounds, .Table = section.Table };
            }
        }

        return k_ManifestOk;
    }
    //NOLINTEND(modernize-use-designated-initializers)

    ShaderManifestError CheckManifestStringBlobs(const ShaderManifestHeader& parsed,
                                                 std::span<const std::byte> bytes) noexcept
    {
        auto stringOutOfBounds = [](const ManifestStringRef& string_ref, const uint32_t blob_size)
        {
            return string_ref.Length > blob_size || string_ref.Offset > blob_size - string_ref.Length;
        };

        const std::span<const ManifestStringRef> stringSpan =
            MakeTable<ManifestStringRef>(bytes, parsed.StringTableOffset, parsed.StringCount);
        for (uint32_t i = 0u; i < stringSpan.size(); ++i)
        {
            if (stringOutOfBounds(stringSpan[i], parsed.StringBlobSize))
            {
                return { .Code = ShaderManifestErrorCode::StringOutOfBounds,
                         .Table = ShaderManifestTable::Strings,
                         .RecordIndex = i };
            }
        }

        const std::span<const ManifestStringRef> sourceSpan =
            MakeTable<ManifestStringRef>(bytes, parsed.SourceTableOffset, parsed.SourceCount);
        for (uint32_t i = 0u; i < sourceSpan.size(); ++i)
        {
            if (stringOutOfBounds(sourceSpan[i], parsed.SourceBlobSize))
            {
                return { .Code = ShaderManifestErrorCode::SourceOutOfBounds,
                         .Table = ShaderManifestTable::Sources,
                         .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

    ShaderManifestError ValidateManifestRunTables(const ShaderManifestHeader& parsed,
                                                  std::span<const std::byte> bytes) noexcept
    {
        auto validManifestRun = [](const ManifestRun& run, const size_t list_size)
        {
            // <= on first check for edge case: module with empty trailing list
            return static_cast<size_t>(run.First) <= list_size &&
                   (static_cast<size_t>(run.First) + static_cast<size_t>(run.Count)) <= list_size;
        };

        // all indices are valid: now validate the runs, as those are indices into the resource index list
        // the visiblity lists then give indices into this list
        const std::span<const ManifestRun> resourceListSpan =
            MakeTable<ManifestRun>(bytes, parsed.ResourceListTableOffset, parsed.ResourceListCount);
        for (uint32_t i = 0u; i < resourceListSpan.size(); ++i)
        {
            if (!validManifestRun(resourceListSpan[i], parsed.ResourceIndexCount))
            {
                return { .Code = ShaderManifestErrorCode::InvalidResourceListRun,
                         .Table = ShaderManifestTable::ResourceLists,
                         .RecordIndex = i };
            }
        }

        // we can't really validate footprints, as they are mostly descriptive and don't have strict
        // referential integrity requirements
        const std::span<const ManifestRun> footprintListSpan =
            MakeTable<ManifestRun>(bytes, parsed.FootprintListTableOffset, parsed.FootprintListCount);
        for (uint32_t i = 0u; i < footprintListSpan.size(); ++i)
        {
            if (!validManifestRun(footprintListSpan[i], parsed.FootprintCount))
            {
                return { .Code = ShaderManifestErrorCode::InvalidFootprintListRun,
                         .Table = ShaderManifestTable::FootprintLists,
                         .RecordIndex = i };
            }
        }

        // now visibility lists
        const std::span<const ManifestRun> visibilityListSpan =
            MakeTable<ManifestRun>(bytes, parsed.VisibilityListTableOffset, parsed.VisibilityListCount);
        for (uint32_t i = 0u; i < visibilityListSpan.size(); ++i)
        {
            if (!validManifestRun(visibilityListSpan[i], parsed.VisibilityIndexCount))
            {
                return { .Code = ShaderManifestErrorCode::InvalidVisibilityListRun,
                         .Table = ShaderManifestTable::VisibilityLists,
                         .RecordIndex = i };
            }
        }

        return k_ManifestOk;
    }

} // namespace

std::string_view ToString(ShaderManifestErrorCode error) noexcept
{
    return magic_enum::enum_name(error);
}

namespace
{
    // One human sentence per error code, in enum order so the index is the code. The static_assert
    // pins the length to the enum, so a new code that forgets a line fails to compile rather than
    // reporting the wrong sentence.
    constexpr std::array<std::string_view, static_cast<size_t>(ShaderManifestErrorCode::Count)>
        k_ErrorDescriptions{
            "no error code was set",
            "no error",
            "the byte span is smaller than the manifest header",
            "the file does not begin with the manifest magic number",
            "the manifest version does not match this reader",
            "the header's file size does not match the byte span",
            "a table or blob section runs past the end of the file",
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
            "the variant key count does not match the variant count",
            "a variant's slot range runs past the slot table",
            "a raster's vertex input range is out of range",
            "a raster's color target range is out of range",
            "a vertex input names a string past the string table",
            "unused: a misspelled duplicate of InvalidSlotVisibilityIndex",
            "a variant's footprint list index points past the footprint list table",
            "a uniform member names a string past the string table",
            "an axis names a string past the string table",
            "an axis's value range runs past the axis value table",
        };

    static_assert(k_ErrorDescriptions.size() == static_cast<size_t>(ShaderManifestErrorCode::Count),
                  "every ShaderManifestErrorCode needs a description; add a line when you add a code");
} // namespace

std::string DescribeShaderManifestError(const ShaderManifestError& error)
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

    // `Detail` carries a different fact per code, so the ones worth spelling out get their own phrasing.
    switch (error.Code)
    {
    case ShaderManifestErrorCode::VersionMismatch:
        out += std::format(" (file is version {}, reader expects {})", error.Detail, k_ShaderManifestVersion);
        break;
    case ShaderManifestErrorCode::SizeMismatch:
        out += std::format(" (header claims {} bytes)", error.Detail);
        break;
    case ShaderManifestErrorCode::TooSmall:
        out += std::format(" (span is only {} bytes)", error.Detail);
        break;
    case ShaderManifestErrorCode::VariantKeyVariantCountMismatch:
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

ShaderManifestView::ShaderManifestView() noexcept = default;

ManifestResult<ShaderManifestView> ShaderManifestView::Open(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < sizeof(ShaderManifestHeader))
    {
        return std::unexpected(ShaderManifestError{ .Code = ShaderManifestErrorCode::TooSmall,
                                                    .Detail = static_cast<uint32_t>(bytes.size()) });
    }

    if ((reinterpret_cast<uintptr_t>(bytes.data()) % 8u) != 0u)
    {
        return std::unexpected(ShaderManifestError{ .Code = ShaderManifestErrorCode::Misaligned });
    }

    ShaderManifestHeader parsed{};
    std::memcpy(&parsed, bytes.data(), sizeof(ShaderManifestHeader));

    const size_t fileSize = bytes.size();
    const ShaderManifestError headerCheck = CheckManifestHeader(parsed, fileSize);
    if (headerCheck.Code != ShaderManifestErrorCode::Success) [[unlikely]]
    {
        return std::unexpected(headerCheck);
    }

    const ShaderManifestError tablesInRangeCheck = ValidateTablesInRange(parsed, bytes);
    if (tablesInRangeCheck.Code != ShaderManifestErrorCode::Success) [[unlikely]]
    {
        return std::unexpected(tablesInRangeCheck);
    }

    const ShaderManifestError stringBlobsCheck = CheckManifestStringBlobs(parsed, bytes);
    if (stringBlobsCheck.Code != ShaderManifestErrorCode::Success) [[unlikely]]
    {
        return std::unexpected(stringBlobsCheck);
    }

    const std::span<const ManifestBinding> bindingSpan =
        MakeTable<ManifestBinding>(bytes, parsed.BindingTableOffset, parsed.BindingCount);
    // validate integrity of binding data
    for (uint32_t i = 0u; i < bindingSpan.size(); ++i)
    {
        const ManifestBinding& binding = bindingSpan[i];
        if (binding.NameString >= parsed.StringCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::ManifestBindingInvalidName,
                                     .Table = ShaderManifestTable::Bindings,
                                     .RecordIndex = i,
                                     .Detail = binding.NameString });
        }

        if (binding.FirstUniformMember > parsed.UniformMemberCount ||
            binding.UniformMemberCount > parsed.UniformMemberCount - binding.FirstUniformMember)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::ManifestBindingInvalidUniforms,
                                     .Table = ShaderManifestTable::Bindings,
                                     .RecordIndex = i });
        }
    }

    // resource index list is also context-free: it's the (potentially repeated) runs of indices
    // of resources used by the variants
    const std::span<const uint32_t> resourceIndexList =
        MakeTable<uint32_t>(bytes, parsed.ResourceIndexTableOffset, parsed.ResourceIndexCount);
    // make sure all indices are within range
    for (uint32_t i = 0u; i < resourceIndexList.size(); ++i)
    {
        if (resourceIndexList[i] >= parsed.BindingCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidResourceBindingIndex,
                                     .Table = ShaderManifestTable::ResourceIndices,
                                     .RecordIndex = i,
                                     .Detail = resourceIndexList[i] });
        }
    }

    // check all the tables of "runs", which are just ranges of indices into other lists
    const ShaderManifestError runTablesCheck = ValidateManifestRunTables(parsed, bytes);
    if (runTablesCheck.Code != ShaderManifestErrorCode::Success)
    {
        return std::unexpected(runTablesCheck);
    }

    const std::span<const ManifestEntryPoint> entryPointSpan =
        MakeTable<ManifestEntryPoint>(bytes, parsed.EntryPointTableOffset, parsed.EntryPointCount);
    for (uint32_t i = 0u; i < entryPointSpan.size(); ++i)
    {
        const ManifestEntryPoint& entryPoint = entryPointSpan[i];
        // check namestring validity and stage correctness
        if (entryPoint.NameString >= parsed.StringCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::EntryPointInvalidName,
                                     .Table = ShaderManifestTable::EntryPoints,
                                     .RecordIndex = i,
                                     .Detail = entryPoint.NameString });
        }

        if (entryPoint.Stage >= static_cast<uint32_t>(ShaderStageKind::Count) ||
            entryPoint.Stage == static_cast<uint32_t>(ShaderStageKind::Invalid))
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::EntryPointInvalidStage,
                                     .Table = ShaderManifestTable::EntryPoints,
                                     .RecordIndex = i,
                                     .Detail = entryPoint.Stage });
        }
    }

    const std::span<const ManifestSlot> slotSpan =
        MakeTable<ManifestSlot>(bytes, parsed.SlotTableOffset, parsed.SlotCount);
    for (uint32_t i = 0u; i < slotSpan.size(); ++i)
    {
        const ManifestSlot& slot = slotSpan[i];
        if (slot.SourceIndex >= parsed.SourceCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidSlotSourceIndex,
                                     .Table = ShaderManifestTable::Slots,
                                     .RecordIndex = i,
                                     .Detail = slot.SourceIndex });
        }

        if (slot.VisibilityIndex >= parsed.VisibilityListCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidSlotVisibilityIndex,
                                     .Table = ShaderManifestTable::Slots,
                                     .RecordIndex = i,
                                     .Detail = slot.VisibilityIndex });
        }

        if (slot.RasterIndex >= parsed.RasterCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidSlotRasterIndex,
                                     .Table = ShaderManifestTable::Slots,
                                     .RecordIndex = i,
                                     .Detail = slot.RasterIndex });
        }
    }

    // variant validation is going to be a bit more complex: going to validate variant keys first
    const std::span<const uint64_t> variantKeySpan =
        MakeTable<uint64_t>(bytes, parsed.VariantKeyTableOffset, parsed.VariantKeyCount);
    // validate all keys are sorted in ascending order and unique
    const auto variantKeysSorted = std::ranges::adjacent_find(variantKeySpan, std::greater_equal<uint64_t>{});
    if (variantKeysSorted != variantKeySpan.end())
    {
        return std::unexpected(ShaderManifestError{
            .Code = ShaderManifestErrorCode::InvalidVariantKeyOrder,
            .Table = ShaderManifestTable::VariantKeys,
            .RecordIndex = static_cast<uint32_t>(std::distance(variantKeySpan.begin(), variantKeysSorted)) });
    }

    // this better be true...
    if (variantKeySpan.size() != parsed.VariantCount)
    {
        return std::unexpected(
            ShaderManifestError{ .Code = ShaderManifestErrorCode::VariantKeyVariantCountMismatch,
                                 .Table = ShaderManifestTable::VariantKeys,
                                 .Detail = parsed.VariantCount });
    }

    // now do referential integrity pass, so that hotpath accessors can make more assumptions and use
    // less branchy logic
    const std::span<const ManifestVariant> variantSpan =
        MakeTable<ManifestVariant>(bytes, parsed.VariantTableOffset, parsed.VariantCount);
    const std::span<const ManifestRun> resourceLists =
        MakeTable<ManifestRun>(bytes, parsed.ResourceListTableOffset, parsed.ResourceListCount);
    const std::span<const uint32_t> visiblityIndices =
        MakeTable<uint32_t>(bytes, parsed.VisibilityIndexTableOffset, parsed.VisibilityIndexCount);
    const std::span<const ManifestRun> visibilityLists =
        MakeTable<ManifestRun>(bytes, parsed.VisibilityListTableOffset, parsed.VisibilityListCount);
    for (uint32_t vi = 0u; vi < variantSpan.size(); ++vi)
    {
        const ManifestVariant& variant = variantSpan[vi];
        if (static_cast<size_t>(variant.FirstSlot) + static_cast<size_t>(variant.SlotCount) >
            parsed.SlotCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::VariantSlotOutOfRange,
                                     .Table = ShaderManifestTable::Variants,
                                     .RecordIndex = vi });
        }

        // now check the resource lists
        if (variant.ResourceListIndex >= parsed.ResourceListCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidResourceListRun,
                                     .Table = ShaderManifestTable::Variants,
                                     .RecordIndex = vi,
                                     .Detail = variant.ResourceListIndex });
        }

        const std::span<const uint32_t> variantResourceIndices =
            RunOf<const uint32_t>(resourceLists, resourceIndexList, variant.ResourceListIndex);
        for (uint32_t i = 0u; i < variant.SlotCount; ++i)
        {
            const uint32_t slotIndex = variant.FirstSlot + i;
            const ManifestSlot& currSlot = slotSpan[slotIndex];
            const std::span<const uint32_t> slotVisibilityIndices =
                RunOf<uint32_t>(visibilityLists, visiblityIndices, currSlot.VisibilityIndex);
            // absolute offset of this run into the visibility index table, so a bad entry names its row
            const auto runOffset =
                static_cast<uint32_t>(slotVisibilityIndices.data() - visiblityIndices.data());

            for (uint32_t j = 0u; j < slotVisibilityIndices.size(); ++j)
            {
                // idx = index into the variant's resource list, which then indexes the *global* resource
                // table (those values are the final values, a ManifestBinding entry)
                const uint32_t idx = slotVisibilityIndices[j];
                const bool valid =
                    idx < variantResourceIndices.size() && variantResourceIndices[idx] < bindingSpan.size();
                if (!valid)
                {
                    return std::unexpected(
                        ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidSlotVisibilityIndex,
                                             .Table = ShaderManifestTable::VisibilityIndices,
                                             .RecordIndex = runOffset + j,
                                             .Detail = idx });
                }
            }
        }

        // further open question for footprint lists: should we change it so that the null check
        // is no longer needed? this is at L700 now, in `MakeBindingInfo`. We should have a sentinel
        // value that indicates an empty or null footprint for a resource, since that is still a valid case
        if (variant.FootprintListIndex >= parsed.FootprintListCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidVariantFootprintListIndex,
                                     .Table = ShaderManifestTable::Variants,
                                     .RecordIndex = vi,
                                     .Detail = variant.FootprintListIndex });
        }
    }

    const std::span<const ManifestRaster> rasterSpan =
        MakeTable<ManifestRaster>(bytes, parsed.RasterTableOffset, parsed.RasterCount);
    for (uint32_t i = 0u; i < rasterSpan.size(); ++i)
    {
        const ManifestRaster& raster = rasterSpan[i];
        if (static_cast<size_t>(raster.FirstVertexInput) + static_cast<size_t>(raster.VertexInputCount) >
            parsed.VertexInputCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidRasterVertexInputRange,
                                     .Table = ShaderManifestTable::Rasters,
                                     .RecordIndex = i });
        }

        if (static_cast<size_t>(raster.FirstColorTarget) + static_cast<size_t>(raster.ColorTargetCount) >
            parsed.ColorTargetCount)
        {
            return std::unexpected(
                ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidRasterColorTargetRange,
                                     .Table = ShaderManifestTable::Rasters,
                                     .RecordIndex = i });
        }
    }

    const std::span<const ManifestVertexInput> vertexInputSpan =
        MakeTable<ManifestVertexInput>(bytes, parsed.VertexInputTableOffset, parsed.VertexInputCount);
    for (uint32_t i = 0u; i < vertexInputSpan.size(); ++i)
    {
        if (vertexInputSpan[i].SemanticNameString >= parsed.StringCount)
        {
            return std::unexpected(ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidVertexInput,
                                                        .Table = ShaderManifestTable::VertexInputs,
                                                        .RecordIndex = i,
                                                        .Detail = vertexInputSpan[i].SemanticNameString });
        }
    }

    const std::span<const ManifestUniformMember> uniformMemberSpan =
        MakeTable<ManifestUniformMember>(bytes, parsed.UniformMemberTableOffset, parsed.UniformMemberCount);
    for (uint32_t i = 0u; i < uniformMemberSpan.size(); ++i)
    {
        if (uniformMemberSpan[i].NameString >= parsed.StringCount)
        {
            return std::unexpected(ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidUniformMember,
                                                        .Table = ShaderManifestTable::UniformMembers,
                                                        .RecordIndex = i,
                                                        .Detail = uniformMemberSpan[i].NameString });
        }
    }

    // Each axis names a string and owns a run of values in the axis value table. The values themselves
    // are plain int64 data with nothing to reference, so only the name and the run need checking.
    const std::span<const ManifestAxis> axisSpan =
        MakeTable<ManifestAxis>(bytes, parsed.AxisTableOffset, parsed.AxisCount);
    for (uint32_t i = 0u; i < axisSpan.size(); ++i)
    {
        const ManifestAxis& axis = axisSpan[i];
        if (axis.NameString >= parsed.StringCount)
        {
            return std::unexpected(ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidAxisName,
                                                        .Table = ShaderManifestTable::Axes,
                                                        .RecordIndex = i,
                                                        .Detail = axis.NameString });
        }

        if (static_cast<size_t>(axis.FirstValue) + static_cast<size_t>(axis.ValueCount) > parsed.AxisValueCount)
        {
            return std::unexpected(ShaderManifestError{ .Code = ShaderManifestErrorCode::InvalidAxisValueRange,
                                                        .Table = ShaderManifestTable::Axes,
                                                        .RecordIndex = i,
                                                        .Detail = axis.ValueCount });
        }
    }

    ShaderManifestView view;
    view.bytes = bytes;
    view.header = reinterpret_cast<const ShaderManifestHeader*>(bytes.data());
    view.strings = MakeTable<ManifestStringRef>(bytes, parsed.StringTableOffset, parsed.StringCount);
    view.sources = MakeTable<ManifestStringRef>(bytes, parsed.SourceTableOffset, parsed.SourceCount);
    view.bindings = bindingSpan;
    view.resourceLists = resourceLists;
    view.resourceIndices = resourceIndexList;
    view.footprints = MakeTable<ManifestFootprint>(bytes, parsed.FootprintTableOffset, parsed.FootprintCount);
    view.footprintLists =
        MakeTable<ManifestRun>(bytes, parsed.FootprintListTableOffset, parsed.FootprintListCount);
    view.visibilityLists =
        MakeTable<ManifestRun>(bytes, parsed.VisibilityListTableOffset, parsed.VisibilityListCount);
    view.visibilityIndices =
        MakeTable<uint32_t>(bytes, parsed.VisibilityIndexTableOffset, parsed.VisibilityIndexCount);
    view.entryPoints = entryPointSpan;
    view.slots = MakeTable<ManifestSlot>(bytes, parsed.SlotTableOffset, parsed.SlotCount);
    view.variants = MakeTable<ManifestVariant>(bytes, parsed.VariantTableOffset, parsed.VariantCount);
    view.variantKeys = MakeTable<uint64_t>(bytes, parsed.VariantKeyTableOffset, parsed.VariantKeyCount);
    view.axes = MakeTable<ManifestAxis>(bytes, parsed.AxisTableOffset, parsed.AxisCount);
    view.axisValues = MakeTable<int64_t>(bytes, parsed.AxisValueTableOffset, parsed.AxisValueCount);
    view.rasterStates = MakeTable<ManifestRaster>(bytes, parsed.RasterTableOffset, parsed.RasterCount);
    view.vertexInputs =
        MakeTable<ManifestVertexInput>(bytes, parsed.VertexInputTableOffset, parsed.VertexInputCount);
    view.colorTargets =
        MakeTable<ManifestColorTarget>(bytes, parsed.ColorTargetTableOffset, parsed.ColorTargetCount);
    view.uniformMembers =
        MakeTable<ManifestUniformMember>(bytes, parsed.UniformMemberTableOffset, parsed.UniformMemberCount);

    return view;
}

std::string_view ShaderManifestView::String(uint32_t string_index) const noexcept
{
    assert(header != nullptr && string_index < strings.size());
    const ManifestStringRef& reference = strings[string_index];
    const char* base = reinterpret_cast<const char*>(bytes.data() + header->StringBlobOffset);
    return std::string_view{ base + reference.Offset, reference.Length };
}

std::string_view ShaderManifestView::ModuleName() const noexcept
{
    assert(header != nullptr);
    return String(header->ModuleNameString);
}

std::string_view ShaderManifestView::Source(uint32_t source_index) const noexcept
{
    // if this assert fires on source_index, caller provided invalid index
    assert(header != nullptr && source_index < sources.size());
    const ManifestStringRef& reference = sources[source_index];
    const char* base = reinterpret_cast<const char*>(bytes.data() + header->SourceBlobOffset);
    return std::string_view{ base + reference.Offset, reference.Length };
}

std::span<const ManifestBinding> ShaderManifestView::Bindings() const noexcept
{
    return bindings;
}

std::span<const ManifestSlot> ShaderManifestView::SlotTable() const noexcept
{
    return slots;
}

std::span<const ManifestSlot> ShaderManifestView::Slots(const ManifestVariant& variant) const noexcept
{
    return slots.subspan(variant.FirstSlot, variant.SlotCount);
}

std::span<const uint32_t> ShaderManifestView::ResourceList(uint32_t list_index) const noexcept
{
    return RunOf(resourceLists, resourceIndices, list_index);
}

std::span<const ManifestFootprint> ShaderManifestView::FootprintList(uint32_t list_index) const noexcept
{
    return RunOf(footprintLists, footprints, list_index);
}

std::span<const uint32_t> ShaderManifestView::VisibilityList(uint32_t list_index) const noexcept
{
    return RunOf(visibilityLists, visibilityIndices, list_index);
}

std::span<const ManifestEntryPoint> ShaderManifestView::EntryPoints() const noexcept
{
    return entryPoints;
}

std::span<const ManifestVariant> ShaderManifestView::Variants() const noexcept
{
    return variants;
}

std::span<const ManifestAxis> ShaderManifestView::Axes() const noexcept
{
    return axes;
}

std::span<const int64_t> ShaderManifestView::AxisValues(uint32_t axis_index) const noexcept
{
    if (axis_index >= axes.size())
    {
        return {};
    }

    const ManifestAxis& axis = axes[axis_index];
    if (axis.FirstValue > axisValues.size() || axis.ValueCount > axisValues.size() - axis.FirstValue)
    {
        return {};
    }

    return axisValues.subspan(axis.FirstValue, axis.ValueCount);
}

std::span<const ManifestVertexInput> ShaderManifestView::VertexInputs(uint32_t raster_index) const noexcept
{
    assert(raster_index < rasterStates.size());
    const ManifestRaster& raster = rasterStates[raster_index];
    return vertexInputs.subspan(raster.FirstVertexInput, raster.VertexInputCount);
}

std::span<const ManifestColorTarget> ShaderManifestView::ColorTargets(uint32_t raster_index) const noexcept
{
    assert(raster_index < rasterStates.size());
    const ManifestRaster& raster = rasterStates[raster_index];
    return colorTargets.subspan(raster.FirstColorTarget, raster.ColorTargetCount);
}

bool ShaderManifestView::WritesFragDepth(uint32_t raster_index) const noexcept
{
    assert(raster_index < rasterStates.size());
    return rasterStates[raster_index].WritesFragDepth != 0u;
}

std::span<const ManifestUniformMember> ShaderManifestView::UniformMembers(
    const ManifestBinding& binding) const noexcept
{
    return uniformMembers.subspan(binding.FirstUniformMember, binding.UniformMemberCount);
}

const ManifestSlot* ShaderManifestView::FindSlot(uint32_t entry_point, uint64_t variant_key) const noexcept
{
    const auto keyIter = std::ranges::lower_bound(variantKeys, variant_key);
    if (keyIter == variantKeys.end() || *keyIter != variant_key) [[unlikely]]
    {
        return nullptr;
    }
    // after the so called "phase E" changes to data driven permutations, the location of a key in the
    // variantKeys array *is* the dense index of the variant
    const size_t variantIndex = static_cast<size_t>(std::distance(variantKeys.begin(), keyIter));
    const ManifestVariant& variant = variants[variantIndex];
    return &slots[variant.FirstSlot + entry_point];
}

ManifestShaderSourceProvider::ManifestShaderSourceProvider(ShaderManifestView _view,
                                                           uint64_t _generation) noexcept
    : view{ _view },
      generation{ _generation }
{
    const std::span<const ManifestBinding> records = view.Bindings();
    bindingInfos.reserve(records.size());

    size_t totalMembers = 0u;
    for (const ManifestBinding& record : records)
    {
        totalMembers += record.UniformMemberCount;
    }

    memberInfos.reserve(totalMembers);
    for (const ManifestBinding& record : records)
    {
        for (const ManifestUniformMember& member : view.UniformMembers(record))
        {
            UniformMemberInfo info;
            info.Name = view.String(member.NameString);
            info.Offset = member.Offset;
            info.Size = member.Size;
            info.ArrayCount = member.ArrayCount;
            memberInfos.push_back(info);
        }
    }

    // Where each resource's members start, so a gathered binding can point at them.
    std::vector<uint32_t> memberOffsets;
    memberOffsets.reserve(records.size());
    uint32_t memberCursor = 0u;
    for (const ManifestBinding& record : records)
    {
        memberOffsets.push_back(memberCursor);
        memberCursor += record.UniformMemberCount;
    }

    const std::span<const ManifestSlot> allSlots = view.SlotTable();
    slotFirstBinding.assign(allSlots.size(), 0u);
    slotBindingCount.assign(allSlots.size(), 0u);

    for (const ManifestVariant& variant : view.Variants())
    {
        GatherVariantBindings(variant, memberOffsets);
    }
}

void ManifestShaderSourceProvider::GatherVariantBindings(const ManifestVariant& variant,
                                                         const std::vector<uint32_t>& member_offsets)
{
    const std::span<const ManifestBinding> records = view.Bindings();
    const std::span<const ManifestSlot> allSlots = view.SlotTable();
    const std::span<const uint32_t> resources = view.ResourceList(variant.ResourceListIndex);
    const std::span<const ManifestFootprint> footprints = view.FootprintList(variant.FootprintListIndex);

    for (uint32_t i = 0u; i < variant.SlotCount && variant.FirstSlot + i < allSlots.size(); ++i)
    {
        const uint32_t slotIndex = variant.FirstSlot + i;
        const std::span<const uint32_t> visible = view.VisibilityList(allSlots[slotIndex].VisibilityIndex);

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

BindingInfo ManifestShaderSourceProvider::MakeBindingInfo(const ManifestBinding& record,
                                                          const ManifestFootprint* footprint,
                                                          uint32_t member_offset) const noexcept
{
    BindingInfo info;
    info.Name = view.String(record.NameString);
    info.ScopeName = view.String(record.ScopeString);
    info.Group = record.Group;
    info.Binding = record.Binding;
    info.Kind = static_cast<BindingKind>(record.Kind);
    info.ElementStride = record.ElementStride;
    info.ByteSize = record.ByteSize;
    info.ArrayCount = record.ArrayCount;
    info.Shape = static_cast<ResourceShape>(record.Shape);
    info.SampleType = static_cast<TextureSampleType>(record.SampleType);
    info.StorageFormat = static_cast<TextureFormat>(record.StorageFormat);
    info.StorageAccess = static_cast<StorageTextureAccess>(record.StorageAccess);
    info.SamplerType = static_cast<SamplerBindingType>(record.SamplerType);

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

ManifestShaderSourceProvider::~ManifestShaderSourceProvider() = default;

std::string_view ManifestShaderSourceProvider::Source(uint32_t entry_point,
                                                      uint32_t variant_index) const noexcept
{
    const ManifestSlot* slot = view.FindSlot(entry_point, variant_index);
    if (slot == nullptr)
    {
        return {};
    }

    return view.Source(slot->SourceIndex);
}

std::span<const BindingInfo> ManifestShaderSourceProvider::Bindings(uint32_t entry_point,
                                                                    uint32_t variant_index) const noexcept
{
    const ManifestSlot* slot = view.FindSlot(entry_point, variant_index);
    if (slot == nullptr)
    {
        return {};
    }

    const size_t slotIndex = static_cast<size_t>(slot - view.SlotTable().data());
    if (slotIndex >= slotBindingCount.size() || slotBindingCount[slotIndex] == 0u)
    {
        return {};
    }

    return std::span<const BindingInfo>{ bindingInfos.data() + slotFirstBinding[slotIndex],
                                         slotBindingCount[slotIndex] };
}

WorkgroupSize ManifestShaderSourceProvider::Workgroup(uint32_t entry_point,
                                                      uint32_t variant_index) const noexcept
{
    const ManifestSlot* slot = view.FindSlot(entry_point, variant_index);
    if (slot == nullptr)
    {
        return WorkgroupSize{ .X = 0, .Y = 0, .Z = 0 };
    }

    return WorkgroupSize{ .X = slot->WorkgroupX, .Y = slot->WorkgroupY, .Z = slot->WorkgroupZ };
}

uint64_t ManifestShaderSourceProvider::Generation() const noexcept
{
    return generation;
}

const ShaderManifestView& ManifestShaderSourceProvider::View() const noexcept
{
    return view;
}

} // namespace lodestone

#ifdef __clang__
#pragma clang diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
