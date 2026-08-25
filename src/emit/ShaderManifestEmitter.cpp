#include "emit/ShaderManifestEmitter.hpp"
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "permute/PermutationValue.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// todo-ship: in almost all places we are using std::string, we could just use std::vector<std::byte> and
// avoid the string encoding issues. the manifest is a binary file, so we don't need to treat it as text
// anywhere
namespace lodestone
{

namespace
{

    /** Collects strings once and hands back the index of each. Two equal strings share one entry, so
     * the blob holds each binding name a single time however many layouts name it. */
    // todo-ship: There are better ways to do this, and we should absolutely explore them since strings
    // will rapidly become a huge cost as variant count increases
    class StringTableBuilder
    {
        struct StringViewHash
        {
            std::size_t operator()(std::string_view text) const noexcept
            {
                return std::hash<std::string_view>{}(text);
            }
            using is_transparent = void;
        };
    public:
        StringTableBuilder()
        {
            lookup.reserve(1024);
            references.reserve(1024);
            blob.reserve(16384);
        }

        uint32_t Add(std::string_view text)
        {
            const auto found = lookup.find(text);
            if (found != lookup.end())
            {
                return found->second;
            }

            const auto index = static_cast<uint32_t>(references.size());
            references.emplace_back(static_cast<uint32_t>(blob.size()), static_cast<uint32_t>(text.size()));
            blob.append(text);
            lookup.emplace(std::string{ text }, index);
            return index;
        }

        [[nodiscard]] const std::vector<ManifestStringRef>& References() const noexcept
        {
            return references;
        }

        [[nodiscard]] const std::string& Blob() const noexcept
        {
            return blob;
        }

    private:
        std::unordered_map<std::string, uint32_t, StringViewHash, std::equal_to<>> lookup;
        std::vector<ManifestStringRef> references;
        std::string blob;
    };

    /** @brief Writes the actual bytes to the given `out` string. */
    void AppendBytes(std::string& out, const void* data, size_t size)
    {
        out.append(static_cast<const char*>(data), size);
    }

    /** Pads to the next 8-byte boundary. Binding records and axis values hold 64-bit fields, and the
     * reader maps them in place, so every section must start aligned. */
    void AlignTo8(std::string& out)
    {
        while ((out.size() % 8u) != 0u)
        {
            out.push_back('\0');
        }
    }

    /** @brief Appends a table of records to the output string, aligned to 8 bytes. Returns the offset
     *  of the first record in the output string where the new records are now located */
    template<typename RecordType>
    uint32_t AppendTable(std::string& out, const std::vector<RecordType>& records)
    {
        AlignTo8(out);
        const auto offset = static_cast<uint32_t>(out.size());
        if (!records.empty())
        {
            AppendBytes(out, records.data(), records.size() * sizeof(RecordType));
        }

        return offset;
    }

    ManifestBinding MakeBindingRecord(const ReflectedBinding& binding,
                                      StringTableBuilder& strings,
                                      std::vector<ManifestUniformMember>& member_records)
    {
        ManifestBinding record;
        record.ByteSize = binding.ByteSize;
        record.NameString = strings.Add(binding.Name);
        record.ScopeString = strings.Add(binding.ScopeName);
        record.Group = GroupOf(binding);
        record.Binding = BindingOf(binding);
        record.ElementStride = binding.ElementStride;
        record.ArrayCount = binding.ArrayCount;
        record.StorageFormat = static_cast<uint32_t>(binding.StorageFormat);
        record.PlacementKind = static_cast<uint8_t>(
            GetBoundPlacement(binding.Placement) != nullptr ? PlacementKind::Bound : PlacementKind::None);
        record.Kind = static_cast<uint8_t>(binding.Kind);
        record.Shape = static_cast<uint8_t>(binding.Shape);
        record.SampleType = static_cast<uint8_t>(binding.SampleType);
        record.StorageAccess = static_cast<uint8_t>(binding.StorageAccess);
        record.SamplerType = static_cast<uint8_t>(binding.SamplerType);

        record.FirstUniformMember = static_cast<uint32_t>(member_records.size());
        record.UniformMemberCount = static_cast<uint32_t>(binding.UniformMembers.size());
        member_records.reserve(member_records.size() + binding.UniformMembers.size());
        for (const ReflectedUniformMember& member : binding.UniformMembers)
        {
            ManifestUniformMember memberRecord;
            memberRecord.NameString = strings.Add(member.Name);
            memberRecord.Offset = member.Offset;
            memberRecord.Size = member.Size;
            memberRecord.ArrayCount = member.ArrayCount;
            member_records.emplace_back(memberRecord);
        }

        return record;
    }

