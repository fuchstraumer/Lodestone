#include "emit/ShaderManifestEmitter.hpp"
#include "VariantKey.hpp"
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "permute/PermutationValue.hpp"
#include "TransparentHash.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <functional>
#include <numeric>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
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
     * the blob holds each binding name a single time however many layouts name it. One builder serves the
     * whole cook, so a name that several modules or profiles share is stored once. */
    // todo-ship: There are better ways to do this, and we should absolutely explore them since strings
    // will rapidly become a huge cost as variant count increases
    class StringTableBuilder
    {
    public:
        StringTableBuilder();
        uint32_t Add(std::string_view text);
        [[nodiscard]] std::string_view Text(uint32_t string_index) const noexcept;
        [[nodiscard]] const std::vector<manifest::StringRef>& References() const noexcept;
        [[nodiscard]] const std::string& Blob() const noexcept;

    private:
        std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> lookup;
        std::vector<manifest::StringRef> references;
        std::string blob;
    };

    /** One axis in the vocabulary of the manifest. A named value (a type or an enum case) is already a
     * string index here, so two modules that name the same case hold the same value. */
    struct EncodedAxis
    {
        uint32_t NameString{ 0u };
        AxisKind Kind{ AxisKind::None };
        AxisValueDomain Domain{ AxisValueDomain::None };
        EarliestBindingTime BindingTime{ EarliestBindingTime::None };
        std::vector<AxisValueType> Values;

        bool operator==(const EncodedAxis& other) const = default;
    };

    /** What a module states, independent of the profile it was cooked for. */
    struct ModuleShape
    {
        std::vector<manifest::EntryPoint> EntryPoints;
        std::vector<EncodedAxis> Axes;
    };

    /** The four cooked tables, flattened into runs over index and payload tables. */
    struct LayoutTables
    {
        std::vector<manifest::UniformMember> UniformMembers;
        std::vector<manifest::Binding> Bindings;
        std::vector<uint32_t> ResourceIndices;
        std::vector<manifest::Run> ResourceLists;
        std::vector<manifest::Footprint> Footprints;
        std::vector<manifest::Run> FootprintLists;
        std::vector<uint32_t> VisibilityIndices;
        std::vector<manifest::Run> VisibilityLists;
    };

    struct RasterTables
    {
        std::vector<manifest::VertexInput> VertexInputs;
        std::vector<manifest::ColorTarget> ColorTargets;
        std::vector<manifest::RasterState> Rasters;
    };

    /** The variant keys, the variant records, and the axis masks are parallel and sorted by key. The slots
     * are a grid with one row for each variant. */
    struct VariantTables
    {
        std::vector<VariantKey> Keys;
        std::vector<manifest::Variant> Variants;
        std::vector<uint64_t> AxisMasks;
        std::vector<manifest::EntryPointInstance> Slots;
    };

    struct SourceTables
    {
        std::string Blob;
        std::vector<manifest::SourceRef> Refs;
    };

    constexpr uint32_t k_MaxModuleAxisValues = 32u;
    constexpr size_t k_AxisMaskWordBits = 64u;

    /** @brief Writes the actual bytes to the given `out` string. */
    void AppendBytes(std::string& out, const void* data, size_t size);

    /** Pads to the next 8-byte boundary. Binding records and axis values hold 64-bit fields, and the
     * reader maps them in place, so every section must start aligned. */
    void AlignTo8(std::string& out);

    /** @brief Appends a table of records to the output string, aligned to 8 bytes. Returns the offset
     *  of the first record in the output string where the new records are now located */
    template<typename RecordType>
    uint64_t AppendTable(std::string& out, const std::vector<RecordType>& records)
    {
        AlignTo8(out);
        const uint64_t offset = out.size();
        if (!records.empty())
        {
            AppendBytes(out, records.data(), records.size() * sizeof(RecordType));
        }

        return offset;
    }

    /** The same append, for a table inside an extent. Every extent offset is relative to the extent start,
     * and an extent is its own buffer, so the returned offset is already relative. */
    template<typename RecordType>
    uint32_t AppendExtentTable(std::string& out, const std::vector<RecordType>& records)
    {
        return static_cast<uint32_t>(AppendTable(out, records));
    }

    template<typename PayloadType, typename RecordType, typename MakeRecordFn>
    void AppendRuns(const std::vector<std::vector<PayloadType>>& lists,
                    std::vector<RecordType>& out_payloads,
                    std::vector<manifest::Run>& out_runs,
                    MakeRecordFn make_record)
    {
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

    uint32_t PassThroughIndex(uint32_t index) noexcept;
    manifest::Binding MakeBindingRecord(const ReflectedBinding& binding,
                                        StringTableBuilder& strings,
                                        std::vector<manifest::UniformMember>& member_records);
    manifest::Footprint MakeFootprintRecord(const ResourceFootprint& footprint) noexcept;
    manifest::EntryPointInstance MakeSlotRecord(const LibraryVariant& variant, size_t entry_point_index) noexcept;
    manifest::VertexInput MakeVertexInputRecord(const ReflectedVertexInput& input, StringTableBuilder& strings);
    manifest::ColorTarget MakeColorTargetRecord(const ReflectedColorTarget& target) noexcept;

    /** The module's axes and entry points, as the manifest states them. Every profile of one module must
     * give the same shape, because the key of a variant packs against the module's axes. */
    ModuleShape EncodeModuleShape(const CookedModule& module, StringTableBuilder& strings);
    bool SameModuleShape(const ModuleShape& lhs, const ModuleShape& rhs) noexcept;
    /** Adds a module's axis to the root axis table. A root axis with the same name, kind, domain, and binding
     * time takes it when the module's values fit the root values in order, so digit D of the module still
     * selects the module's D-th value. Otherwise the axis gets a root record of its own. */
    CookResult<manifest::ModuleAxis> MergeIntoRootAxes(std::vector<EncodedAxis>& root_axes,
                                                       const EncodedAxis& module_axis,
                                                       StringTableBuilder& strings);
    bool TryMergeValues(const EncodedAxis& root_axis,
                        const EncodedAxis& module_axis,
                        std::vector<AxisValueType>& appended_values,
                        uint32_t& live_values_mask);
    std::vector<manifest::Axis> BuildRootAxisRecords(const std::vector<EncodedAxis>& root_axes,
                                                     std::vector<AxisValueType>& out_values);

    LayoutTables BuildLayoutTables(const CookedModule& module, StringTableBuilder& strings);
    RasterTables BuildRasterTables(const CookedModule& module, StringTableBuilder& strings);
    CookResult<VariantTables> BuildVariantTables(const CookedModule& module, StringTableBuilder& strings);
    SourceTables BuildSourceTables(const CookedModule& module);
    /** One environment extent: an `EnvironmentHeader`, then every table of one (profile, module) pair. */
    CookResult<std::string> BuildExtent(const CookedModule& module, StringTableBuilder& strings);

    /** The first profile that cooked the module, or null when no profile did. */
    const CookedModule* FirstCookedEnvironment(const CookedLibrary& library, size_t module_index) noexcept;

    CookError CheckBundleDirectory(const CookedLibrary& library, const manifest::BundleView& bundle);
    CookError CheckEnvironment(const CookedModule& module,
                               const manifest::EnvironmentView& view,
                               uint32_t& checked_count);
    CookError CheckVariantKeyDecode(const manifest::EnvironmentView& view,
                                    const LibraryVariant& variant,
                                    VariantKey key);
    CookError CheckVariantAxisMask(const CookedModule& module,
                                   const manifest::EnvironmentView& view,
                                   const LibraryVariant& variant,
                                   uint32_t variant_index);
    CookError CheckManifestSource(const CookedModule& module,
                                  const manifest::ShaderSourceProvider& provider,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index);
    CookError CheckManifestWorkgroup(const CookedModule& module,
                                     const manifest::ShaderSourceProvider& provider,
                                     const LibraryVariant& variant,
                                     size_t entry_point_index);
    bool RecordMatchesFootprint(const manifest::Footprint& record, const ResourceFootprint& footprint) noexcept;
    bool RecordMatchesBinding(const manifest::Binding& record, const ReflectedBinding& binding) noexcept;
    bool ManifestUniformMembersMatch(const manifest::EnvironmentView& view,
                                     const manifest::Binding& read,
                                     const ReflectedBinding& expected);
    CookError CheckManifestLayout(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  uint32_t variant_index,
                                  size_t entry_point_index);
    CookError CheckManifestVertexInputs(std::span<const manifest::VertexInput> read_inputs,
                                        const ReflectedRasterState& expected_raster,
                                        const manifest::EnvironmentView& view);
    CookError CheckManifestColorTargets(std::span<const manifest::ColorTarget> read_targets,
                                        const ReflectedRasterState& expected_raster);
    CookError CheckManifestRaster(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index);
    /** Every fact the manifest states about one entry point of one variant. */
    CookError CheckManifestSlot(const CookedModule& module,
                                const manifest::EnvironmentView& view,
                                const manifest::ShaderSourceProvider& provider,
                                const LibraryVariant& variant,
                                uint32_t variant_index,
                                size_t entry_point_index);

} // namespace

