#include "ShaderManifest.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <iterator>
#include <magic_enum/magic_enum.hpp>
#include <ranges>
#include <span>
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

    bool BlobIsInBounds(uint32_t offset, uint32_t size, size_t file_size) noexcept
    {
        if (size == 0u)
        {
            return true;
        }

        return offset <= file_size && size <= file_size - offset;
    }

    template<typename RecordType>
    std::span<const RecordType> MakeTable(std::span<const std::byte> bytes,
                                          uint32_t offset,
                                          uint32_t count) noexcept
    {
        return std::span<const RecordType>{ reinterpret_cast<const RecordType*>(bytes.data() + offset),
                                            count };
    }

    /**@brief In multiple locations, we store ranges of data in "runs". Each run specifies a contiguous block of
     * payloads, which could be themselves simple indices or POD structs. This is just a more succinct accessor for
     * those cases  */
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
            return ShaderManifestError::BadMagic;
        }

        if (parsed.Version != k_ShaderManifestVersion)
        {
            return ShaderManifestError::VersionMismatch;
        }

        if (parsed.FileSize != file_size)
        {
            return ShaderManifestError::SizeMismatch;
        }

        return ShaderManifestError::Success;
    }

    ShaderManifestError ValidateTablesInRange(const ShaderManifestHeader& parsed,
                                              std::span<const std::byte> bytes) noexcept
    {
        const size_t fileSize = bytes.size();
        const bool sectionsFit =
            TableIsInBounds(
                parsed.StringTableOffset, parsed.StringCount, sizeof(ManifestStringRef), fileSize) &&
            BlobIsInBounds(parsed.StringBlobOffset, parsed.StringBlobSize, fileSize) &&
            TableIsInBounds(
                parsed.SourceTableOffset, parsed.SourceCount, sizeof(ManifestStringRef), fileSize) &&
            BlobIsInBounds(parsed.SourceBlobOffset, parsed.SourceBlobSize, fileSize) &&
            TableIsInBounds(
                parsed.BindingTableOffset, parsed.BindingCount, sizeof(ManifestBinding), fileSize) &&
            TableIsInBounds(
                parsed.ResourceListTableOffset, parsed.ResourceListCount, sizeof(ManifestRun), fileSize) &&
            TableIsInBounds(
                parsed.ResourceIndexTableOffset, parsed.ResourceIndexCount, sizeof(uint32_t), fileSize) &&
            TableIsInBounds(
                parsed.FootprintTableOffset, parsed.FootprintCount, sizeof(ManifestFootprint), fileSize) &&
            TableIsInBounds(
                parsed.FootprintListTableOffset, parsed.FootprintListCount, sizeof(ManifestRun), fileSize) &&
            TableIsInBounds(parsed.VisibilityListTableOffset,
                            parsed.VisibilityListCount,
                            sizeof(ManifestRun),
                            fileSize) &&
            TableIsInBounds(
                parsed.VisibilityIndexTableOffset, parsed.VisibilityIndexCount, sizeof(uint32_t), fileSize) &&
            TableIsInBounds(
                parsed.EntryPointTableOffset, parsed.EntryPointCount, sizeof(ManifestEntryPoint), fileSize) &&
            TableIsInBounds(parsed.SlotTableOffset, parsed.SlotCount, sizeof(ManifestSlot), fileSize) &&
            TableIsInBounds(
                parsed.VariantTableOffset, parsed.VariantCount, sizeof(ManifestVariant), fileSize) &&
            TableIsInBounds(
                parsed.VariantKeyTableOffset, parsed.VariantKeyCount, sizeof(uint64_t), fileSize) &&
            TableIsInBounds(parsed.AxisTableOffset, parsed.AxisCount, sizeof(ManifestAxis), fileSize) &&
            TableIsInBounds(parsed.AxisValueTableOffset, parsed.AxisValueCount, sizeof(int64_t), fileSize) &&
            TableIsInBounds(parsed.RasterTableOffset, parsed.RasterCount, sizeof(ManifestRaster), fileSize) &&
            TableIsInBounds(parsed.VertexInputTableOffset,
                            parsed.VertexInputCount,
                            sizeof(ManifestVertexInput),
                            fileSize) &&
            TableIsInBounds(parsed.ColorTargetTableOffset,
                            parsed.ColorTargetCount,
                            sizeof(ManifestColorTarget),
                            fileSize) &&
            TableIsInBounds(parsed.UniformMemberTableOffset,
                            parsed.UniformMemberCount,
                            sizeof(ManifestUniformMember),
                            fileSize);

        if (!sectionsFit)
        {
            return ShaderManifestError::SectionOutOfBounds;
        }

        return ShaderManifestError::Success;
    }

    ShaderManifestError CheckManifestStringBlobs(const ShaderManifestHeader& parsed,
                                                 std::span<const std::byte> bytes) noexcept
    {
        auto validateStrInRange = [&](const ManifestStringRef& string_ref, const uint32_t table_size)
        {
            return string_ref.Length > table_size || string_ref.Offset > table_size - string_ref.Length;
        };

        const std::span<const ManifestStringRef> stringSpan =
            MakeTable<ManifestStringRef>(bytes, parsed.StringTableOffset, parsed.StringCount);
        const bool anyOutOfBounds =
            std::ranges::any_of(stringSpan,
                                [&](const ManifestStringRef& string_ref)
                                {
                                    return validateStrInRange(string_ref, parsed.StringBlobSize);
                                });
        if (anyOutOfBounds)
        {
            return ShaderManifestError::StringOutOfBounds;
        }

        const std::span<const ManifestStringRef> sourceSpan =
            MakeTable<ManifestStringRef>(bytes, parsed.SourceTableOffset, parsed.SourceCount);
        const bool anySourceOutOfBounds =
            std::ranges::any_of(sourceSpan,
                                [&](const ManifestStringRef& string_ref)
                                {
                                    return validateStrInRange(string_ref, parsed.SourceBlobSize);
                                });
        if (anySourceOutOfBounds)
        {
            return ShaderManifestError::SourceOutOfBounds;
        }

        return ShaderManifestError::Success;
    }

    ShaderManifestError ValidateManifestRunTables(const ShaderManifestHeader& parsed,
                                                  std::span<const std::byte> bytes) noexcept
    {
        auto validManifestRun = [&](const ManifestRun& run, const size_t list_size)
        {
            // <= on first check for edge case: module with empty trailing list
            return static_cast<size_t>(run.First) <= list_size &&
                   (static_cast<size_t>(run.First) + static_cast<size_t>(run.Count)) <= list_size;
        };

        // all indices are valid: now validate the runs, as those are indices into the resource index list
        // the visiblity lists then give indices into this list
        const std::span<const ManifestRun> resourceListSpan =
            MakeTable<ManifestRun>(bytes, parsed.ResourceListTableOffset, parsed.ResourceListCount);
        const bool allResourceListRunsValid =
            std::ranges::all_of(resourceListSpan,
                                [&](const ManifestRun& run)
                                {
                                    return validManifestRun(run, parsed.ResourceIndexCount);
                                });

        if (!allResourceListRunsValid)
        {
            return ShaderManifestError::InvalidResourceListRun;
        }

        // we can't really validate footprints, as they are mostly descriptive and don't have strict
        // referential integrity requirements
        const std::span<const ManifestRun> footprintListSpan =
            MakeTable<ManifestRun>(bytes, parsed.FootprintListTableOffset, parsed.FootprintListCount);
        const bool allFootprintListRunsValid =
            std::ranges::all_of(footprintListSpan,
                                [&](const ManifestRun& run)
                                {
                                    return validManifestRun(run, parsed.FootprintCount);
                                });

        if (!allFootprintListRunsValid)
        {
            return ShaderManifestError::InvalidFootprintListRun;
        }

        // now visibility lists
        const std::span<const ManifestRun> visibilityListSpan =
            MakeTable<ManifestRun>(bytes, parsed.VisibilityListTableOffset, parsed.VisibilityListCount);
        const bool allVisibilityListRunsValid =
            std::ranges::all_of(visibilityListSpan,
                                [&](const ManifestRun& run)
                                {
                                    return validManifestRun(run, parsed.VisibilityIndexCount);
                                });

        if (!allVisibilityListRunsValid)
        {
            return ShaderManifestError::InvalidVisibilityListRun;
        }

        return ShaderManifestError::Success;
    }

} // namespace

