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
        std::vector<uint32_t> ResourceTags;
        std::vector<manifest::Binding> Bindings;
        std::vector<uint32_t> ResourceIndices;
        std::vector<manifest::Run> ResourceLists;
        std::vector<manifest::Footprint> Footprints;
        std::vector<manifest::Run> FootprintLists;
        std::vector<uint32_t> VisibilityIndices;
        std::vector<manifest::Run> VisibilityLists;
        std::vector<manifest::SamplerTexturePair> SamplerTexturePairs;
        std::vector<manifest::Run> SamplerPairLists;
        std::vector<manifest::EntryPointConstants> EntryPointConstants;
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
    manifest::EntryPointConstants MakeEntryPointConstantsRecord(const EntryPointConstants& constants,
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

    /** Appends one record for each member to `member_records`, and returns the index of the first. */
    uint32_t AppendMemberRecords(std::span<const ReflectedUniformMember> members,
                                 StringTableBuilder& strings,
                                 std::vector<manifest::UniformMember>& member_records)
    {
        const auto first = static_cast<uint32_t>(member_records.size());
        member_records.reserve(member_records.size() + members.size());
        for (const ReflectedUniformMember& member : members)
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
        return first;
    }

    manifest::Binding MakeBindingRecord(const ReflectedBinding& binding,
                                        StringTableBuilder& strings,
                                        std::vector<manifest::UniformMember>& member_records,
                                        std::vector<uint32_t>& tag_records)
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

        record.FirstUniformMember = AppendMemberRecords(binding.UniformMembers, strings, member_records);
        record.UniformMemberCount = static_cast<uint32_t>(binding.UniformMembers.size());
        record.Roles = std::to_underlying(binding.Roles);
        record.FirstTag = static_cast<uint32_t>(tag_records.size());
        record.TagCount = static_cast<uint32_t>(binding.Tags.size());
        for (const std::string& tag : binding.Tags)
        {
            tag_records.push_back(strings.Add(tag));
        }
        return record;
    }

    manifest::EntryPointConstants MakeEntryPointConstantsRecord(const EntryPointConstants& constants,
                                                                StringTableBuilder& strings,
                                                                std::vector<manifest::UniformMember>& member_records)
    {
        manifest::EntryPointConstants record;
        record.ByteSize = constants.ByteSize;
        record.Binding = constants.Binding;
        record.FirstUniformMember = AppendMemberRecords(constants.Members, strings, member_records);
        record.UniformMemberCount = static_cast<uint32_t>(constants.Members.size());
        record.Delivery = static_cast<uint8_t>(constants.Delivery);
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
        slot.Constants = variant.ConstantsIndices[entry_point_index];
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
            tables.Bindings.push_back(MakeBindingRecord(resource, strings, tables.UniformMembers, tables.ResourceTags));
        }
        // todo-ship: Make this into a move, and make footprint record stop using a variant. Change values
        // to be stored not in a variant, but in a dedicated footprint record structure that we can just copy
        // already. Use sentinel values to derive type
        AppendRuns(module.ResourceLists, tables.ResourceIndices, tables.ResourceLists, &PassThroughIndex);
        AppendRuns(module.FootprintLists, tables.Footprints, tables.FootprintLists, &MakeFootprintRecord);
        AppendRuns(
            module.VisibilityLists, tables.VisibilityIndices, tables.VisibilityLists, &PassThroughIndex);
        AppendRuns(module.SamplerPairLists, tables.SamplerTexturePairs, tables.SamplerPairLists, &MakeSamplerPairRecord);

        tables.EntryPointConstants.reserve(module.EntryPointConstantsTable.size());
        for (const EntryPointConstants& constants : module.EntryPointConstantsTable)
        {
            tables.EntryPointConstants.push_back(
                MakeEntryPointConstantsRecord(constants, strings, tables.UniformMembers));
        }

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
        environment.EntryPointConstants = AppendTable(extent, layouts.EntryPointConstants);
        environment.ResourceTags = AppendTable(extent, layouts.ResourceTags);

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
} // namespace

} // namespace lodestone