CookResult<std::string> EmitShaderManifest(const CookedLibrary& library)
{
    const size_t moduleCount = library.ModuleNames.size();
    const size_t profileCount = library.Profiles.size();

    // One string table serves the whole cook. The call order below decides every string index, and the
    // cook must be deterministic, so the order is fixed: module names, module shapes, profiles, extents.
    StringTableBuilder strings;

    std::vector<manifest::ModuleRootHeader> moduleHeaders(moduleCount);
    std::vector<ModuleShape> moduleShapes(moduleCount);
    for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
    {
        moduleHeaders[moduleIndex].ModuleNameString = strings.Add(library.ModuleNames[moduleIndex]);
    }

    for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
    {
        const CookedModule* first = FirstCookedEnvironment(library, moduleIndex);
        if (first == nullptr)
        {
            continue;
        }

        moduleShapes[moduleIndex] = EncodeModuleShape(*first, strings);
        for (size_t profileIndex = 0u; profileIndex < profileCount; ++profileIndex)
        {
            const std::optional<CookedModule>& environment =
                library.Environments[(profileIndex * moduleCount) + moduleIndex];
            if (environment.has_value() &&
                !SameModuleShape(moduleShapes[moduleIndex], EncodeModuleShape(*environment, strings)))
            {
                std::println(stderr,
                             "[shader_cooker] module {} states different entry points or axes for target '{}'",
                             library.ModuleNames[moduleIndex],
                             library.Profiles[profileIndex].TargetName);
                return std::unexpected(CookError::ManifestModuleShapeMismatch);
            }
        }
    }

    std::vector<EncodedAxis> rootAxes;
    std::vector<std::vector<manifest::ModuleAxis>> moduleAxes(moduleCount);
    for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
    {
        for (const EncodedAxis& moduleAxis : moduleShapes[moduleIndex].Axes)
        {
            const CookResult<manifest::ModuleAxis> merged = MergeIntoRootAxes(rootAxes, moduleAxis, strings);
            if (!merged)
            {
                return std::unexpected(merged.error());
            }
            moduleAxes[moduleIndex].push_back(*merged);
        }
    }

    std::vector<manifest::Profile> profiles;
    profiles.reserve(profileCount);
    for (const CookedProfile& profile : library.Profiles)
    {
        profiles.push_back(manifest::Profile{ .TargetNameString = strings.Add(profile.TargetName),
                                              .AccessModel = profile.AccessModel });
    }

    // Build every extent before the header region, because an extent adds binding names and suffixes to
    // the string table, and the header region holds that table.
    std::vector<std::string> extents(library.Environments.size());
    for (size_t environmentIndex = 0u; environmentIndex < library.Environments.size(); ++environmentIndex)
    {
        const std::optional<CookedModule>& environment = library.Environments[environmentIndex];
        if (!environment.has_value())
        {
            continue;
        }

        CookResult<std::string> extent = BuildExtent(*environment, strings);
        if (!extent)
        {
            return std::unexpected(extent.error());
        }
        extents[environmentIndex] = std::move(*extent);
    }

    std::vector<AxisValueType> rootAxisValues;
    const std::vector<manifest::Axis> rootAxisRecords = BuildRootAxisRecords(rootAxes, rootAxisValues);
    std::vector<manifest::EnvironmentDirectoryEntry> directory(library.Environments.size());

    manifest::Header header;
    header.Magic = manifest::k_ShaderManifestMagic;
    header.Version = manifest::k_ShaderManifestVersion;
    header.ModuleCount = moduleCount;
    header.ProfileCount = profileCount;

    // The module headers sit right after the header, so the reader finds them with no offset.
    std::string bytes;
    bytes.resize(sizeof(manifest::Header) + (moduleCount * sizeof(manifest::ModuleRootHeader)), '\0');

    header.ProfileTableOffset = AppendTable(bytes, profiles);
    header.EnvironmentDirectoryOffset = AppendTable(bytes, directory);
    header.AxisTableOffset = AppendTable(bytes, rootAxisRecords);
    header.AxisCount = rootAxisRecords.size();
    header.AxisValueTableOffset = AppendTable(bytes, rootAxisValues);
    header.AxisValueCount = rootAxisValues.size();

    for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
    {
        manifest::ModuleRootHeader& moduleHeader = moduleHeaders[moduleIndex];
        moduleHeader.EntryPointTableOffset = AppendTable(bytes, moduleShapes[moduleIndex].EntryPoints);
        moduleHeader.EntryPointCount = moduleShapes[moduleIndex].EntryPoints.size();
        moduleHeader.ModuleAxisTableOffset = AppendTable(bytes, moduleAxes[moduleIndex]);
        moduleHeader.ModuleAxisCount = moduleAxes[moduleIndex].size();
    }

    header.StringTableOffset = AppendTable(bytes, strings.References());
    header.StringCount = strings.References().size();
    AlignTo8(bytes);
    header.StringBlobOffset = bytes.size();
    header.StringBlobSize = strings.Blob().size();
    bytes.append(strings.Blob());

    AlignTo8(bytes);
    header.HeaderSize = bytes.size();

    // Profile first, then module: the grid order, and the order a renderer reads one profile in.
    for (size_t environmentIndex = 0u; environmentIndex < extents.size(); ++environmentIndex)
    {
        if (extents[environmentIndex].empty())
        {
            continue;
        }

        AlignTo8(bytes);
        directory[environmentIndex] = manifest::EnvironmentDirectoryEntry{
            .ExtentOffset = bytes.size(), .ExtentSize = extents[environmentIndex].size()
        };
        bytes.append(extents[environmentIndex]);
    }

    AlignTo8(bytes);
    header.FileSize = bytes.size();

    std::memcpy(bytes.data(), &header, sizeof(manifest::Header));
    if (!moduleHeaders.empty())
    {
        std::memcpy(bytes.data() + sizeof(manifest::Header),
                    moduleHeaders.data(),
                    moduleHeaders.size() * sizeof(manifest::ModuleRootHeader));
    }
    if (!directory.empty())
    {
        std::memcpy(bytes.data() + header.EnvironmentDirectoryOffset,
                    directory.data(),
                    directory.size() * sizeof(manifest::EnvironmentDirectoryEntry));
    }

    return bytes;
}