std::string_view ToString(ShaderManifestError error) noexcept
{
    return magic_enum::enum_name(error);
}

ShaderManifestView::ShaderManifestView() noexcept = default;

ManifestResult<ShaderManifestView> ShaderManifestView::Open(std::span<const std::byte> bytes) noexcept
{
    if (bytes.size() < sizeof(ShaderManifestHeader))
    {
        return std::unexpected(ShaderManifestError::TooSmall);
    }

    if ((reinterpret_cast<uintptr_t>(bytes.data()) % 8u) != 0u)
    {
        return std::unexpected(ShaderManifestError::Misaligned);
    }

    ShaderManifestHeader parsed{};
    std::memcpy(&parsed, bytes.data(), sizeof(ShaderManifestHeader));

    const size_t fileSize = bytes.size();
    const ShaderManifestError headerCheck = CheckManifestHeader(parsed, fileSize);
    if (headerCheck != ShaderManifestError::Success) [[unlikely]]
    {
        return std::unexpected(headerCheck);
    }

    const ShaderManifestError tablesInRangeCheck = ValidateTablesInRange(parsed, bytes);
    if (tablesInRangeCheck != ShaderManifestError::Success) [[unlikely]]
    {
        return std::unexpected(tablesInRangeCheck);
    }

    const ShaderManifestError stringBlobsCheck = CheckManifestStringBlobs(parsed, bytes);
    if (stringBlobsCheck != ShaderManifestError::Success) [[unlikely]]
    {
        return std::unexpected(stringBlobsCheck);
    }

    const std::span<const ManifestBinding> bindingSpan =
        MakeTable<ManifestBinding>(bytes, parsed.BindingTableOffset, parsed.BindingCount);
    // validate integrity of binding data
    for (const auto& binding : bindingSpan)
    {
        if (binding.NameString >= parsed.StringCount)
        {
            return std::unexpected(ShaderManifestError::ManifestBindingInvalidName);
        }

        if (binding.FirstUniformMember > parsed.UniformMemberCount ||
            binding.UniformMemberCount > parsed.UniformMemberCount - binding.FirstUniformMember)
        {
            return std::unexpected(ShaderManifestError::ManifestBindingInvalidUniforms);
        }
    }

    // resource index list is also context-free: it's the (potentially repeated) runs of indices
    // of resources used by the variants
    const std::span<const uint32_t> resourceIndexList =
        MakeTable<uint32_t>(bytes, parsed.ResourceIndexTableOffset, parsed.ResourceIndexCount);
    // make sure all indices are within range
    const bool allResourceIndicesValid = std::ranges::all_of(resourceIndexList,
                                                             [&](uint32_t index)
                                                             {
                                                                 return index < parsed.BindingCount;
                                                             });
    if (!allResourceIndicesValid)
    {
        return std::unexpected(ShaderManifestError::InvalidResourceBindingIndex);
    }

    // check all the tables of "runs", which are just ranges of indices into other lists
    const ShaderManifestError runTablesCheck = ValidateManifestRunTables(parsed, bytes);
    if (runTablesCheck != ShaderManifestError::Success)
    {
        return std::unexpected(runTablesCheck);
    }

    const std::span<const ManifestEntryPoint> entryPointSpan =
        MakeTable<ManifestEntryPoint>(bytes, parsed.EntryPointTableOffset, parsed.EntryPointCount);
    for (const auto& entryPoint : entryPointSpan)
    {
        // check namestring validity and stage correctness
        if (entryPoint.NameString >= parsed.StringCount)
        {
            return std::unexpected(ShaderManifestError::EntryPointInvalidName);
        }

        if (entryPoint.Stage >= static_cast<uint32_t>(ShaderStageKind::Count) ||
            entryPoint.Stage == static_cast<uint32_t>(ShaderStageKind::Invalid))
        {
            return std::unexpected(ShaderManifestError::EntryPointInvalidStage);
        }
    }

    const std::span<const ManifestSlot> slotSpan =
        MakeTable<ManifestSlot>(bytes, parsed.SlotTableOffset, parsed.SlotCount);
    for (const auto& slot : slotSpan)
    {
        if (slot.SourceIndex >= parsed.SourceCount)
        {
            return std::unexpected(ShaderManifestError::InvalidSlotSourceIndex);
        }

        if (slot.VisibilityIndex >= parsed.VisibilityListCount)
        {
            return std::unexpected(ShaderManifestError::InvalidSlotVisibilityIndex);
        }

        if (slot.RasterIndex >= parsed.RasterCount)
        {
            return std::unexpected(ShaderManifestError::InvalidSlotRasterIndex);
        }
    }

    // variant validation is going to be a bit more complex: going to validate variant keys first
    const std::span<const uint64_t> variantKeySpan =
        MakeTable<uint64_t>(bytes, parsed.VariantKeyTableOffset, parsed.VariantKeyCount);
    // validate all keys are sorted in ascending order and unique
    const auto variantKeysSorted =
        std::ranges::adjacent_find(variantKeySpan, std::greater_equal<uint64_t>{});
    if (variantKeysSorted != variantKeySpan.end())
    {
        return std::unexpected(ShaderManifestError::InvalidVariantKeyOrder);
    }

    // this better be true...
    if (variantKeySpan.size() != parsed.VariantCount)
    {
        return std::unexpected(ShaderManifestError::VariantKeyVariantCountMismatch);
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
    for (const auto& variant : variantSpan)
    {
        if (variant.FirstSlot >= parsed.SlotCount ||
            variant.FirstSlot + variant.SlotCount > parsed.SlotCount)
        {
            return std::unexpected(ShaderManifestError::VariantSlotOutOfRange);
        }

        // now check the resource lists
        if (variant.ResourceListIndex >= parsed.ResourceListCount)
        {
            return std::unexpected(ShaderManifestError::InvalidResourceListRun);
        }

        const std::span<const uint32_t> variantResourceIndices =
            RunOf<const uint32_t>(resourceLists, resourceIndexList, variant.ResourceListIndex);
        for (uint32_t i = 0u; i < variant.SlotCount; ++i)
        {
            const uint32_t slotIndex = variant.FirstSlot + i;
            const ManifestSlot& currSlot = slotSpan[slotIndex];
            const std::span<const uint32_t> slotVisibilityIndices =
                RunOf<uint32_t>(visibilityLists, visiblityIndices, currSlot.VisibilityIndex);
            
            auto validVisiblityIndex = [&variantResourceIndices, &bindingSpan](const uint32_t idx)
            {
                // idx = index into variant index list... which is then an index into the *global* resource table
                // (those values are the final values, a ManifestBinding entry)
                return idx < variantResourceIndices.size() ? variantResourceIndices[idx] < bindingSpan.size() : false;
            };
            const bool allVisibilityIndicesValid = std::ranges::all_of(slotVisibilityIndices, validVisiblityIndex);
            if (!allVisibilityIndicesValid)
            {
                return std::unexpected(ShaderManifestError::InvalidSlotVisibilityIndex);
            }
        }
    }

    const std::span<const ManifestRaster> rasterSpan =
        MakeTable<ManifestRaster>(bytes, parsed.RasterTableOffset, parsed.RasterCount);
    for (const auto& raster : rasterSpan)
    {
        if (raster.FirstVertexInput >= parsed.VertexInputCount ||
            raster.FirstVertexInput + raster.VertexInputCount > parsed.VertexInputCount)
        {
            return std::unexpected(ShaderManifestError::InvalidRasterVertexInputRange);
        }

        if (raster.FirstColorTarget >= parsed.ColorTargetCount ||
            raster.FirstColorTarget + raster.ColorTargetCount > parsed.ColorTargetCount)
        {
            return std::unexpected(ShaderManifestError::InvalidRasterColorTargetRange);
        }
    }

    const std::span<const ManifestVertexInput> vertexInputSpan =
        MakeTable<ManifestVertexInput>(bytes, parsed.VertexInputTableOffset, parsed.VertexInputCount);
    auto validVertexInput = [&](const ManifestVertexInput& vertex_input)
    {
        return vertex_input.SemanticNameString < parsed.StringCount;
    };
    const bool allVertexInputsValid = std::ranges::all_of(vertexInputSpan, validVertexInput);
    if (!allVertexInputsValid)
    {
        return std::unexpected(ShaderManifestError::InvalidVertexInput);
    }

    const std::span<const ManifestUniformMember> uniformMemberSpan =
        MakeTable<ManifestUniformMember>(bytes, parsed.UniformMemberTableOffset, parsed.UniformMemberCount);
    auto validUniformMember = [&](const ManifestUniformMember& uniform_member)
    {
        return uniform_member.NameString < parsed.StringCount;
    };
    const bool allUniformMembersValid = std::ranges::all_of(uniformMemberSpan, validUniformMember);
    if (!allUniformMembersValid)
    {
        return std::unexpected(ShaderManifestError::InvalidUniformMember);
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
    view.footprintLists = MakeTable<ManifestRun>(bytes, parsed.FootprintListTableOffset, parsed.FootprintListCount);
    view.visibilityLists = MakeTable<ManifestRun>(bytes, parsed.VisibilityListTableOffset, parsed.VisibilityListCount);
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