    ManifestFootprint MakeFootprintRecord(const ResourceFootprint& footprint) noexcept
    {
        if (const BufferFootprint* buffer = std::get_if<BufferFootprint>(&footprint))
        {
            return ManifestFootprint{ .ElementCount = buffer->ElementCount,
                                      .Kind = static_cast<uint32_t>(FootprintKind::Buffer) };
        }

        if (const TextureFootprint* texture = std::get_if<TextureFootprint>(&footprint))
        {
            return ManifestFootprint{ .ExtentX = texture->ExtentX,
                                      .ExtentY = texture->ExtentY,
                                      .ExtentZ = texture->ExtentZ,
                                      .Kind = static_cast<uint32_t>(FootprintKind::Texture) };
        }

        return ManifestFootprint{};
    }

    bool RecordMatchesFootprint(const ManifestFootprint& record, const ResourceFootprint& footprint) noexcept
    {
        const ManifestFootprint expected = MakeFootprintRecord(footprint);
        return record.Kind == expected.Kind && record.ElementCount == expected.ElementCount &&
               record.ExtentX == expected.ExtentX && record.ExtentY == expected.ExtentY &&
               record.ExtentZ == expected.ExtentZ;
    }

    bool RecordMatchesBinding(const ManifestBinding& record, const ReflectedBinding& binding) noexcept
    {
        return record.ByteSize == binding.ByteSize && record.Group == GroupOf(binding) &&
               record.Binding == BindingOf(binding) && record.ElementStride == binding.ElementStride &&
               record.ArrayCount == binding.ArrayCount &&
               record.StorageFormat == static_cast<uint32_t>(binding.StorageFormat) &&
               record.Kind == static_cast<uint8_t>(binding.Kind) &&
               record.Shape == static_cast<uint8_t>(binding.Shape) &&
               record.SampleType == static_cast<uint8_t>(binding.SampleType) &&
               record.StorageAccess == static_cast<uint8_t>(binding.StorageAccess) &&
               record.SamplerType == static_cast<uint8_t>(binding.SamplerType);
    }

    CookResult<ShaderManifestView> OpenManifestForCheck(const CookedModule& module,
                                                        std::span<const std::byte> raw)
    {
        const ManifestResult<ShaderManifestView> opened = ShaderManifestView::Open(raw);
        if (!opened.has_value())
        {
            std::println(stderr,
                         "[shader_cooker] module {} manifest does not open: {}",
                         module.Name,
                         ToString(opened.error()));
            return std::unexpected(CookError::LibraryRoundTripFailed);
        }

        if (opened.value().ModuleName() != module.Name)
        {
            std::println(stderr,
                         "[shader_cooker] manifest names module '{}', but the cook produced '{}'",
                         opened.value().ModuleName(),
                         module.Name);
            return std::unexpected(CookError::LibraryRoundTripFailed);
        }

        return opened.value();
    }

    CookError CheckManifestSource(const CookedModule& module,
                                         const ManifestShaderSourceProvider& provider,
                                         const LibraryVariant& variant,
                                         size_t entry_point_index,
                                         uint16_t entry_point_id)
    {
        const std::string_view expectedSource = ResolveSource(module, variant, entry_point_index);
        if (provider.Source(entry_point_id, variant.Index) == expectedSource)
        {
            return CookError::Success;
        }

        std::println(stderr,
                     "[shader_cooker] manifest returns different text for {} variant {} [{}]",
                     module.EntryPoints[entry_point_index].Name,
                     variant.Index,
                     variant.Description);
        return CookError::ManifestVariantSourceCodeMismatch;
    }

    CookError CheckManifestWorkgroup(const CookedModule& module,
                                            const ManifestShaderSourceProvider& provider,
                                            const LibraryVariant& variant,
                                            size_t entry_point_index,
                                            uint16_t entry_point_id)
    {
        const WorkgroupSize expected = variant.Workgroups[entry_point_index];
        const WorkgroupSize read = provider.Workgroup(entry_point_id, variant.Index);

        if (read.X == expected.X && read.Y == expected.Y && read.Z == expected.Z)
        {
            return CookError::Success;
        }

        std::println(stderr,
                     "[shader_cooker] manifest returns a different workgroup size for {} variant {}",
                     module.EntryPoints[entry_point_index].Name,
                     variant.Index);
        return CookError::ManifestVariantWorkgroupSizeMismatch;
    }