CookError VerifyManifestRoundTrip(const CookedLibrary& library, const std::string& manifest_bytes)
{
    const std::span<const char> rawChars{ manifest_bytes.data(), manifest_bytes.size() };
    const std::span<const std::byte> raw = std::as_bytes(rawChars);

    const manifest::ManifestResult<manifest::BundleView> opened = manifest::BundleView::Open(raw);
    if (!opened)
    {
        std::println(stderr,
                     "[shader_cooker] manifest bundle does not open: {}",
                     manifest::DescribeShaderManifestError(opened.error()));
        return CookError::LibraryRoundTripFailed;
    }

    const manifest::BundleView& bundle = *opened;
    const CookError directoryError = CheckBundleDirectory(library, bundle);
    if (directoryError != CookError::Success)
    {
        return directoryError;
    }

    const size_t moduleCount = library.ModuleNames.size();
    uint32_t checked = 0u;
    for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
    {
        for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
        {
            const std::optional<CookedModule>& environment =
                library.Environments[(profileIndex * moduleCount) + moduleIndex];
            const manifest::ManifestResult<manifest::EnvironmentView> view =
                bundle.OpenEnvironment(static_cast<uint32_t>(profileIndex), static_cast<uint32_t>(moduleIndex));

            if (!environment.has_value())
            {
                // an environment the cook skipped must read back as absent, and not as some other extent
                if (view.has_value() || view.error().Code != manifest::ErrorCode::EnvironmentNotCooked)
                {
                    std::println(stderr,
                                 "[shader_cooker] manifest holds module {} for target '{}', but the cook did not",
                                 library.ModuleNames[moduleIndex],
                                 library.Profiles[profileIndex].TargetName);
                    return CookError::LibraryRoundTripFailed;
                }
                continue;
            }

            if (!view)
            {
                std::println(stderr,
                             "[shader_cooker] module {} for target '{}' does not open: {}",
                             library.ModuleNames[moduleIndex],
                             library.Profiles[profileIndex].TargetName,
                             manifest::DescribeShaderManifestError(view.error()));
                return CookError::LibraryRoundTripFailed;
            }

            const CookError environmentError = CheckEnvironment(*environment, *view, checked);
            if (environmentError != CookError::Success)
            {
                return environmentError;
            }
        }
    }

    std::println(stderr,
                 "[shader_cooker] manifest round trip verified: {} modules x {} profiles, {} entrypoint variants "
                 "read back identical ({} KiB)",
                 moduleCount,
                 library.Profiles.size(),
                 checked,
                 manifest_bytes.size() / 1024u);

    return CookError::Success;
}

namespace
{

    StringTableBuilder::StringTableBuilder()
    {
        lookup.reserve(1024);
        references.reserve(1024);
        blob.reserve(16384);
    }

    uint32_t StringTableBuilder::Add(std::string_view text)
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

    std::string_view StringTableBuilder::Text(uint32_t string_index) const noexcept
    {
        const manifest::StringRef& reference = references[string_index];
        return std::string_view{ blob }.substr(reference.Offset, reference.Length);
    }

    const std::vector<manifest::StringRef>& StringTableBuilder::References() const noexcept
    {
        return references;
    }

    const std::string& StringTableBuilder::Blob() const noexcept
    {
        return blob;
    }

    void AppendBytes(std::string& out, const void* data, size_t size)
    {
        out.append(static_cast<const char*>(data), size);
    }

    void AlignTo8(std::string& out)
    {
        while ((out.size() % 8u) != 0u)
        {
            out.push_back('\0');
        }
    }

    uint32_t PassThroughIndex(uint32_t index) noexcept
    {
        return index;
    }

