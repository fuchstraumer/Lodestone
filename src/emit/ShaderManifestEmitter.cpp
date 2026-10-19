#include "emit/ShaderManifestEmitter.hpp"
#include "VariantKey.hpp"
#include "model/AxisInfluence.hpp"
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

namespace lodestone
{

namespace
{

    /** Deduplicates strings and returns the index of each one. Equal strings share one entry, so the blob
     * stores each binding name once, no matter how many layouts use it. One builder is used for the whole
     * cook, so names shared by several modules or profiles are also stored once. */
    // todo-ship: There are better ways to do this, and we should absolutely explore them since strings
    // will rapidly become a huge cost as variant count increases
    class StringTableBuilder
    {
    public:
        StringTableBuilder();
        uint32_t Add(std::string_view text);
        [[nodiscard]] std::string_view Text(uint32_t string_index) const noexcept;
        [[nodiscard]] const std::vector<manifest::StringRef>& References() const noexcept;
        [[nodiscard]] const std::vector<std::byte>& Blob() const noexcept;
        [[nodiscard]] std::vector<std::byte>&& TakeBlob() noexcept;

    private:
        std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> lookup;
        std::vector<manifest::StringRef> references;
        std::vector<std::byte> blob;
    };

    /** One axis in the vocabulary of the manifest. Named values (types and enum cases) are already string
     * indices here, so two modules that use the same case store the same value. */
    struct EncodedAxis
    {
        uint32_t NameString{};
        AxisKind Kind{ AxisKind::None };
        AxisValueDomain Domain{ AxisValueDomain::None };
        EarliestBindingTime BindingTime{ EarliestBindingTime::None };
        std::vector<AxisValueType> Values;

        bool operator==(const EncodedAxis& other) const = default;
    };

    /** The parts of a module that do not depend on the profile it was cooked for. */
    struct ModuleShape
    {
        std::vector<manifest::EntryPoint> EntryPoints;
        std::vector<EncodedAxis> Axes;
        // the declared default of each axis, as a digit in the module's own value order. this is kept apart
        // from EncodedAxis, because EncodedAxis is also the root axis record, and root axes are shared
        // between modules that can declare different defaults
        std::vector<uint32_t> AxisDefaultDigits;
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
        std::vector<manifest::SamplerTexturePair> SamplerTexturePairs;
        std::vector<manifest::Run> SamplerPairLists;
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
        std::vector<std::byte> Blob;
        std::vector<manifest::SourceRef> Refs;
    };

    constexpr uint32_t k_MaxModuleAxisValues = 32u;
    constexpr size_t k_AxisMaskWordBits = 64u;

    /** @brief Writes the actual bytes to the given `out` string. */
    void AppendBytes(std::vector<std::byte>& out, const void* data, size_t size);

    /** Pads to the next 8-byte boundary. Binding records and axis values contain 64-bit fields, and the
     * reader maps them in place, so every section must start aligned. */
    void AlignTo8(std::vector<std::byte>& out);

    /** @brief Appends a table of records to the output string, aligned to 8 bytes. Returns the offset
     *  of the first record in the output string where the new records are now located */
    template<typename RecordType>
    manifest::TableRef AppendTable(std::vector<std::byte>& out, const std::vector<RecordType>& records)
    {
        AlignTo8(out);
        const uint32_t offset = static_cast<uint32_t>(out.size());
        if (!records.empty())
        {
            AppendBytes(out, records.data(), records.size() * sizeof(RecordType));
        }

        return manifest::TableRef{ offset, static_cast<uint32_t>(records.size()) };
    }

    template<typename RecordType>
    manifest::TableRef64 AppendTable64(std::vector<std::byte>& out, const std::vector<RecordType>& records)
    {
        AlignTo8(out);
        const uint64_t offset = out.size();
        if (!records.empty())
        {
            AppendBytes(out, records.data(), records.size() * sizeof(RecordType));
        }

        return manifest::TableRef64{ offset, static_cast<uint64_t>(records.size()) };
    }