    bool ManifestUniformMembersMatch(const ShaderManifestView& view,
                                     const ManifestBinding& read,
                                     const ReflectedBinding& expected)
    {
        const std::span<const ManifestUniformMember> readMembers = view.UniformMembers(read);
        if (readMembers.size() != expected.UniformMembers.size())
        {
            return false;
        }

        for (size_t memberIndex = 0u; memberIndex < readMembers.size(); ++memberIndex)
        {
            const ReflectedUniformMember& expectedMember = expected.UniformMembers[memberIndex];
            const ManifestUniformMember& readMember = readMembers[memberIndex];

            const bool matches = view.String(readMember.NameString) == expectedMember.Name &&
                                 readMember.Offset == expectedMember.Offset &&
                                 readMember.Size == expectedMember.Size &&
                                 readMember.ArrayCount == expectedMember.ArrayCount;
            if (!matches)
            {
                return false;
            }
        }

        return true;
    }

    CookError CheckManifestLayout(const CookedModule& module,
                                         const ShaderManifestView& view,
                                         const LibraryVariant& variant,
                                         size_t entry_point_index)
    {
        auto readVariantIter = std::ranges::lower_bound(view.Variants(),
                                                        variant.Index,
                                                        {},
                                                        [](const ManifestVariant& candidate)
                                                        {
                                                            return candidate.Index;
                                                        });
        // failed lookup returns next bigger index , need inequality check, oops i almost missed this lol
        if (readVariantIter == view.Variants().end() ||
            readVariantIter->Index != variant.Index)
        {
            std::println(stderr, "[shader_cooker] manifest holds no variant {}", variant.Index);
            return CookError::ManifestMissingVariant;
        }

        const ManifestVariant& readVariant = *readVariantIter;
        const ShaderLayoutView expectedLayout = ResolveLayoutView(module, variant, entry_point_index);

        const std::span<const uint32_t> resources = view.ResourceList(readVariant.ResourceListIndex);
        const std::span<const ManifestFootprint> footprints =
            view.FootprintList(readVariant.FootprintListIndex);
        const std::span<const ManifestSlot> slots = view.Slots(readVariant);

        if (entry_point_index >= slots.size())
        {
            std::println(stderr,
                         "[shader_cooker] manifest variant {} holds no slot {}",
                         variant.Index,
                         entry_point_index);
            return CookError::ManifestVariantMissingEntryPoint;
        }

        const std::span<const uint32_t> visible =
            view.VisibilityList(slots[entry_point_index].VisibilityIndex);

        if (visible.size() != expectedLayout.size())
        {
            std::println(stderr,
                         "[shader_cooker] manifest variant {} entry point {} sees {} resources, the cook "
                         "produced {}",
                         variant.Index,
                         entry_point_index,
                         visible.size(),
                         expectedLayout.size());
            return CookError::ManifestVariantResourceVisibilityMismatch;
        }

        for (size_t i = 0u; i < expectedLayout.size(); ++i)
        {
            const ResolvedBindingView expected = expectedLayout[i];
            const uint32_t local = visible[i];

            if (local >= resources.size() || resources[local] >= view.Bindings().size() ||
                local >= footprints.size())
            {
                std::println(stderr,
                             "[shader_cooker] manifest variant {} resolves resource {} out of range",
                             variant.Index,
                             local);
                return CookError::ManifestVariantResourceResolveOutOfRange;
            }

            const ManifestBinding& read = view.Bindings()[resources[local]];

            if (view.String(read.NameString) != expected.Resource->Name ||
                view.String(read.ScopeString) != expected.Resource->ScopeName ||
                !RecordMatchesBinding(read, *expected.Resource) ||
                !RecordMatchesFootprint(footprints[local], *expected.Footprint) ||
                !ManifestUniformMembersMatch(view, read, *expected.Resource))
            {
                std::println(stderr,
                             "[shader_cooker] manifest binding '{}' of variant {} does not match the cook",
                             expected.Resource->Name,
                             variant.Index);
                return CookError::ManifestVariantResourceBindingMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestVertexInputs(std::span<const ManifestVertexInput> read_inputs,
                                               const ReflectedRasterState& expected_raster,
                                               const ShaderManifestView& view)
    {
        for (size_t inputIndex = 0u; inputIndex < read_inputs.size(); ++inputIndex)
        {
            const ReflectedVertexInput& expectedInput = expected_raster.VertexInputs[inputIndex];
            const ManifestVertexInput& readInput = read_inputs[inputIndex];

            if (view.String(readInput.SemanticNameString) != expectedInput.SemanticName ||
                readInput.SemanticIndex != expectedInput.Data.SemanticIndex ||
                readInput.Location != expectedInput.Data.Location ||
                readInput.ScalarType != static_cast<uint32_t>(expectedInput.Data.ScalarType) ||
                readInput.ComponentCount != expectedInput.Data.ComponentCount)
            {
                std::println(stderr,
                             "[shader_cooker] manifest vertex input '{}' does not match the cook",
                             expectedInput.SemanticName);
                return CookError::ManifestVertexInputMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestColorTargets(std::span<const ManifestColorTarget> read_targets,
                                               const ReflectedRasterState& expected_raster)
    {
        for (size_t targetIndex = 0u; targetIndex < read_targets.size(); ++targetIndex)
        {
            const ReflectedColorTarget& expectedTarget = expected_raster.ColorTargets[targetIndex];
            const ManifestColorTarget& readTarget = read_targets[targetIndex];

            if (readTarget.Location != expectedTarget.Location ||
                readTarget.ScalarType != static_cast<uint32_t>(expectedTarget.ScalarType) ||
                readTarget.ComponentCount != expectedTarget.ComponentCount)
            {
                std::println(stderr,
                             "[shader_cooker] manifest color target {} does not match the cook",
                             expectedTarget.Location);
                return CookError::ManifestColorTargetMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestRaster(const CookedModule& module,
                                         const ShaderManifestView& view,
                                         const LibraryVariant& variant,
                                         size_t entry_point_index)
    {
        const uint32_t rasterIndex = variant.RasterIndices[entry_point_index];
        const ReflectedRasterState& expectedRaster = module.RasterStates[rasterIndex];

        const std::span<const ManifestVertexInput> readInputs = view.VertexInputs(rasterIndex);
        const std::span<const ManifestColorTarget> readTargets = view.ColorTargets(rasterIndex);

        if (readInputs.size() != expectedRaster.VertexInputs.size() ||
            readTargets.size() != expectedRaster.ColorTargets.size() ||
            view.WritesFragDepth(rasterIndex) != expectedRaster.WritesFragDepth)
        {
            std::println(stderr,
                         "[shader_cooker] manifest raster state {} does not match the cook for {}",
                         rasterIndex,
                         module.EntryPoints[entry_point_index].Name);
            return CookError::ManifestRasterStateMismatch;
        }

        const CookError inputsError = CheckManifestVertexInputs(readInputs, expectedRaster, view);
        if (inputsError != CookError::Success)
        {
            return inputsError;
        }

        return CheckManifestColorTargets(readTargets, expectedRaster);
    }

    /** Every fact the manifest states about one entry point of one variant. */
    CookError CheckManifestSlot(const CookedModule& module,
                                       const ShaderManifestView& view,
                                       const ManifestShaderSourceProvider& provider,
                                       const LibraryVariant& variant,
                                       size_t entry_point_index)
    {
        // The provider takes the EntryPointId value, which counts from one.
        const auto entryPointId = static_cast<uint16_t>(entry_point_index + 1u);

        const CookError sourceError = CheckManifestSource(module, provider, variant, entry_point_index, entryPointId);
        if (sourceError != CookError::Success)
        {
            return sourceError;
        }

        const CookError workgroupError = CheckManifestWorkgroup(module, provider, variant, entry_point_index, entryPointId);
        if (workgroupError != CookError::Success)
        {
            return workgroupError;
        }

        const CookError layoutError = CheckManifestLayout(module, view, variant, entry_point_index);
        if (layoutError != CookError::Success)
        {
            return layoutError;
        }

        return CheckManifestRaster(module, view, variant, entry_point_index);
    }

    std::string MakeManifestFileName(std::string_view module_name)
    {
        return std::format("{}.ldshaders", module_name);
    }

    /** Each builder appends to the string table in call order, so the order of the calls in
     * EmitShaderManifest decides every string index in the file. Do not reorder them. */
    std::vector<ManifestEntryPoint> BuildEntryPointRecords(const CookedModule& module,
                                                           StringTableBuilder& strings)
    {
        std::vector<ManifestEntryPoint> records;
        records.reserve(module.EntryPoints.size());

        for (const LibraryEntryPoint& entryPoint : module.EntryPoints)
        {
            records.push_back(ManifestEntryPoint{ .NameString = strings.Add(entryPoint.Name),
                                                  .Stage = static_cast<uint32_t>(entryPoint.Stage) });
        }

        return records;
    }

    /** The four cooked tables, flattened into runs over index and payload tables. */
    struct LayoutTables
    {
        std::vector<ManifestUniformMember> UniformMembers;
        std::vector<ManifestBinding> Bindings;
        std::vector<uint32_t> ResourceIndices;
        std::vector<ManifestRun> ResourceLists;
        std::vector<ManifestFootprint> Footprints;
        std::vector<ManifestRun> FootprintLists;
        std::vector<uint32_t> VisibilityIndices;
        std::vector<ManifestRun> VisibilityLists;
    };

    template<typename PayloadType, typename RecordType, typename MakeRecordFn>
    void AppendRuns(const std::vector<std::vector<PayloadType>>& lists,
                    std::vector<RecordType>& out_payloads,
                    std::vector<ManifestRun>& out_runs,
                    MakeRecordFn make_record)
    {
        // using input sizes as part of reserve in case we merge lists or runs
        // together in the future: otherwise we'd underallocate (if at all) often
        size_t totalSize = 0u;
        for (const std::vector<PayloadType>& list : lists)
        {
            totalSize += list.size();
        }

        uint32_t currentOffset = static_cast<uint32_t>(out_payloads.size());
        for (const std::vector<PayloadType>& list : lists)
        {
            const uint32_t currentListSize = static_cast<uint32_t>(list.size());
            out_runs.emplace_back(currentOffset, currentListSize);
            currentOffset += currentListSize;
        }

        // trying something a little new, esp. since we have that input lambda
        auto flattenedRecords = lists | std::views::join | std::views::transform(make_record);
        out_payloads.insert(out_payloads.end(),
                            std::make_move_iterator(flattenedRecords.begin()),
                            std::make_move_iterator(flattenedRecords.end()));
    }

    uint32_t PassThroughIndex(uint32_t index) noexcept
    {
        return index;
    }

    LayoutTables BuildLayoutTables(const CookedModule& module, StringTableBuilder& strings)
    {
        LayoutTables tables;
        tables.Bindings.reserve(module.Resources.size());

        for (const ReflectedBinding& resource : module.Resources)
        {
            tables.Bindings.push_back(MakeBindingRecord(resource, strings, tables.UniformMembers));
        }
        // todo-ship: Make this into a move, and make footprint record stop using a variant. Change values
        // to be stored not in a variant, but in a dedicated footprint record structure that we can just copy
        // already. Use sentinel values to derive type
        AppendRuns(module.ResourceLists, tables.ResourceIndices, tables.ResourceLists, &PassThroughIndex);
        AppendRuns(module.FootprintLists, tables.Footprints, tables.FootprintLists, &MakeFootprintRecord);
        AppendRuns(
            module.VisibilityLists, tables.VisibilityIndices, tables.VisibilityLists, &PassThroughIndex);

        return tables;
    }

    struct RasterTables
    {
        std::vector<ManifestVertexInput> VertexInputs;
        std::vector<ManifestColorTarget> ColorTargets;
        std::vector<ManifestRaster> Rasters;
    };

    ManifestVertexInput MakeVertexInputRecord(const ReflectedVertexInput& input, StringTableBuilder& strings)
    {
        ManifestVertexInput record;
        record.SemanticNameString = strings.Add(input.SemanticName);
        record.SemanticIndex = input.Data.SemanticIndex;
        record.Location = input.Data.Location;
        record.ScalarType = static_cast<uint32_t>(input.Data.ScalarType);
        record.ComponentCount = input.Data.ComponentCount;
        return record;
    }

    ManifestColorTarget MakeColorTargetRecord(const ReflectedColorTarget& target) noexcept
    {
        ManifestColorTarget record;
        record.Location = target.Location;
        record.ScalarType = static_cast<uint32_t>(target.ScalarType);
        record.ComponentCount = target.ComponentCount;
        return record;
    }

    RasterTables BuildRasterTables(const CookedModule& module, StringTableBuilder& strings)
    {
        RasterTables tables;
        tables.Rasters.reserve(module.RasterStates.size());

        for (const ReflectedRasterState& raster : module.RasterStates)
        {
            ManifestRaster record;
            record.FirstVertexInput = static_cast<uint32_t>(tables.VertexInputs.size());
            record.VertexInputCount = static_cast<uint32_t>(raster.VertexInputs.size());
            record.FirstColorTarget = static_cast<uint32_t>(tables.ColorTargets.size());
            record.ColorTargetCount = static_cast<uint32_t>(raster.ColorTargets.size());
            record.WritesFragDepth = raster.WritesFragDepth ? 1u : 0u;
            tables.Rasters.push_back(record);

            for (const ReflectedVertexInput& input : raster.VertexInputs)
            {
                tables.VertexInputs.push_back(MakeVertexInputRecord(input, strings));
            }

            for (const ReflectedColorTarget& target : raster.ColorTargets)
            {
                tables.ColorTargets.push_back(MakeColorTargetRecord(target));
            }
        }

        return tables;
    }

    struct VariantTables
    {
        std::vector<ManifestSlot> Slots;
        std::vector<ManifestVariant> Variants;
    };

    ManifestSlot MakeSlotRecord(const LibraryVariant& variant, size_t entry_point_index) noexcept
    {
        ManifestSlot slot;
        slot.SourceIndex = variant.SourceIndices[entry_point_index];
        slot.VisibilityIndex = variant.VisibilityIndices[entry_point_index];
        slot.WorkgroupX = variant.Workgroups[entry_point_index].X;
        slot.WorkgroupY = variant.Workgroups[entry_point_index].Y;
        slot.WorkgroupZ = variant.Workgroups[entry_point_index].Z;
        slot.RasterIndex = variant.RasterIndices[entry_point_index];
        return slot;
    }

    VariantTables BuildVariantTables(const CookedModule& module, StringTableBuilder& strings)
    {
        VariantTables tables;
        tables.Variants.reserve(module.Variants.size());
        // most modules will have 3-4 entrypoints: reserve for that
        tables.Slots.reserve(module.Variants.size() * 4u);

        for (const LibraryVariant& variant : module.Variants)
        {
            ManifestVariant record;
            record.Index = variant.Index;
            record.FirstSlot = static_cast<uint32_t>(tables.Slots.size());
            record.SlotCount = static_cast<uint32_t>(module.EntryPoints.size());
            record.SuffixString = strings.Add(variant.Suffix);
            record.ResourceListIndex = variant.ResourceListIndex;
            record.FootprintListIndex = variant.FootprintListIndex;
            tables.Variants.push_back(record);

            for (size_t i = 0u; i < module.EntryPoints.size(); ++i)
            {
                tables.Slots.push_back(MakeSlotRecord(variant, i));
            }
        }

        return tables;
    }

    /** Maps a dense variant index to a row of the variant table. A hole keeps k_ShaderManifestNoIndex. */
    std::vector<uint32_t> BuildVariantIndexTable(const CookedModule& module)
    {
        std::vector<uint32_t> records(module.SpaceSize, k_ShaderManifestNoIndex);

        for (size_t i = 0u; i < module.Variants.size(); ++i)
        {
            const uint32_t denseIndex = module.Variants[i].Index;
            if (denseIndex < records.size())
            {
                records[denseIndex] = static_cast<uint32_t>(i);
            }
        }

        return records;
    }

    struct AxisTables
    {
        std::vector<ManifestAxis> Axes;
        std::vector<int64_t> Values;
    };

    AxisTables BuildAxisTables(const CookedModule& module, StringTableBuilder& strings)
    {
        AxisTables tables;
        if (module.Space == nullptr)
        {
            return tables;
        }

        tables.Axes.reserve(module.Space->AxisCount());

        for (const PermutationAxis& axis : module.Space->Axes())
        {
            ManifestAxis record;
            record.NameString = strings.Add(axis.Name);
            record.FirstValue = static_cast<uint32_t>(tables.Values.size());
            record.ValueCount = static_cast<uint32_t>(axis.NumValues());
            tables.Axes.push_back(record);
            for (const PermutationValue& value : axis.GetValues())
            {
                tables.Values.emplace_back(PermutationValueToInt64(value));
            }
        }

        return tables;
    }

    struct SourceTables
    {
        std::string Blob;
        std::vector<ManifestSourceRef> Refs;
    };

    SourceTables BuildSourceTables(const CookedModule& module)
    {
        SourceTables tables;
        tables.Refs.reserve(module.Sources.size());

        for (const std::string& source : module.Sources)
        {
            tables.Refs.push_back(ManifestSourceRef{ .Offset = static_cast<uint32_t>(tables.Blob.size()),
                                                     .Length = static_cast<uint32_t>(source.size()) });
            tables.Blob.append(source);
        }

        return tables;
    }

} // namespace

std::string EmitShaderManifest(const CookedModule& module)
{
    StringTableBuilder strings;
    /** DO NOT REORDER THESE. The order of these calls currently decides the order of the strings
     * in the resulting manifest file. We encode the offsets and lengths of these strings based
     * on the header of ShaderManifest, so changing the order changes data locations and result layout!
     * This is, of course, a fragile design, but doing more is a bit out of scope for this tool rn.
     * And to be clear, this ordering *here* exactly is for the indices. Not the actual stored bytes.
     */
    const uint32_t moduleNameString = strings.Add(module.Name);
    const std::vector<ManifestEntryPoint> entryPointRecords = BuildEntryPointRecords(module, strings);
    const LayoutTables layouts = BuildLayoutTables(module, strings);
    const RasterTables rasters = BuildRasterTables(module, strings);
    const VariantTables variants = BuildVariantTables(module, strings);
    const std::vector<uint32_t> variantIndexRecords = BuildVariantIndexTable(module);
    const AxisTables axes = BuildAxisTables(module, strings);
    const SourceTables sources = BuildSourceTables(module);

    ShaderManifestHeader header;
    header.Magic = k_ShaderManifestMagic;
    header.Version = k_ShaderManifestVersion;
    header.ModuleNameString = moduleNameString;

    std::string bytes;

    // fully reserve bytes: we know all of our sizes now!
    // some of these use decltype, as I have a hunch we might change them in the future (namely, integral
    // types)
    const size_t totalSize =
        sizeof(ShaderManifestHeader) + strings.Blob().size() + sources.Blob.size() +
        (entryPointRecords.size() * sizeof(ManifestEntryPoint)) +
        (layouts.Bindings.size() * sizeof(ManifestBinding)) +
        (layouts.ResourceIndices.size() * sizeof(uint32_t)) +
        (layouts.ResourceLists.size() * sizeof(ManifestRun)) +
        (layouts.Footprints.size() * sizeof(ManifestFootprint)) +
        (layouts.FootprintLists.size() * sizeof(ManifestRun)) +
        (layouts.VisibilityIndices.size() * sizeof(uint32_t)) +
        (layouts.VisibilityLists.size() * sizeof(ManifestRun)) +
        (rasters.VertexInputs.size() * sizeof(ManifestVertexInput)) +
        (rasters.ColorTargets.size() * sizeof(ManifestColorTarget)) +
        (rasters.Rasters.size() * sizeof(ManifestRaster)) + (variants.Slots.size() * sizeof(ManifestSlot)) +
        (variants.Variants.size() * sizeof(ManifestVariant)) +
        (variantIndexRecords.size() * sizeof(decltype(variantIndexRecords)::value_type)) +
        (axes.Axes.size() * sizeof(ManifestAxis)) +
        (axes.Values.size() * sizeof(decltype(axes.Values)::value_type));

    bytes.reserve(totalSize);
    bytes.resize(sizeof(ShaderManifestHeader), '\0');

    header.StringTableOffset = AppendTable(bytes, strings.References());
    header.StringCount = static_cast<uint32_t>(strings.References().size());

    AlignTo8(bytes);
    header.StringBlobOffset = static_cast<uint32_t>(bytes.size());
    header.StringBlobSize = static_cast<uint32_t>(strings.Blob().size());
    bytes.append(strings.Blob());

    header.SourceTableOffset = AppendTable(bytes, sources.Refs);
    header.SourceCount = static_cast<uint32_t>(sources.Refs.size());

    AlignTo8(bytes);
    header.SourceBlobOffset = static_cast<uint32_t>(bytes.size());
    header.SourceBlobSize = static_cast<uint32_t>(sources.Blob.size());
    bytes.append(sources.Blob);

    // DO NOT REORDER THESE. Above, the ordering sets how the indices for the string table are assigned.
    // This ordering controls the actual order of the data the indices refer to. Changing either one
    // will break the other. If you are going to change something, BOTH must be changed together

    header.BindingTableOffset = AppendTable(bytes, layouts.Bindings);
    header.BindingCount = static_cast<uint32_t>(layouts.Bindings.size());
    header.ResourceIndexTableOffset = AppendTable(bytes, layouts.ResourceIndices);
    header.ResourceIndexCount = static_cast<uint32_t>(layouts.ResourceIndices.size());
    header.ResourceListTableOffset = AppendTable(bytes, layouts.ResourceLists);
    header.ResourceListCount = static_cast<uint32_t>(layouts.ResourceLists.size());
    header.FootprintTableOffset = AppendTable(bytes, layouts.Footprints);
    header.FootprintCount = static_cast<uint32_t>(layouts.Footprints.size());
    header.FootprintListTableOffset = AppendTable(bytes, layouts.FootprintLists);
    header.FootprintListCount = static_cast<uint32_t>(layouts.FootprintLists.size());
    header.VisibilityIndexTableOffset = AppendTable(bytes, layouts.VisibilityIndices);
    header.VisibilityIndexCount = static_cast<uint32_t>(layouts.VisibilityIndices.size());
    header.VisibilityListTableOffset = AppendTable(bytes, layouts.VisibilityLists);
    header.VisibilityListCount = static_cast<uint32_t>(layouts.VisibilityLists.size());
    header.EntryPointTableOffset = AppendTable(bytes, entryPointRecords);
    header.EntryPointCount = static_cast<uint32_t>(entryPointRecords.size());
    header.SlotTableOffset = AppendTable(bytes, variants.Slots);
    header.SlotCount = static_cast<uint32_t>(variants.Slots.size());
    header.VariantTableOffset = AppendTable(bytes, variants.Variants);
    header.VariantCount = static_cast<uint32_t>(variants.Variants.size());
    header.VariantIndexTableOffset = AppendTable(bytes, variantIndexRecords);
    header.VariantIndexCount = static_cast<uint32_t>(variantIndexRecords.size());
    header.AxisTableOffset = AppendTable(bytes, axes.Axes);
    header.AxisCount = static_cast<uint32_t>(axes.Axes.size());
    header.AxisValueTableOffset = AppendTable(bytes, axes.Values);
    header.AxisValueCount = static_cast<uint32_t>(axes.Values.size());
    header.RasterTableOffset = AppendTable(bytes, rasters.Rasters);
    header.RasterCount = static_cast<uint32_t>(rasters.Rasters.size());
    header.VertexInputTableOffset = AppendTable(bytes, rasters.VertexInputs);
    header.VertexInputCount = static_cast<uint32_t>(rasters.VertexInputs.size());
    header.ColorTargetTableOffset = AppendTable(bytes, rasters.ColorTargets);
    header.ColorTargetCount = static_cast<uint32_t>(rasters.ColorTargets.size());
    header.UniformMemberTableOffset = AppendTable(bytes, layouts.UniformMembers);
    header.UniformMemberCount = static_cast<uint32_t>(layouts.UniformMembers.size());

    AlignTo8(bytes);
    header.FileSize = static_cast<uint32_t>(bytes.size());

    std::memcpy(bytes.data(), &header, sizeof(ShaderManifestHeader));

    return bytes;
}

CookError VerifyManifestRoundTrip(const CookedModule& module, const std::string& manifest_bytes)
{
    const std::span<const std::byte> raw{ reinterpret_cast<const std::byte*>(manifest_bytes.data()),
                                          manifest_bytes.size() };

    const CookResult<ShaderManifestView> opened = OpenManifestForCheck(module, raw);
    if (!opened)
    {
        return opened.error();
    }

    const ShaderManifestView& view = opened.value();
    const ManifestShaderSourceProvider provider{ view, 0u };
    uint32_t checked = 0u;

    for (const LibraryVariant& variant : module.Variants)
    {
        for (size_t i = 0u; i < module.EntryPoints.size(); ++i)
        {
            const CookError slotError = CheckManifestSlot(module, view, provider, variant, i);
            if (slotError != CookError::Success)
            {
                return slotError;
            }

            ++checked;
        }
    }

    std::println(stderr,
                 "[shader_cooker] module {} manifest round trip verified: {} entrypoint variants read back "
                 "identical ({} KiB)",
                 module.Name,
                 checked,
                 manifest_bytes.size() / 1024u);

    return CookError::Success;
}

} // namespace lodestone