    manifest::Binding MakeBindingRecord(const ReflectedBinding& binding,
                                        StringTableBuilder& strings,
                                        std::vector<manifest::UniformMember>& member_records)
    {
        manifest::Binding record;
        record.ByteSize = binding.ByteSize;
        record.NameString = strings.Add(binding.Name);
        record.ScopeString = strings.Add(binding.ScopeName);
        // A bound placement is the only one the cooker produces today. Its payload is a group and a binding.
        record.Placement = manifest::PlacementPayload{ .Word0 = GroupOf(binding), .Word1 = BindingOf(binding) };
        record.ElementStride = binding.ElementStride;
        record.ArrayCount = binding.ArrayCount;
        record.StorageFormat = static_cast<uint32_t>(binding.StorageFormat);
        record.PlacementKind = static_cast<uint8_t>(
            GetBoundPlacement(binding.Placement) != nullptr ? PlacementKind::Bound : PlacementKind::None);
        record.Kind = static_cast<uint8_t>(binding.Kind);
        record.Shape = static_cast<uint8_t>(binding.Shape);
        record.IsComparisonSampler = static_cast<uint8_t>(binding.IsComparisonSampler);
        record.Access = static_cast<uint8_t>(binding.Access);

        record.FirstUniformMember = static_cast<uint32_t>(member_records.size());
        record.UniformMemberCount = static_cast<uint32_t>(binding.UniformMembers.size());
        member_records.reserve(member_records.size() + binding.UniformMembers.size());
        for (const ReflectedUniformMember& member : binding.UniformMembers)
        {
            manifest::UniformMember memberRecord;
            memberRecord.NameString = strings.Add(member.Name);
            memberRecord.Offset = member.Offset;
            memberRecord.Size = member.Size;
            memberRecord.ArrayCount = member.ArrayCount;
            // widen these types to uint32_t, since this is all 8-byte aligned anyways
            memberRecord.ElementStride = static_cast<uint32_t>(member.ElementStride);
            memberRecord.MatrixLayout = static_cast<uint32_t>(member.MatrixLayout);
            member_records.emplace_back(memberRecord);
        }

        return record;
    }

    manifest::Footprint MakeFootprintRecord(const ResourceFootprint& footprint) noexcept
    {
        if (const BufferFootprint* buffer = std::get_if<BufferFootprint>(&footprint))
        {
            return manifest::Footprint{ .ElementCount = buffer->ElementCount,
                                        .Kind = static_cast<uint32_t>(FootprintKind::Buffer) };
        }

        if (const TextureFootprint* texture = std::get_if<TextureFootprint>(&footprint))
        {
            return manifest::Footprint{ .ExtentX = texture->ExtentX,
                                        .ExtentY = texture->ExtentY,
                                        .ExtentZ = texture->ExtentZ,
                                        .Kind = static_cast<uint32_t>(FootprintKind::Texture) };
        }

        return manifest::Footprint{};
    }

    manifest::EntryPointInstance MakeSlotRecord(const LibraryVariant& variant, size_t entry_point_index) noexcept
    {
        manifest::EntryPointInstance slot;
        slot.SourceIndex = variant.SourceIndices[entry_point_index];
        slot.VisibilityIndex = variant.VisibilityIndices[entry_point_index];
        slot.WorkgroupX = variant.Workgroups[entry_point_index].X;
        slot.WorkgroupY = variant.Workgroups[entry_point_index].Y;
        slot.WorkgroupZ = variant.Workgroups[entry_point_index].Z;
        slot.RasterIndex = variant.RasterIndices[entry_point_index];
        return slot;
    }

    manifest::VertexInput MakeVertexInputRecord(const ReflectedVertexInput& input, StringTableBuilder& strings)
    {
        manifest::VertexInput record;
        record.SemanticNameString = strings.Add(input.SemanticName);
        record.SemanticIndex = input.Data.SemanticIndex;
        record.Location = input.Data.Location;
        record.ScalarType = static_cast<uint32_t>(input.Data.ScalarType);
        record.ComponentCount = input.Data.ComponentCount;
        return record;
    }

    manifest::ColorTarget MakeColorTargetRecord(const ReflectedColorTarget& target) noexcept
    {
        manifest::ColorTarget record;
        record.Location = target.Location;
        record.ScalarType = static_cast<uint32_t>(target.ScalarType);
        record.ComponentCount = target.ComponentCount;
        return record;
    }

    ModuleShape EncodeModuleShape(const CookedModule& module, StringTableBuilder& strings)
    {
        ModuleShape shape;
        shape.EntryPoints.reserve(module.EntryPoints.size());
        for (const LibraryEntryPoint& entryPoint : module.EntryPoints)
        {
            shape.EntryPoints.emplace_back(strings.Add(entryPoint.Name), static_cast<uint32_t>(entryPoint.Stage));
        }

        if (module.Space == nullptr)
        {
            return shape;
        }

        shape.Axes.reserve(module.Space->AxisCount());
        for (const PermutationAxis& axis : module.Space->Axes())
        {
            EncodedAxis encoded;
            encoded.NameString = strings.Add(axis.Name);
            encoded.Kind = axis.Kind;
            encoded.Domain = axis.ValueDomain;
            encoded.BindingTime = axis.BindingTime;
            encoded.Values.reserve(axis.NumValues());
            for (const PermutationValue& value : axis.GetValues())
            {
                switch (axis.ValueDomain)
                {
                case AxisValueDomain::Boolean:
                    [[fallthrough]];
                case AxisValueDomain::Integral:
                    encoded.Values.push_back(value.AsUInt());
                    break;
                case AxisValueDomain::Enum:
                    // an enum value is stored by its case name, the same way a type value is
                    encoded.Values.push_back(strings.Add(value.AsEnumCase(axis)));
                    break;
                case AxisValueDomain::Type:
                    encoded.Values.push_back(strings.Add(value.AsType(axis)));
                    break;
                case AxisValueDomain::None:
                    std::unreachable();
                }
            }
            shape.Axes.push_back(std::move(encoded));
        }

        return shape;
    }

    bool SameModuleShape(const ModuleShape& lhs, const ModuleShape& rhs) noexcept
    {
        auto sameEntryPoint = [](const manifest::EntryPoint& left, const manifest::EntryPoint& right)
        {
            return left.NameString == right.NameString && left.Stage == right.Stage;
        };

        return std::ranges::equal(lhs.EntryPoints, rhs.EntryPoints, sameEntryPoint) && lhs.Axes == rhs.Axes;
    }