    template<typename PayloadType, typename RecordType, typename MakeRecordFn>
    constexpr void AppendRuns(const std::vector<std::vector<PayloadType>>& lists,
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

    constexpr uint32_t PassThroughIndex(uint32_t index) noexcept
    {
        return index;
    }
    manifest::Binding MakeBindingRecord(const ReflectedBinding& binding,
                                        StringTableBuilder& strings,
                                        std::vector<manifest::UniformMember>& member_records);
    manifest::Footprint MakeFootprintRecord(const ResourceFootprint& footprint) noexcept;
    manifest::EntryPointInstance MakeSlotRecord(const LibraryVariant& variant, size_t entry_point_index) noexcept;
    manifest::VertexInput MakeVertexInputRecord(const ReflectedVertexInput& input, StringTableBuilder& strings);
    manifest::ColorTarget MakeColorTargetRecord(const ReflectedColorTarget& target) noexcept;

    /** The module's axes and entry points, as written to the manifest. Every profile of one module must
     * produce the same shape, because variant keys are packed against the module's axes. */
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
    CookResult<std::vector<std::byte>> BuildExtent(const CookedModule& module, StringTableBuilder& strings);

    /** The first profile that cooked the module, or null when no profile did. */
    const CookedModule* FirstCookedEnvironment(const CookedLibrary& library, size_t module_index) noexcept;

    CookError CheckBundleDirectory(const CookedLibrary& library, const manifest::BundleView& bundle);
    CookError CheckEnvironment(const CookedModule& module,
                               const manifest::EnvironmentView& view,
                               uint32_t& checked_count);
    CookError CheckVariantKeyDecode(const manifest::EnvironmentView& view,
                                    const LibraryVariant& variant,
                                    VariantKey key);
    /** The axis relevance validator. It reads the written environment only, through the client views, and
     * derives relevance a second time */
    CookError VerifyAxisRelevance(const manifest::EnvironmentView& view);
    CookError CheckVariantAxisMask(const CookedModule& module,
                                   const LibraryVariant& variant,
                                   manifest::VariantView read_variant);
    /** True when the value stored at `digit` of one module axis is `value` of `axis`. Both the key check and
     * the default check read values this way, which is the same way a client reads them. */
    bool StoredAxisValueMatches(const manifest::EnvironmentView& view,
                                uint32_t axis_index,
                                uint32_t digit,
                                const PermutationAxis& axis,
                                const PermutationValue& value);
    CookError CheckAxisDefaults(const CookedModule& module, const manifest::EnvironmentView& view);
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
                                  manifest::VariantView read_variant,
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
    /** Checks every field the manifest stores for one entry point of one variant. */
    CookError CheckManifestSlot(const CookedModule& module,
                                const manifest::EnvironmentView& view,
                                const manifest::ShaderSourceProvider& provider,
                                const LibraryVariant& variant,
                                manifest::VariantView read_variant,
                                size_t entry_point_index);

} // namespace

CookResult<std::vector<std::byte>> EmitShaderManifest(const CookedLibrary& library)
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
                             "[lodestone] module {} states different entry points or axes for target '{}'",
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
        const ModuleShape& shape = moduleShapes[moduleIndex];
        for (const auto&& [axisIndex, moduleAxis] : std::views::enumerate(shape.Axes))
        {
            CookResult<manifest::ModuleAxis> merged = MergeIntoRootAxes(rootAxes, moduleAxis, strings);
            if (!merged)
            {
                return std::unexpected(merged.error());
            }
            merged->DefaultDigit = shape.AxisDefaultDigits[static_cast<size_t>(axisIndex)];
            moduleAxes[moduleIndex].push_back(std::move(*merged));
        }
    }

    std::vector<manifest::Profile> profiles;
    profiles.reserve(profileCount);
    for (const CookedProfile& profile : library.Profiles)
    {
        const uint16_t flags = profile.SamplerTexturePairsRecorded
                                   ? std::to_underlying(manifest::ProfileFlags::SamplerTexturePairsRecorded)
                                   : uint16_t{ 0u };
        profiles.push_back(manifest::Profile{ .TargetNameString = strings.Add(profile.TargetName),
                                              .AccessModel = profile.AccessModel,
                                              .CodeFormat = profile.CodeFormat,
                                              .Flags = flags });
    }

    // build every extent before the header region: extents add binding names and suffixes to the string
    // table, and the string table is part of the header region
    std::vector<std::vector<std::byte>> extents(library.Environments.size());
    for (size_t environmentIndex = 0u; environmentIndex < library.Environments.size(); ++environmentIndex)
    {
        const std::optional<CookedModule>& environment = library.Environments[environmentIndex];
        if (!environment.has_value())
        {
            continue;
        }

        CookResult<std::vector<std::byte>> extent = BuildExtent(*environment, strings);
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

    // The module headers sit right after the header, so the reader finds them with no offset.
    std::vector<std::byte> bytes;
    bytes.resize(sizeof(manifest::Header) + (moduleCount * sizeof(manifest::ModuleRootHeader)), std::byte{ 0 });
    header.Profiles = AppendTable64(bytes, profiles);
    header.Axes = AppendTable64(bytes, rootAxisRecords);
    header.AxesValues = AppendTable64(bytes, rootAxisValues);
    manifest::TableRef64 envDir = AppendTable64(bytes, directory);
    header.EnvironmentDirectoryOffset = envDir.Offset;

    for (size_t moduleIndex = 0u; moduleIndex < moduleCount; ++moduleIndex)
    {
        manifest::ModuleRootHeader& moduleHeader = moduleHeaders[moduleIndex];
        moduleHeader.EntryPoints = AppendTable64(bytes, moduleShapes[moduleIndex].EntryPoints);
        moduleHeader.ModuleAxes = AppendTable64(bytes, moduleAxes[moduleIndex]);
    }

    header.Strings = AppendTable64(bytes, strings.References());

    AlignTo8(bytes);
    header.StringBlobs.Offset = bytes.size();
    header.StringBlobs.Count = strings.Blob().size();
    bytes.append_range(std::move(strings.TakeBlob()));

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
        directory[environmentIndex] = manifest::EnvironmentDirectoryEntry
        {
            .ExtentOffset = bytes.size(), .ExtentSize = extents[environmentIndex].size()
        };
        bytes.append_range(std::move(extents[environmentIndex]));
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

CookError VerifyManifestRoundTrip(const CookedLibrary& library, const std::vector<std::byte>& manifest_bytes)
{
    const std::span<const std::byte> raw{ manifest_bytes.data(), manifest_bytes.size() };

    const manifest::ManifestResult<manifest::BundleView> opened = manifest::BundleView::Open(raw);
    if (!opened)
    {
        std::println(stderr,
                     "[lodestone] manifest bundle does not open: {}",
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
                                 "[lodestone] manifest holds module {} for target '{}', but the cook did not",
                                 library.ModuleNames[moduleIndex],
                                 library.Profiles[profileIndex].TargetName);
                    return CookError::LibraryRoundTripFailed;
                }
                continue;
            }

            if (!view)
            {
                std::println(stderr,
                             "[lodestone] module {} for target '{}' does not open: {}",
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
                 "[lodestone] manifest round trip verified: {} modules x {} profiles, {} entrypoint variants "
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

        const uint32_t index{ static_cast<uint32_t>(references.size()) };
        references.emplace_back(static_cast<uint32_t>(blob.size()), static_cast<uint32_t>(text.size()));
        std::span<const std::byte> bytes{ reinterpret_cast<const std::byte*>(text.data()), text.size() };
        blob.append_range(bytes);
        lookup.emplace(std::string{ text }, index);
        return index;
    }

    std::string_view StringTableBuilder::Text(uint32_t string_index) const noexcept
    {
        const manifest::StringRef& reference = references[string_index];
        // jut casts all the way down....
        std::span<const char> asChars{ reinterpret_cast<const char*>(blob.data()), blob.size() };
        return std::string_view{ asChars }.substr(reference.Offset, reference.Length);
    }

    const std::vector<manifest::StringRef>& StringTableBuilder::References() const noexcept
    {
        return references;
    }

    const std::vector<std::byte>& StringTableBuilder::Blob() const noexcept
    {
        return blob;
    }

    std::vector<std::byte>&& StringTableBuilder::TakeBlob() noexcept
    {
        return std::move(blob);
    }

    void AppendBytes(std::vector<std::byte>& out, const void* data, size_t size)
    {
        const size_t oldTail = out.size();
        out.resize(out.size() + size);
        std::memcpy(out.data() + oldTail, data, size);
    }

    void AlignTo8(std::vector<std::byte>& out)
    {
        // use size_t{} to make sure it is for sure 64 bits on x64
        const size_t alignedSize = (out.size() + 7u) & ~size_t{ 7u };
        out.resize(alignedSize, static_cast<std::byte>(0));
    }

    manifest::Binding MakeBindingRecord(const ReflectedBinding& binding,
                                        StringTableBuilder& strings,
                                        std::vector<manifest::UniformMember>& member_records)
    {
        manifest::Binding record;
        record.ByteSize = binding.ByteSize;
        record.NameString = strings.Add(binding.Name);
        record.ScopeString = strings.Add(binding.ScopeName);
        // the cooker only produces bound placements today, so the payload is a group and a binding
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
        record.SampleType = static_cast<uint8_t>(binding.SampleType);

        record.FirstUniformMember = static_cast<uint32_t>(member_records.size());
        record.UniformMemberCount = static_cast<uint32_t>(binding.UniformMembers.size());
        member_records.reserve(member_records.size() + binding.UniformMembers.size());
        for (const ReflectedUniformMember& member : binding.UniformMembers)
        {
            manifest::UniformMember memberRecord;
            memberRecord.NameString = strings.Add(member.Name);
            memberRecord.Offset = member.Data.Offset;
            memberRecord.Size = member.Data.Size;
            memberRecord.ArrayCount = member.Data.ArrayCount;
            // widen these types to uint32_t, since this is all 8-byte aligned anyways
            memberRecord.ElementStride = static_cast<uint32_t>(member.Data.ElementStride);
            memberRecord.MatrixLayout = static_cast<uint32_t>(member.Data.MatrixLayout);
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

    manifest::SamplerTexturePair MakeSamplerPairRecord(const SamplerTexturePair& pair) noexcept
    {
        return manifest::SamplerTexturePair{ .Texture = pair.Texture, .Sampler = pair.Sampler };
    }

    manifest::EntryPointInstance MakeSlotRecord(const LibraryVariant& variant, size_t entry_point_index) noexcept
    {
        manifest::EntryPointInstance slot;
        slot.Source = variant.SourceIndices[entry_point_index];
        slot.Visibility = variant.VisibilityIndices[entry_point_index];
        slot.WorkgroupX = variant.Workgroups[entry_point_index].X;
        slot.WorkgroupY = variant.Workgroups[entry_point_index].Y;
        slot.WorkgroupZ = variant.Workgroups[entry_point_index].Z;
        slot.Raster = variant.RasterIndices[entry_point_index];
        slot.SamplerPairs = variant.SamplerPairIndices[entry_point_index];
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
            shape.AxisDefaultDigits.push_back(axis.DeclaredDefaultIndex());
        }

        return shape;
    }

    bool SameModuleShape(const ModuleShape& lhs, const ModuleShape& rhs) noexcept
    {
        auto sameEntryPoint = [](const manifest::EntryPoint& left, const manifest::EntryPoint& right)
        {
            return left.NameString == right.NameString && left.Stage == right.Stage;
        };

        return std::ranges::equal(lhs.EntryPoints, rhs.EntryPoints, sameEntryPoint) && lhs.Axes == rhs.Axes &&
               lhs.AxisDefaultDigits == rhs.AxisDefaultDigits;
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
                         "[lodestone] axis '{}' holds {} values, and a module axis can hold at most {}",
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
        AppendRuns(module.SamplerPairLists, tables.SamplerTexturePairs, tables.SamplerPairLists, &MakeSamplerPairRecord);

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
                         "[lodestone] module {} holds two variants with key {}",
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
            tables.Variants.push_back(
                manifest::Variant{ .SuffixString = strings.Add(variant.Suffix),
                                       .ResourceList = variant.ResourceListIndex,
                                       .FootprintList = variant.FootprintListIndex });

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

        for (const std::vector<std::byte>& source : module.Sources)
        {
            tables.Refs.emplace_back(static_cast<uint32_t>(tables.Blob.size()), static_cast<uint32_t>(source.size()));
            tables.Blob.append_range(source);
        }

        return tables;
    }

    /** Returns one axis relevance mask for each entry point, in the word layout of the axis mask table. Bit k is
     * set when `model/AxisInfluence` measures axis k as Active or Undetermined for that entry point. */
    CookResult<std::vector<uint64_t>> BuildAxisRelevanceTable(const CookedModule& module)
    {
        const size_t axisCount = module.Space != nullptr ? module.Space->AxisCount() : 0u;
        const size_t maskWordCount = (axisCount + k_AxisMaskWordBits - 1u) / k_AxisMaskWordBits;
        std::vector<uint64_t> relevance(module.EntryPoints.size() * maskWordCount, 0u);
        if (axisCount == 0u)
        {
            return relevance;
        }

        const CookResult<ModuleInfluence> influence = ComputeActualInfluence(module);
        if (!influence)
        {
            return std::unexpected(influence.error());
        }

        for (auto&& [entryPointIndex, entryPoint] : std::views::enumerate(influence->EntryPoints))
        {
            for (auto&& [axisIndex, axisInfluence] : std::views::enumerate(entryPoint.Axes))
            {
                if (IsAxisRelevant(axisInfluence))
                {
                    const size_t wordIndex = (static_cast<size_t>(entryPointIndex) * maskWordCount) +
                                             (static_cast<size_t>(axisIndex) / k_AxisMaskWordBits);
                    relevance[wordIndex] |= uint64_t{ 1u } << (static_cast<size_t>(axisIndex) % k_AxisMaskWordBits);
                }
            }
        }

        return relevance;
    }

    CookResult<std::vector<std::byte>> BuildExtent(const CookedModule& module, StringTableBuilder& strings)
    {
        CookResult<VariantTables> variantsResult = BuildVariantTables(module, strings);
        if (!variantsResult)
        {
            return std::unexpected(variantsResult.error());
        }

        const VariantTables& variants = *variantsResult;
        const CookResult<std::vector<uint64_t>> relevance = BuildAxisRelevanceTable(module);
        if (!relevance)
        {
            return std::unexpected(relevance.error());
        }
        const LayoutTables layouts = BuildLayoutTables(module, strings);
        const RasterTables rasters = BuildRasterTables(module, strings);
        SourceTables sources = BuildSourceTables(module);

        manifest::EnvironmentHeader environment;
        std::vector<std::byte> extent;
        extent.resize(sizeof(manifest::EnvironmentHeader), std::byte{ 0 });

        // The variant count sizes these four tables, so each keeps its offset only.
        environment.VariantCount = static_cast<uint32_t>(variants.Variants.size());
        environment.VariantKeyTableOffset = AppendTable(extent, variants.Keys).Offset;
        environment.VariantTableOffset = AppendTable(extent, variants.Variants).Offset;
        environment.AxisMaskTableOffset = AppendTable(extent, variants.AxisMasks).Offset;
        environment.EntryPointInstanceTableOffset = AppendTable(extent, variants.Slots).Offset;
        // the entry point count and the module's axis count size this table, so it keeps its offset only
        environment.AxisRelevanceTableOffset = AppendTable(extent, relevance.value()).Offset;

        environment.Sources = AppendTable(extent, sources.Refs);
        // The count of the blob is in bytes.
        AlignTo8(extent);
        environment.SourceBlob = manifest::TableRef{ static_cast<uint32_t>(extent.size()),
                                                     static_cast<uint32_t>(sources.Blob.size()) };
        extent.append_range(std::move(sources.Blob));

        environment.Bindings = AppendTable(extent, layouts.Bindings);
        environment.ResourceLists = AppendTable(extent, layouts.ResourceLists);
        environment.ResourceIndices = AppendTable(extent, layouts.ResourceIndices);
        environment.Footprints = AppendTable(extent, layouts.Footprints);
        environment.FootprintLists = AppendTable(extent, layouts.FootprintLists);
        environment.VisibilityLists = AppendTable(extent, layouts.VisibilityLists);
        environment.VisibilityIndices = AppendTable(extent, layouts.VisibilityIndices);
        environment.Rasters = AppendTable(extent, rasters.Rasters);
        environment.VertexInputs = AppendTable(extent, rasters.VertexInputs);
        environment.ColorTargets = AppendTable(extent, rasters.ColorTargets);
        environment.UniformMembers = AppendTable(extent, layouts.UniformMembers);
        // Reserved. The cooker writes no specialization constant yet.
        environment.SpecConstants = AppendTable(extent, std::vector<manifest::SpecializationConstant>{});
        environment.SamplerPairLists = AppendTable(extent, layouts.SamplerPairLists);
        environment.SamplerTexturePairs = AppendTable(extent, layouts.SamplerTexturePairs);

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
                         "[lodestone] manifest holds {} modules and {} profiles, the cook produced {} and {}",
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
                             "[lodestone] manifest names module '{}', but the cook produced '{}'",
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
                             "[lodestone] manifest profile {} names target '{}', but the cook produced '{}'",
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
                         "[lodestone] manifest module {} holds {} variants, the cook produced {}",
                         module.Name,
                         view.Variants().size(),
                         module.Variants.size());
            return CookError::ManifestMissingVariant;
        }

        const CookError defaultsError = CheckAxisDefaults(module, view);
        if (defaultsError != CookError::Success)
        {
            return defaultsError;
        }

        const CookError relevanceError = VerifyAxisRelevance(view);
        if (relevanceError != CookError::Success)
        {
            return relevanceError;
        }

        const manifest::ShaderSourceProvider provider{ view, 0u };
        for (const LibraryVariant& variant : module.Variants)
        {
            const VariantKey key = module.VariantKeys[variant.Index];
            const std::optional<manifest::VariantView> readVariant = view.VariantByKey(key);
            if (!readVariant.has_value())
            {
                std::println(stderr, "[lodestone] manifest holds no variant {} [{}]", variant.Index, variant.Description);
                return CookError::ManifestMissingVariant;
            }

            const CookError keyError = CheckVariantKeyDecode(view, variant, key);
            if (keyError != CookError::Success)
            {
                return keyError;
            }

            const CookError maskError = CheckVariantAxisMask(module, variant, readVariant.value());
            if (maskError != CookError::Success)
            {
                return maskError;
            }

            for (size_t entryPointIndex = 0u; entryPointIndex < module.EntryPoints.size(); ++entryPointIndex)
            {
                const CookError slotError =
                    CheckManifestSlot(module, view, provider, variant, readVariant.value(), entryPointIndex);
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
                         "[lodestone] manifest module {} has {} axes, variant [{}] has {}",
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
            if (!StoredAxisValueMatches(view, axisIndex, digits[axisIndex], axis, binding.Value))
            {
                std::println(stderr,
                             "[lodestone] manifest key {} of module {} decodes axis '{}' differently than "
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

    bool StoredAxisValueMatches(const manifest::EnvironmentView& view,
                                uint32_t axis_index,
                                uint32_t digit,
                                const PermutationAxis& axis,
                                const PermutationValue& value)
    {
        const manifest::ModuleView& moduleView = view.Module();
        const AxisValueType stored = moduleView.AxisValue(axis_index, digit);
        if (view.String(moduleView.AxisData(axis_index).NameString) != axis.Name)
        {
            return false;
        }

        switch (axis.ValueDomain)
        {
        case AxisValueDomain::Boolean:
            [[fallthrough]];
        case AxisValueDomain::Integral:
            return stored == value.AsUInt();
        case AxisValueDomain::Enum:
            return view.String(stored) == value.AsEnumCase(axis);
        case AxisValueDomain::Type:
            return view.String(stored) == value.AsType(axis);
        case AxisValueDomain::None:
            break;
        }
        std::unreachable();
    }

    CookError CheckAxisDefaults(const CookedModule& module, const manifest::EnvironmentView& view)
    {
        if (module.Space == nullptr)
        {
            return CookError::Success;
        }

        // read each default back through the module digit and the root value table, as a client does
        const std::span<const PermutationAxis> axes = module.Space->Axes();
        for (uint32_t axisIndex = 0u; axisIndex < axes.size(); ++axisIndex)
        {
            const PermutationAxis& axis = axes[axisIndex];
            const uint32_t digit = view.Module().AxisDefaultDigit(axisIndex);
            if (!StoredAxisValueMatches(view, axisIndex, digit, axis, axis.GetDeclaredDefault()))
            {
                std::println(stderr,
                             "[lodestone] manifest module {} reads the default of axis '{}' back as digit {}, which "
                             "is not the declared default",
                             module.Name,
                             axis.Name,
                             digit);
                return CookError::ManifestAxisDefaultMismatch;
            }
        }

        return CookError::Success;
    }

    /** Returns the bytes of one source in the profile's code format. */
    std::span<const std::byte> SourceBytes(const manifest::EnvironmentView& view, uint32_t source_index)
    {
        if (view.CodeFormat() == ShaderCodeFormat::Spirv)
        {
            return std::as_bytes(view.SpirvWords(source_index));
        }
        return std::as_bytes(std::span{ view.Source(source_index) });
    }

    template<typename RecordType>
    bool RecordsEqual(std::span<const RecordType> lhs, std::span<const RecordType> rhs) noexcept
    {
        // manifest records are trivially copyable bags of bytes with zeroed reserved fields, and every string
        // they name is an index into a deduplicated string table, so equal bytes are equal content
        return lhs.size() == rhs.size() && std::memcmp(lhs.data(), rhs.data(), lhs.size_bytes()) == 0;
    }

    /** Compares one resource of two layouts by content. `FirstUniformMember` is an index that `--no-dedupe`
     * makes unique, so the members are compared instead. */
    bool ResourcesEqual(const manifest::EnvironmentView& view,
                        const manifest::ResolvedResource& lhs,
                        const manifest::ResolvedResource& rhs) noexcept
    {
        manifest::Binding lhsBinding = lhs.Record();
        manifest::Binding rhsBinding = rhs.Record();
        lhsBinding.FirstUniformMember = 0u;
        rhsBinding.FirstUniformMember = 0u;
        if (std::memcmp(&lhsBinding, &rhsBinding, sizeof(manifest::Binding)) != 0 ||
            !RecordsEqual(view.UniformMembers(lhs.Record()), view.UniformMembers(rhs.Record())))
        {
            return false;
        }

        const manifest::Footprint* lhsFootprint = lhs.FootprintRecord();
        const manifest::Footprint* rhsFootprint = rhs.FootprintRecord();
        if (lhsFootprint == nullptr || rhsFootprint == nullptr)
        {
            return lhsFootprint == rhsFootprint;
        }

        return std::memcmp(lhsFootprint, rhsFootprint, sizeof(manifest::Footprint)) == 0;
    }

    /** Compares the instance of one entry point in two variants by content: source, layout, raster state, and
     * workgroup size. Equal record indices are a shortcut only: unequal indices still compare content. */
    bool InstancesEqual(const manifest::EnvironmentView& view, uint32_t lhs_variant, uint32_t rhs_variant, uint32_t entry_point)
    {
        const manifest::EntryPointInstanceView lhs = view.VariantAt(lhs_variant).EntryPoint(entry_point);
        const manifest::EntryPointInstanceView rhs = view.VariantAt(rhs_variant).EntryPoint(entry_point);
        const manifest::EntryPointInstance& lhsRecord = lhs.Record();
        const manifest::EntryPointInstance& rhsRecord = rhs.Record();

        if (lhsRecord.WorkgroupX != rhsRecord.WorkgroupX || lhsRecord.WorkgroupY != rhsRecord.WorkgroupY ||
            lhsRecord.WorkgroupZ != rhsRecord.WorkgroupZ)
        {
            return false;
        }

        if (lhsRecord.Raster != rhsRecord.Raster &&
            (!RecordsEqual(view.VertexInputs(lhsRecord.Raster), view.VertexInputs(rhsRecord.Raster)) ||
             !RecordsEqual(view.ColorTargets(lhsRecord.Raster), view.ColorTargets(rhsRecord.Raster)) ||
             view.WritesFragDepth(lhsRecord.Raster) != view.WritesFragDepth(rhsRecord.Raster)))
        {
            return false;
        }

        const manifest::LayoutRange lhsLayout = lhs.Layout();
        const manifest::LayoutRange rhsLayout = rhs.Layout();
        if (lhsLayout.Size() != rhsLayout.Size())
        {
            return false;
        }

        for (uint32_t position = 0u; position < lhsLayout.Size(); ++position)
        {
            if (!ResourcesEqual(view, lhsLayout[position], rhsLayout[position]))
            {
                return false;
            }
        }

        if (!RecordsEqual(lhs.SamplerTexturePairs(), rhs.SamplerTexturePairs()))
        {
            return false;
        }

        return lhsRecord.Source == rhsRecord.Source ||
               std::ranges::equal(SourceBytes(view, lhsRecord.Source), SourceBytes(view, rhsRecord.Source));
    }

    /** Groups the variants into classes that agree on every axis whose bit is set in `kept_axes`, and returns
     * each class as a run of variant indices, in `order`. */
    std::vector<std::span<const uint32_t>> GroupVariants(std::span<const std::vector<uint32_t>> digits,
                                                         std::span<const uint8_t> kept_axes,
                                                         std::vector<uint32_t>& order)
    {
        auto lessOnKept = [&](uint32_t lhs, uint32_t rhs)
        {
            for (size_t axis = 0u; axis < kept_axes.size(); ++axis)
            {
                if (kept_axes[axis] != 0u && digits[lhs][axis] != digits[rhs][axis])
                {
                    return digits[lhs][axis] < digits[rhs][axis];
                }
            }
            return false;
        };

        order.resize(digits.size());
        std::ranges::iota(order, 0u);
        std::ranges::stable_sort(order, lessOnKept);

        std::vector<std::span<const uint32_t>> classes;
        size_t begin = 0u;
        while (begin < order.size())
        {
            size_t end = begin + 1u;
            while (end < order.size() && !lessOnKept(order[begin], order[end]))
            {
                ++end;
            }
            classes.emplace_back(order.data() + begin, end - begin);
            begin = end;
        }
        return classes;
    }

    CookError VerifyAxisRelevance(const manifest::EnvironmentView& view)
    {
        const manifest::ModuleView& module = view.Module();
        const uint32_t axisCount = module.AxisCount();
        const auto entryPointCount = static_cast<uint32_t>(module.EntryPoints().size());
        const auto variantCount = static_cast<uint32_t>(view.Variants().size());

        std::vector<uint32_t> radices(axisCount);
        for (uint32_t axis = 0u; axis < axisCount; ++axis)
        {
            radices[axis] = module.AxisValueCount(axis);
        }

        std::vector<std::vector<uint32_t>> digits(variantCount, std::vector<uint32_t>(axisCount, 0u));
        for (uint32_t variant = 0u; variant < variantCount; ++variant)
        {
            UnpackVariantKey(view.VariantKeys()[variant], radices, digits[variant]);
        }

        auto describe = [&](uint32_t axis) { return view.String(module.AxisData(axis).NameString); };
        auto entryPointName = [&](uint32_t entry_point)
        {
            return view.String(module.EntryPoints()[entry_point].NameString);
        };

        // first pass: an axis is relevant when two variants that differ only in it hold different instances, or
        // when no two variants differ only in it (one cooked value is undetermined)
        std::vector<std::vector<uint8_t>> derived(entryPointCount, std::vector<uint8_t>(axisCount, uint8_t{ 0u }));
        std::vector<uint32_t> order;
        std::vector<uint8_t> allButOne(axisCount, uint8_t{ 1u });
        for (uint32_t axis = 0u; axis < axisCount; ++axis)
        {
            allButOne[axis] = 0u;
            const std::vector<std::span<const uint32_t>> classes = GroupVariants(digits, allButOne, order);
            allButOne[axis] = 1u;
            const bool determined =
                std::ranges::any_of(classes, [](std::span<const uint32_t> group) { return group.size() > 1u; });

            for (uint32_t entryPoint = 0u; entryPoint < entryPointCount; ++entryPoint)
            {
                const bool relevant = !determined || std::ranges::any_of(classes, [&](std::span<const uint32_t> group) {
                    return std::ranges::any_of(group.subspan(1u), [&](uint32_t variant) {
                        return !InstancesEqual(view, group.front(), variant, entryPoint);
                    });
                });
                derived[entryPoint][axis] = relevant ? 1u : 0u;
            }
        }

        // closing pass: in each class of variants that agree on the relevant axes and hold more than one instance,
        // mark every axis that varies within the class, until no class holds more than one instance
        for (uint32_t entryPoint = 0u; entryPoint < entryPointCount; ++entryPoint)
        {
            std::vector<uint8_t>& relevant = derived[entryPoint];
            while (true)
            {
                std::vector<uint8_t> marked(axisCount, uint8_t{ 0u });
                for (const std::span<const uint32_t> group : GroupVariants(digits, relevant, order))
                {
                    const bool oneInstance = std::ranges::all_of(group.subspan(1u), [&](uint32_t variant) {
                        return InstancesEqual(view, group.front(), variant, entryPoint);
                    });
                    if (oneInstance)
                    {
                        continue;
                    }
                    for (uint32_t axis = 0u; axis < axisCount; ++axis)
                    {
                        const bool varies = std::ranges::any_of(group.subspan(1u), [&](uint32_t variant) {
                            return digits[variant][axis] != digits[group.front()][axis];
                        });
                        if (relevant[axis] == 0u && varies)
                        {
                            marked[axis] = 1u;
                        }
                    }
                }

                if (std::ranges::none_of(marked, [](uint8_t bit) { return bit != 0u; }))
                {
                    break;
                }
                for (uint32_t axis = 0u; axis < axisCount; ++axis)
                {
                    relevant[axis] = static_cast<uint8_t>(relevant[axis] | marked[axis]);
                }
            }
        }

        for (uint32_t entryPoint = 0u; entryPoint < entryPointCount; ++entryPoint)
        {
            for (uint32_t axis = 0u; axis < axisCount; ++axis)
            {
                const bool relevant = derived[entryPoint][axis] != 0u;
                if (view.IsAxisRelevant(entryPoint, axis) != relevant)
                {
                    std::println(stderr,
                                 "[lodestone] AXIS RELEVANCE: the manifest stores axis '{}' as {} for entry point '{}', "
                                 "and the written instances say {}",
                                 describe(axis),
                                 relevant ? "irrelevant" : "relevant",
                                 entryPointName(entryPoint),
                                 relevant ? "relevant" : "irrelevant");
                    return CookError::ManifestAxisRelevanceMismatch;
                }
            }
        }

        return CookError::Success;
    }

    CookError CheckVariantAxisMask(const CookedModule& module,
                                   const LibraryVariant& variant,
                                   manifest::VariantView read_variant)
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
            if (read_variant.IsAxisActive(axisIndex) != expected)
            {
                std::println(stderr,
                             "[lodestone] manifest marks axis '{}' {} in variant [{}], the cook did not",
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
        // Read back through the accessor a consumer of this format uses, and compare bytes.
        const std::span<const std::byte> expectedSource = ResolveSource(module, variant, entry_point_index);
        const VariantKey variantKey = module.VariantKeys[variant.Index];
        const uint32_t entryPoint = static_cast<uint32_t>(entry_point_index);
        const std::span<const std::byte> providerBytes =
            (provider.View().CodeFormat() == ShaderCodeFormat::Spirv)
                ? std::as_bytes(provider.SpirvWords(entryPoint, variantKey))
                : std::as_bytes(std::span{ provider.Source(entryPoint, variantKey) });
        if (std::ranges::equal(providerBytes, expectedSource))
        {
            return CookError::Success;
        }

        std::println(stderr,
                     "[lodestone] manifest returns different text for {} variant {} [{}]",
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
                     "[lodestone] manifest returns a different workgroup size for {} variant {}",
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
               record.Access == static_cast<uint8_t>(binding.Access) &&
               record.SampleType == static_cast<uint8_t>(binding.SampleType);
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
                                 readMember.Offset == expectedMember.Data.Offset &&
                                 readMember.Size == expectedMember.Data.Size &&
                                 readMember.ArrayCount == expectedMember.Data.ArrayCount &&
                                 readMember.ElementStride == static_cast<uint32_t>(expectedMember.Data.ElementStride) &&
                                 readMember.MatrixLayout == static_cast<uint32_t>(expectedMember.Data.MatrixLayout);
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
                                  manifest::VariantView read_variant,
                                  size_t entry_point_index)
    {
        const CookResult<ShaderLayoutView> expectedLayoutResult = ResolveLayoutView(module, variant, entry_point_index);
        if (!expectedLayoutResult)
        {
            return expectedLayoutResult.error();
        }
        const ShaderLayoutView& expectedLayout = *expectedLayoutResult;

        if (entry_point_index >= read_variant.EntryPointCount())
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} holds no slot {}",
                         variant.Index,
                         entry_point_index);
            return CookError::ManifestVariantMissingEntryPoint;
        }

        const manifest::LayoutRange layout =
            read_variant.EntryPoint(static_cast<uint32_t>(entry_point_index)).Layout();

        if (layout.Size() != expectedLayout.size())
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} entry point {} sees {} resources, the cook "
                         "produced {}",
                         variant.Index,
                         entry_point_index,
                         layout.Size(),
                         expectedLayout.size());
            return CookError::ManifestVariantResourceVisibilityMismatch;
        }

        size_t position = 0u;
        for (const manifest::ResolvedResource resource : layout)
        {
            const ResolvedBindingView expected = expectedLayout[position];
            const manifest::Footprint* footprint = resource.FootprintRecord();

            if (footprint == nullptr)
            {
                std::println(stderr,
                             "[lodestone] manifest variant {} resolves resource {} out of range",
                             variant.Index,
                             position);
                return CookError::ManifestVariantResourceResolveOutOfRange;
            }

            const manifest::Binding& read = resource.Record();

            if (resource.Name() != expected.Resource->Name ||
                resource.ScopeName() != expected.Resource->ScopeName ||
                !RecordMatchesBinding(read, *expected.Resource) ||
                !RecordMatchesFootprint(*footprint, *expected.Footprint) ||
                !ManifestUniformMembersMatch(view, read, *expected.Resource))
            {
                std::println(stderr,
                             "[lodestone] manifest binding '{}' of variant {} does not match the cook",
                             expected.Resource->Name,
                             variant.Index);
                return CookError::ManifestVariantResourceBindingMismatch;
            }

            ++position;
        }

        // the pairs are positions in the visibility list, which the walk above proved equal, so the positions
        // compare directly
        const SamplerPairList& expectedPairs =
            module.SamplerPairLists[variant.SamplerPairIndices[entry_point_index]];
        const std::span<const manifest::SamplerTexturePair> readPairs =
            read_variant.EntryPoint(static_cast<uint32_t>(entry_point_index)).SamplerTexturePairs();
        const bool pairsMatch = std::ranges::equal(readPairs, expectedPairs, [](const manifest::SamplerTexturePair& read, const SamplerTexturePair& expected) {
            return read.Texture == expected.Texture && read.Sampler == expected.Sampler;
        });
        if (!pairsMatch)
        {
            std::println(stderr,
                         "[lodestone] manifest variant {} entry point {} reads {} sampler and texture pairs, the cook "
                         "produced {}, or one pair differs",
                         variant.Index,
                         entry_point_index,
                         readPairs.size(),
                         expectedPairs.size());
            return CookError::ManifestSamplerPairMismatch;
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
                             "[lodestone] manifest vertex input '{}' does not match the cook",
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
                             "[lodestone] manifest color target {} does not match the cook",
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
                         "[lodestone] manifest raster state {} does not match the cook for {}",
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
                                manifest::VariantView read_variant,
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

        const CookError layoutError = CheckManifestLayout(module, view, variant, read_variant, entry_point_index);
        if (layoutError != CookError::Success)
        {
            return layoutError;
        }

        return CheckManifestRaster(module, view, variant, entry_point_index);
    }

} // namespace

} // namespace lodestone