    CookResult<manifest::ModuleAxis> MergeIntoRootAxes(std::vector<EncodedAxis>& root_axes,
                                                       const EncodedAxis& module_axis,
                                                       StringTableBuilder& strings)
    {
        // The value mask is 32 bits wide, so it is the hard limit. A nudge toward fewer values belongs in
        // the cook diagnostics, not in the format.
        if (module_axis.Values.size() > k_MaxModuleAxisValues)
        {
            std::println(stderr,
                         "[shader_cooker] axis '{}' holds {} values, and a module axis can hold at most {}",
                         strings.Text(module_axis.NameString),
                         module_axis.Values.size(),
                         k_MaxModuleAxisValues);
            return std::unexpected(CookError::ManifestAxisTooManyValues);
        }

        std::vector<AxisValueType> appendedValues;
        for (auto&& [rootIndex, rootAxis] : std::views::enumerate(root_axes))
        {
            uint32_t liveValuesMask = 0u;
            if (TryMergeValues(rootAxis, module_axis, appendedValues, liveValuesMask))
            {
                rootAxis.Values.append_range(appendedValues);
                return manifest::ModuleAxis{ .AxisIndex = static_cast<uint32_t>(rootIndex),
                                             .LiveValuesMask = liveValuesMask };
            }
        }

        root_axes.push_back(module_axis);
        const size_t valueCount = module_axis.Values.size();
        const uint32_t fullMask =
            valueCount == k_MaxModuleAxisValues ? ~0u : ((1u << static_cast<uint32_t>(valueCount)) - 1u);
        return manifest::ModuleAxis{ .AxisIndex = static_cast<uint32_t>(root_axes.size() - 1u),
                                     .LiveValuesMask = fullMask };
    }

    bool TryMergeValues(const EncodedAxis& root_axis,
                        const EncodedAxis& module_axis,
                        std::vector<AxisValueType>& appended_values,
                        uint32_t& live_values_mask)
    {
        const bool sameIdentity = root_axis.NameString == module_axis.NameString &&
                                  root_axis.Kind == module_axis.Kind && root_axis.Domain == module_axis.Domain &&
                                  root_axis.BindingTime == module_axis.BindingTime;
        if (!sameIdentity)
        {
            return false;
        }

        appended_values.clear();
        live_values_mask = 0u;
        // The digit order is the module's value order. Selecting set bits walks the root order. The two
        // agree only when every value lands at a higher root position than the value before it.
        int64_t lastPosition = -1;
        for (const AxisValueType value : module_axis.Values)
        {
            int64_t position = -1;
            const auto rootIter = std::ranges::find(root_axis.Values, value);
            if (rootIter != root_axis.Values.end())
            {
                position = std::distance(root_axis.Values.begin(), rootIter);
            }
            else
            {
                position = std::ssize(root_axis.Values) + std::ssize(appended_values);
                appended_values.push_back(value);
            }

            if (position <= lastPosition || position >= static_cast<int64_t>(k_MaxModuleAxisValues))
            {
                return false;
            }

            lastPosition = position;
            live_values_mask |= 1u << static_cast<uint32_t>(position);
        }

        return true;
    }

    std::vector<manifest::Axis> BuildRootAxisRecords(const std::vector<EncodedAxis>& root_axes,
                                                     std::vector<AxisValueType>& out_values)
    {
        std::vector<manifest::Axis> records;
        records.reserve(root_axes.size());
        for (const EncodedAxis& rootAxis : root_axes)
        {
            manifest::Axis record;
            record.NameString = rootAxis.NameString;
            record.FirstValue = static_cast<uint32_t>(out_values.size());
            record.ValueCount = static_cast<uint32_t>(rootAxis.Values.size());
            record.Kind = rootAxis.Kind;
            record.Domain = rootAxis.Domain;
            record.BindingTime = rootAxis.BindingTime;
            out_values.append_range(rootAxis.Values);
            records.push_back(record);
        }

        return records;
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

    RasterTables BuildRasterTables(const CookedModule& module, StringTableBuilder& strings)
    {
        RasterTables tables;
        tables.Rasters.reserve(module.RasterStates.size());

        for (const ReflectedRasterState& raster : module.RasterStates)
        {
            manifest::RasterState record;
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

    CookResult<VariantTables> BuildVariantTables(const CookedModule& module, StringTableBuilder& strings)
    {
        // The reader finds a variant by a binary search over the keys, so the three parallel tables are
        // written in key order, whatever order the cook appended the variants in.
        std::vector<uint32_t> order(module.Variants.size());
        std::ranges::iota(order, 0u);
        auto keyOf = [&module](uint32_t position) -> VariantKey
        {
            return module.VariantKeys[module.Variants[position].Index];
        };
        std::ranges::sort(order, std::less<VariantKey>{}, keyOf);

        const auto duplicate = std::ranges::adjacent_find(order, std::equal_to<VariantKey>{}, keyOf);
        if (duplicate != order.end())
        {
            std::println(stderr,
                         "[shader_cooker] module {} holds two variants with key {}",
                         module.Name,
                         std::to_underlying(keyOf(*duplicate)));
            return std::unexpected(CookError::ManifestDuplicateVariantKey);
        }

        const size_t axisCount = module.Space != nullptr ? module.Space->AxisCount() : 0u;
        const size_t maskWordCount = (axisCount + k_AxisMaskWordBits - 1u) / k_AxisMaskWordBits;

        VariantTables tables;
        tables.Keys.reserve(order.size());
        tables.Variants.reserve(order.size());
        tables.AxisMasks.assign(order.size() * maskWordCount, 0u);
        tables.Slots.reserve(order.size() * module.EntryPoints.size());

        for (auto&& [row, position] : std::views::enumerate(order))
        {
            const LibraryVariant& variant = module.Variants[position];
            tables.Keys.push_back(keyOf(position));
            tables.Variants.push_back(manifest::Variant{ .SuffixString = strings.Add(variant.Suffix),
                                                         .ResourceListIndex = variant.ResourceListIndex,
                                                         .FootprintListIndex = variant.FootprintListIndex });

            // an active binding points into the space's axis vector, so its offset there is the axis position
            for (const PermutationBinding& binding : variant.Active)
            {
                const size_t axisPosition = static_cast<size_t>(binding.Axis - module.Space->Axes().data());
                const size_t wordIndex = (static_cast<size_t>(row) * maskWordCount) + (axisPosition / k_AxisMaskWordBits);
                tables.AxisMasks[wordIndex] |= uint64_t{ 1u } << (axisPosition % k_AxisMaskWordBits);
            }

            for (size_t entryPointIndex = 0u; entryPointIndex < module.EntryPoints.size(); ++entryPointIndex)
            {
                tables.Slots.push_back(MakeSlotRecord(variant, entryPointIndex));
            }
        }

        return tables;
    }

    SourceTables BuildSourceTables(const CookedModule& module)
    {
        SourceTables tables;
        tables.Refs.reserve(module.Sources.size());

        for (const std::string& source : module.Sources)
        {
            tables.Refs.emplace_back(static_cast<uint32_t>(tables.Blob.size()), static_cast<uint32_t>(source.size()));
            tables.Blob.append(source);
        }

        return tables;
    }

    CookResult<std::string> BuildExtent(const CookedModule& module, StringTableBuilder& strings)
    {
        CookResult<VariantTables> variantsResult = BuildVariantTables(module, strings);
        if (!variantsResult)
        {
            return std::unexpected(variantsResult.error());
        }

        const VariantTables& variants = *variantsResult;
        const LayoutTables layouts = BuildLayoutTables(module, strings);
        const RasterTables rasters = BuildRasterTables(module, strings);
        const SourceTables sources = BuildSourceTables(module);

        manifest::EnvironmentHeader environment;
        std::string extent;
        extent.resize(sizeof(manifest::EnvironmentHeader), '\0');

        environment.VariantCount = static_cast<uint32_t>(variants.Variants.size());
        environment.VariantKeyTableOffset = AppendExtentTable(extent, variants.Keys);
        environment.VariantTableOffset = AppendExtentTable(extent, variants.Variants);
        environment.AxisMaskTableOffset = AppendExtentTable(extent, variants.AxisMasks);
        environment.SlotTableOffset = AppendExtentTable(extent, variants.Slots);

        environment.SourceTableOffset = AppendExtentTable(extent, sources.Refs);
        environment.SourceCount = static_cast<uint32_t>(sources.Refs.size());
        AlignTo8(extent);
        environment.SourceBlobOffset = static_cast<uint32_t>(extent.size());
        environment.SourceBlobSize = static_cast<uint32_t>(sources.Blob.size());
        extent.append(sources.Blob);

        environment.BindingTableOffset = AppendExtentTable(extent, layouts.Bindings);
        environment.BindingCount = static_cast<uint32_t>(layouts.Bindings.size());
        environment.ResourceListTableOffset = AppendExtentTable(extent, layouts.ResourceLists);
        environment.ResourceListCount = static_cast<uint32_t>(layouts.ResourceLists.size());
        environment.ResourceIndexTableOffset = AppendExtentTable(extent, layouts.ResourceIndices);
        environment.ResourceIndexCount = static_cast<uint32_t>(layouts.ResourceIndices.size());
        environment.FootprintTableOffset = AppendExtentTable(extent, layouts.Footprints);
        environment.FootprintCount = static_cast<uint32_t>(layouts.Footprints.size());
        environment.FootprintListTableOffset = AppendExtentTable(extent, layouts.FootprintLists);
        environment.FootprintListCount = static_cast<uint32_t>(layouts.FootprintLists.size());
        environment.VisibilityListTableOffset = AppendExtentTable(extent, layouts.VisibilityLists);
        environment.VisibilityListCount = static_cast<uint32_t>(layouts.VisibilityLists.size());
        environment.VisibilityIndexTableOffset = AppendExtentTable(extent, layouts.VisibilityIndices);
        environment.VisibilityIndexCount = static_cast<uint32_t>(layouts.VisibilityIndices.size());
        environment.RasterTableOffset = AppendExtentTable(extent, rasters.Rasters);
        environment.RasterCount = static_cast<uint32_t>(rasters.Rasters.size());
        environment.VertexInputTableOffset = AppendExtentTable(extent, rasters.VertexInputs);
        environment.VertexInputCount = static_cast<uint32_t>(rasters.VertexInputs.size());
        environment.ColorTargetTableOffset = AppendExtentTable(extent, rasters.ColorTargets);
        environment.ColorTargetCount = static_cast<uint32_t>(rasters.ColorTargets.size());
        environment.UniformMemberTableOffset = AppendExtentTable(extent, layouts.UniformMembers);
        environment.UniformMemberCount = static_cast<uint32_t>(layouts.UniformMembers.size());
        // reserved: the cooker writes no specialization constant yet, so the table stays empty
        AlignTo8(extent);
        environment.SpecializationConstantTableOffset = static_cast<uint32_t>(extent.size());
        environment.SpecializationConstantCount = 0u;

        AlignTo8(extent);
        std::memcpy(extent.data(), &environment, sizeof(manifest::EnvironmentHeader));
        return extent;
    }

    const CookedModule* FirstCookedEnvironment(const CookedLibrary& library, size_t module_index) noexcept
    {
        const size_t moduleCount = library.ModuleNames.size();
        for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
        {
            const std::optional<CookedModule>& environment =
                library.Environments[(profileIndex * moduleCount) + module_index];
            if (environment.has_value())
            {
                return &*environment;
            }
        }

        return nullptr;
    }

    CookError CheckBundleDirectory(const CookedLibrary& library, const manifest::BundleView& bundle)
    {
        if (bundle.ModuleCount() != library.ModuleNames.size() || bundle.Profiles().size() != library.Profiles.size())
        {
            std::println(stderr,
                         "[shader_cooker] manifest holds {} modules and {} profiles, the cook produced {} and {}",
                         bundle.ModuleCount(),
                         bundle.Profiles().size(),
                         library.ModuleNames.size(),
                         library.Profiles.size());
            return CookError::LibraryRoundTripFailed;
        }

        for (uint32_t moduleIndex = 0u; moduleIndex < bundle.ModuleCount(); ++moduleIndex)
        {
            if (bundle.Module(moduleIndex).Name() != library.ModuleNames[moduleIndex])
            {
                std::println(stderr,
                             "[shader_cooker] manifest names module '{}', but the cook produced '{}'",
                             bundle.Module(moduleIndex).Name(),
                             library.ModuleNames[moduleIndex]);
                return CookError::LibraryRoundTripFailed;
            }
        }

        for (auto&& [profileIndex, profile] : std::views::enumerate(bundle.Profiles()))
        {
            const CookedProfile& expected = library.Profiles[static_cast<size_t>(profileIndex)];
            if (bundle.String(profile.TargetNameString) != expected.TargetName ||
                profile.AccessModel != expected.AccessModel)
            {
                std::println(stderr,
                             "[shader_cooker] manifest profile {} names target '{}', but the cook produced '{}'",
                             profileIndex,
                             bundle.String(profile.TargetNameString),
                             expected.TargetName);
                return CookError::LibraryRoundTripFailed;
            }
        }

        return CookError::Success;
    }

    CookError CheckEnvironment(const CookedModule& module,
                               const manifest::EnvironmentView& view,
                               uint32_t& checked_count)
    {
        if (view.Variants().size() != module.Variants.size())
        {
            std::println(stderr,
                         "[shader_cooker] manifest module {} holds {} variants, the cook produced {}",
                         module.Name,
                         view.Variants().size(),
                         module.Variants.size());
            return CookError::ManifestMissingVariant;
        }

        const manifest::ShaderSourceProvider provider{ view, 0u };
        for (const LibraryVariant& variant : module.Variants)
        {
            const VariantKey key = module.VariantKeys[variant.Index];
            const int32_t variantIndex = view.FindVariant(key);
            if (variantIndex < 0)
            {
                std::println(stderr, "[shader_cooker] manifest holds no variant {} [{}]", variant.Index, variant.Description);
                return CookError::ManifestMissingVariant;
            }

            const CookError keyError = CheckVariantKeyDecode(view, variant, key);
            if (keyError != CookError::Success)
            {
                return keyError;
            }

            const CookError maskError = CheckVariantAxisMask(module, view, variant, static_cast<uint32_t>(variantIndex));
            if (maskError != CookError::Success)
            {
                return maskError;
            }

            for (size_t entryPointIndex = 0u; entryPointIndex < module.EntryPoints.size(); ++entryPointIndex)
            {
                const CookError slotError = CheckManifestSlot(
                    module, view, provider, variant, static_cast<uint32_t>(variantIndex), entryPointIndex);
                if (slotError != CookError::Success)
                {
                    return slotError;
                }

                ++checked_count;
            }
        }

        return CookError::Success;
    }

    CookError CheckVariantKeyDecode(const manifest::EnvironmentView& view,
                                    const LibraryVariant& variant,
                                    VariantKey key)
    {
        // A second opinion on the key: decode it the way a client does, through the module axes and the
        // root values, and compare each value against the canonical assignment the cook keyed.
        const manifest::ModuleView& moduleView = view.Module();
        const uint32_t axisCount = moduleView.AxisCount();
        if (variant.Canonical.size() != axisCount)
        {
            std::println(stderr,
                         "[shader_cooker] manifest module {} has {} axes, variant [{}] has {}",
                         moduleView.Name(),
                         axisCount,
                         variant.Description,
                         variant.Canonical.size());
            return CookError::ManifestVariantKeyMismatch;
        }

        std::vector<uint32_t> radices(axisCount);
        std::vector<uint32_t> digits(axisCount);
        for (uint32_t axisIndex = 0u; axisIndex < axisCount; ++axisIndex)
        {
            radices[axisIndex] = moduleView.AxisValueCount(axisIndex);
        }
        UnpackVariantKey(key, radices, digits);

        for (uint32_t axisIndex = 0u; axisIndex < axisCount; ++axisIndex)
        {
            const PermutationBinding& binding = variant.Canonical[axisIndex];
            const PermutationAxis& axis = *binding.Axis;
            const AxisValueType stored = moduleView.AxisValue(axisIndex, digits[axisIndex]);

            bool matches = view.String(moduleView.AxisData(axisIndex).NameString) == axis.Name;
            switch (axis.ValueDomain)
            {
            case AxisValueDomain::Boolean:
                [[fallthrough]];
            case AxisValueDomain::Integral:
                matches = matches && stored == binding.Value.AsUInt();
                break;
            case AxisValueDomain::Enum:
                matches = matches && view.String(stored) == binding.Value.AsEnumCase(axis);
                break;
            case AxisValueDomain::Type:
                matches = matches && view.String(stored) == binding.Value.AsType(axis);
                break;
            case AxisValueDomain::None:
                std::unreachable();
            }

            if (!matches)
            {
                std::println(stderr,
                             "[shader_cooker] manifest key {} of module {} decodes axis '{}' differently than "
                             "variant [{}]",
                             std::to_underlying(key),
                             moduleView.Name(),
                             axis.Name,
                             variant.Description);
                return CookError::ManifestVariantKeyMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckVariantAxisMask(const CookedModule& module,
                                   const manifest::EnvironmentView& view,
                                   const LibraryVariant& variant,
                                   uint32_t variant_index)
    {
        if (module.Space == nullptr)
        {
            return CookError::Success;
        }

        const std::span<const PermutationAxis> axes = module.Space->Axes();
        for (uint32_t axisIndex = 0u; axisIndex < axes.size(); ++axisIndex)
        {
            const PermutationAxis* axis = &axes[axisIndex];
            const bool expected = std::ranges::contains(variant.Active, axis, &PermutationBinding::Axis);
            if (view.IsAxisActive(variant_index, axisIndex) != expected)
            {
                std::println(stderr,
                             "[shader_cooker] manifest marks axis '{}' {} in variant [{}], the cook did not",
                             axis->Name,
                             expected ? "inactive" : "active",
                             variant.Description);
                return CookError::ManifestVariantAxisMaskMismatch;
            }
        }

        return CookError::Success;
    }

    CookError CheckManifestSource(const CookedModule& module,
                                  const manifest::ShaderSourceProvider& provider,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index)
    {
        const std::string_view expectedSource = ResolveSource(module, variant, entry_point_index);
        const VariantKey variantKey = module.VariantKeys[variant.Index];
        const std::string_view providerSource =
            provider.Source(static_cast<uint32_t>(entry_point_index), variantKey);
        if (providerSource == expectedSource)
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
                                     const manifest::ShaderSourceProvider& provider,
                                     const LibraryVariant& variant,
                                     size_t entry_point_index)
    {
        const WorkgroupSize expected = variant.Workgroups[entry_point_index];
        const VariantKey variantKey = module.VariantKeys[variant.Index];
        const WorkgroupSize read = provider.Workgroup(static_cast<uint32_t>(entry_point_index), variantKey);

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

    bool RecordMatchesFootprint(const manifest::Footprint& record, const ResourceFootprint& footprint) noexcept
    {
        const manifest::Footprint expected = MakeFootprintRecord(footprint);
        return record.Kind == expected.Kind && record.ElementCount == expected.ElementCount &&
               record.ExtentX == expected.ExtentX && record.ExtentY == expected.ExtentY &&
               record.ExtentZ == expected.ExtentZ;
    }

    bool RecordMatchesBinding(const manifest::Binding& record, const ReflectedBinding& binding) noexcept
    {
        return record.ByteSize == binding.ByteSize &&
               record.Placement.Word0 == GroupOf(binding) &&
               record.Placement.Word1 == BindingOf(binding) &&
               record.ElementStride == binding.ElementStride &&
               record.ArrayCount == binding.ArrayCount &&
               record.StorageFormat == static_cast<uint32_t>(binding.StorageFormat) &&
               record.UniformMemberCount == static_cast<uint32_t>(binding.UniformMembers.size()) &&
               record.Kind == static_cast<uint8_t>(binding.Kind) &&
               record.Shape == static_cast<uint8_t>(binding.Shape) &&
               record.IsComparisonSampler == static_cast<uint8_t>(binding.IsComparisonSampler) &&
               record.Access == static_cast<uint8_t>(binding.Access);
    }

    bool ManifestUniformMembersMatch(const manifest::EnvironmentView& view,
                                     const manifest::Binding& read,
                                     const ReflectedBinding& expected)
    {
        const std::span<const manifest::UniformMember> readMembers = view.UniformMembers(read);
        if (readMembers.size() != expected.UniformMembers.size())
        {
            return false;
        }

        for (size_t memberIndex = 0u; memberIndex < readMembers.size(); ++memberIndex)
        {
            const ReflectedUniformMember& expectedMember = expected.UniformMembers[memberIndex];
            const manifest::UniformMember& readMember = readMembers[memberIndex];

            const bool matches = view.String(readMember.NameString) == expectedMember.Name &&
                                 readMember.Offset == expectedMember.Offset &&
                                 readMember.Size == expectedMember.Size &&
                                 readMember.ArrayCount == expectedMember.ArrayCount &&
                                 readMember.ElementStride == static_cast<uint32_t>(expectedMember.ElementStride) &&
                                 readMember.MatrixLayout == static_cast<uint32_t>(expectedMember.MatrixLayout);
            if (!matches)
            {
                return false;
            }
        }

        return true;
    }

    CookError CheckManifestLayout(const CookedModule& module,
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  uint32_t variant_index,
                                  size_t entry_point_index)
    {
        const manifest::Variant& readVariant = view.Variants()[variant_index];
        const CookResult<ShaderLayoutView> expectedLayoutResult = ResolveLayoutView(module, variant, entry_point_index);
        if (!expectedLayoutResult)
        {
            return expectedLayoutResult.error();
        }
        const ShaderLayoutView& expectedLayout = *expectedLayoutResult;

        const std::span<const uint32_t> resources = view.ResourceList(readVariant.ResourceListIndex);
        const std::span<const manifest::Footprint> footprints = view.FootprintList(readVariant.FootprintListIndex);
        const std::span<const manifest::EntryPointInstance> slots = view.VariantSlots(variant_index);

        if (entry_point_index >= slots.size())
        {
            std::println(stderr,
                         "[shader_cooker] manifest variant {} holds no slot {}",
                         variant.Index,
                         entry_point_index);
            return CookError::ManifestVariantMissingEntryPoint;
        }

        const std::span<const uint32_t> visible = view.VisibilityList(slots[entry_point_index].VisibilityIndex);

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

            if (local >= footprints.size())
            {
                std::println(stderr,
                             "[shader_cooker] manifest variant {} resolves resource {} out of range",
                             variant.Index,
                             local);
                return CookError::ManifestVariantResourceResolveOutOfRange;
            }

            const manifest::Binding& read = view.Bindings()[resources[local]];

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

    CookError CheckManifestVertexInputs(std::span<const manifest::VertexInput> read_inputs,
                                        const ReflectedRasterState& expected_raster,
                                        const manifest::EnvironmentView& view)
    {
        for (size_t inputIndex = 0u; inputIndex < read_inputs.size(); ++inputIndex)
        {
            const ReflectedVertexInput& expectedInput = expected_raster.VertexInputs[inputIndex];
            const manifest::VertexInput& readInput = read_inputs[inputIndex];

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

    CookError CheckManifestColorTargets(std::span<const manifest::ColorTarget> read_targets,
                                        const ReflectedRasterState& expected_raster)
    {
        for (size_t targetIndex = 0u; targetIndex < read_targets.size(); ++targetIndex)
        {
            const ReflectedColorTarget& expectedTarget = expected_raster.ColorTargets[targetIndex];
            const manifest::ColorTarget& readTarget = read_targets[targetIndex];

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
                                  const manifest::EnvironmentView& view,
                                  const LibraryVariant& variant,
                                  size_t entry_point_index)
    {
        const uint32_t rasterIndex = variant.RasterIndices[entry_point_index];
        const ReflectedRasterState& expectedRaster = module.RasterStates[rasterIndex];

        const std::span<const manifest::VertexInput> readInputs = view.VertexInputs(rasterIndex);
        const std::span<const manifest::ColorTarget> readTargets = view.ColorTargets(rasterIndex);

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

    CookError CheckManifestSlot(const CookedModule& module,
                                const manifest::EnvironmentView& view,
                                const manifest::ShaderSourceProvider& provider,
                                const LibraryVariant& variant,
                                uint32_t variant_index,
                                size_t entry_point_index)
    {
        const CookError sourceError = CheckManifestSource(module, provider, variant, entry_point_index);
        if (sourceError != CookError::Success)
        {
            return sourceError;
        }

        const CookError workgroupError = CheckManifestWorkgroup(module, provider, variant, entry_point_index);
        if (workgroupError != CookError::Success)
        {
            return workgroupError;
        }

        const CookError layoutError = CheckManifestLayout(module, view, variant, variant_index, entry_point_index);
        if (layoutError != CookError::Success)
        {
            return layoutError;
        }

        return CheckManifestRaster(module, view, variant, entry_point_index);
    }

} // namespace

} // namespace lodestone
