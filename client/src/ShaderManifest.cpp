#include "ShaderManifest.hpp"
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestLayout.hpp"
#include "ShaderManifestRead.hpp"
#include "ShaderManifestValidator.hpp"
#include "EnumClassUtils.hpp"
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
#include <iterator>
#include <magic_enum/magic_enum.hpp>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone::manifest
{

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif


using detail::k_AxisMaskWordBits;
using detail::k_ManifestOk;
using detail::Map;
using detail::MaskWordCountForAxes;
using detail::RunOf;
using detail::SelectSetBit;

namespace
{

    // to_array, so that a short list cannot pad itself and silence the static_assert below
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
            "a visibility list is not in placement order: Bound resources first, in strictly ascending (group, binding) order",
            "a merged layout got more entry point instances than MergedLayoutView::k_MaxPipelineStages",
            "a merged layout got two entry point instances of the same stage",
            "a merged layout got a resource that is not Bound",
            "a merged layout got two different resources at one group and binding",
            "a module axis declares a default digit that is not below the axis radix",
            "an entry point's axis relevance mask sets a bit past the module's axes",
            "a sampler pair list runs past the sampler and texture pair table",
            "an entry point instance names a sampler pair list past the list table",
            "a sampler and texture pair names a position past the visibility list, a binding of the wrong kind, or is not sorted and unique",
            "an entry point instance names an entry point constants record past the table",
            "an entry point constants record has an unknown delivery, a member run past the member table, a position on a push constant block, data with no delivery, or a uniform buffer position that does not name a uniform buffer of its size",
            "a merged layout got two push constant blocks whose members differ where the blocks overlap",
            "a binding sets a role bit that ResourceRoleFlags does not define",
            "a binding's tag run is outside the tag table, names a string past the string table, or repeats a tag",
            "a binding's footprint axis mask sets a bit past the module's axes",
            "a capability name is past the string table, or the names are not sorted and unique",
            "a source's capability mask sets a bit past the capability name table",
        });

    static_assert(k_ErrorDescriptions.size() == static_cast<size_t>(ErrorCode::Count),
                  "every ErrorCode needs a description; add a line when you add a code");

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
    case ErrorCode::EntryPointInstanceGridSizeMismatch:
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

    const ErrorState bundleValid = detail::ValidateBundle(parsed, bytes);
    if (!bundleValid)
    {
        return std::unexpected(bundleValid);
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
    // the caller supplies the indices, so this is an ingestion surface
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

ManifestResult<const EntryPoint*> ModuleView::FindEntryPoint(std::string_view name) const noexcept
{
    auto findFn = [&](const EntryPoint& entryPoint) -> bool
    {
        return bundle.String(entryPoint.NameString) == name;
    };

    auto findIter = std::ranges::find_if(entryPoints, findFn);
    if (findIter != entryPoints.end())
    {
        return std::to_address(findIter);
    }
    
    return std::unexpected(ErrorState{ .Code = ErrorCode::EntryPointInvalidName,
                                       .Table = ShaderManifestTable::EntryPoints,
                                       .RecordIndex = moduleIndex,
                                       .Detail = 0 });
}

ManifestResult<uint32_t> ModuleView::FindEntryPointIndex(std::string_view name) const noexcept
{
    auto findFn = [&](const EntryPoint& entryPoint) -> bool
    {
        return bundle.String(entryPoint.NameString) == name;
    };

    auto findIter = std::ranges::find_if(entryPoints, findFn);
    if (findIter != entryPoints.end())
    {
        const std::ptrdiff_t index = std::distance(entryPoints.begin(), findIter);
        return static_cast<uint32_t>(index);
    }

    return std::unexpected(ErrorState{ .Code = ErrorCode::EntryPointInvalidName,
                                       .Table = ShaderManifestTable::EntryPoints,
                                       .RecordIndex = moduleIndex,
                                       .Detail = 0 });
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

uint32_t ModuleView::AxisDefaultDigit(uint32_t local_axis) const noexcept
{
    return moduleAxes[local_axis].DefaultDigit;
}

AxisValueType ModuleView::AxisValue(uint32_t local_axis, uint32_t digit) const noexcept
{
    const ModuleAxis& moduleAxis = moduleAxes[local_axis];
    const uint32_t rootValueIndex = SelectSetBit(moduleAxis.LiveValuesMask, digit);
    std::span<const AxisValueType> axisValues = bundle.AxisValues(moduleAxis.AxisIndex);
    return axisValues[rootValueIndex];
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
    const ErrorState extentValid = detail::OpenEnvironmentExtent(bundle, profile_index, module_index, extent_bytes);
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

    const detail::EnvironmentContext context{ .Environment = parsed,
                                      .Extent = extent_bytes,
                                      .StringCount = bundle.StringCount(),
                                      .EntryPointCount = view.module.EntryPoints().size(),
                                      .AxisMaskWordCount = view.module.AxisMaskWordCount(),
                                      .ModuleAxisCount = view.module.AxisCount(),
                                      .CodeFormat = bundle.Profiles()[profile_index].CodeFormat };

    const ErrorState environmentValid = detail::ValidateEnvironment(context);
    if (!environmentValid)
    {
        return std::unexpected(environmentValid);
    }

    const uint64_t slotCount = static_cast<uint64_t>(parsed.VariantCount) * context.EntryPointCount;
    const uint64_t maskWordCount = static_cast<uint64_t>(parsed.VariantCount) * context.AxisMaskWordCount;
    view.variantKeys = Map<VariantKey>(extent_bytes, TableRef{ .Offset = parsed.VariantKeyTableOffset, .Count = parsed.VariantCount });
    view.variants = Map<Variant>(extent_bytes, TableRef{ .Offset = parsed.VariantTableOffset, .Count = parsed.VariantCount });
    const TableRef64 axisMaskTableRef{ .Offset = static_cast<uint64_t>(parsed.AxisMaskTableOffset), .Count = maskWordCount };
    view.axisMasks = Map<uint64_t>(extent_bytes, axisMaskTableRef);
    const TableRef64 axisRelevanceTableRef{ .Offset = static_cast<uint64_t>(parsed.AxisRelevanceTableOffset),
                                            .Count = context.EntryPointCount * context.AxisMaskWordCount };
    view.axisRelevance = Map<uint64_t>(extent_bytes, axisRelevanceTableRef);
    const TableRef64 footprintAxisTableRef{ .Offset = static_cast<uint64_t>(parsed.FootprintAxisTableOffset),
                                            .Count = static_cast<uint64_t>(parsed.Bindings.Count) *
                                                     context.AxisMaskWordCount };
    view.footprintAxes = Map<uint64_t>(extent_bytes, footprintAxisTableRef);
    view.capabilityNames = Map<uint32_t>(extent_bytes, parsed.CapabilityNames);
    const TableRef64 sourceCapabilityTableRef{ .Offset = static_cast<uint64_t>(parsed.SourceCapabilityTableOffset),
                                               .Count = static_cast<uint64_t>(parsed.Sources.Count) *
                                                        MaskWordCountForAxes(parsed.CapabilityNames.Count) };
    view.sourceCapabilities = Map<uint64_t>(extent_bytes, sourceCapabilityTableRef);
    const TableRef64 entryPointInstanceTableRef{
        .Offset = static_cast<uint64_t>(parsed.EntryPointInstanceTableOffset), .Count = slotCount };
    view.entryPointInstances = Map<EntryPointInstance>(extent_bytes, entryPointInstanceTableRef);
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
    view.resourceTags = Map<uint32_t>(extent_bytes, parsed.ResourceTags);
    view.specializationConstants = Map<SpecializationConstant>(extent_bytes, parsed.SpecConstants);
    view.samplerPairLists = Map<Run>(extent_bytes, parsed.SamplerPairLists);
    view.samplerTexturePairs = Map<SamplerTexturePair>(extent_bytes, parsed.SamplerTexturePairs);
    view.entryPointConstants = Map<EntryPointConstants>(extent_bytes, parsed.EntryPointConstants);
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
    // the key table is parallel to the variant table
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

std::span<const uint64_t> EnvironmentView::AxisRelevance(uint32_t entry_point_index) const noexcept
{
    const size_t wordCount = module.AxisMaskWordCount();
    return axisRelevance.subspan(static_cast<size_t>(entry_point_index) * wordCount, wordCount);
}

bool EnvironmentView::IsAxisRelevant(uint32_t entry_point_index, uint32_t local_axis) const noexcept
{
    const std::span<const uint64_t> mask = AxisRelevance(entry_point_index);
    const uint64_t word = mask[local_axis / k_AxisMaskWordBits];
    return ((word >> (local_axis % k_AxisMaskWordBits)) & 1u) != 0u;
}

std::span<const uint64_t> EnvironmentView::FootprintAxes(uint32_t binding_index) const noexcept
{
    const size_t wordCount = module.AxisMaskWordCount();
    return footprintAxes.subspan(static_cast<size_t>(binding_index) * wordCount, wordCount);
}

bool EnvironmentView::IsFootprintAxis(uint32_t binding_index, uint32_t local_axis) const noexcept
{
    const std::span<const uint64_t> mask = FootprintAxes(binding_index);
    const uint64_t word = mask[local_axis / k_AxisMaskWordBits];
    return ((word >> (local_axis % k_AxisMaskWordBits)) & 1u) != 0u;
}

std::span<const uint32_t> EnvironmentView::CapabilityNames() const noexcept
{
    return capabilityNames;
}

std::string_view EnvironmentView::CapabilityName(uint32_t capability_index) const noexcept
{
    return String(capabilityNames[capability_index]);
}

uint32_t EnvironmentView::CapabilityMaskWordCount() const noexcept
{
    return MaskWordCountForAxes(capabilityNames.size());
}

std::span<const uint64_t> EnvironmentView::SourceCapabilities(uint32_t source_index) const noexcept
{
    const size_t wordCount = CapabilityMaskWordCount();
    return sourceCapabilities.subspan(static_cast<size_t>(source_index) * wordCount, wordCount);
}

bool EnvironmentView::SourceNeedsCapability(uint32_t source_index, uint32_t capability_index) const noexcept
{
    const std::span<const uint64_t> mask = SourceCapabilities(source_index);
    const uint64_t word = mask[capability_index / k_AxisMaskWordBits];
    return ((word >> (capability_index % k_AxisMaskWordBits)) & 1u) != 0u;
}

uint32_t EnvironmentView::SourceWorkgroupMemoryBytes(uint32_t source_index) const noexcept
{
    return sources[source_index].WorkgroupMemoryBytes;
}

std::span<const EntryPointInstance> EnvironmentView::EntryPointInstanceTable() const noexcept
{
    return entryPointInstances;
}

std::span<const EntryPointInstance> EnvironmentView::VariantEntryPoints(uint32_t variant_index) const noexcept
{
    const size_t entryPointCount = module.EntryPoints().size();
    return entryPointInstances.subspan(static_cast<size_t>(variant_index) * entryPointCount, entryPointCount);
}

const EntryPointInstance* EnvironmentView::FindEntryPointInstance(uint32_t entry_point, VariantKey variant_key) const noexcept
{
    const std::optional<uint32_t> variantIndex = FindVariant(variant_key);
    if (!variantIndex.has_value()) [[unlikely]]
    {
        return nullptr;
    }
    std::span<const EntryPointInstance> variantEPs = VariantEntryPoints(variantIndex.value());
    return &variantEPs[entry_point];
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

std::span<const uint32_t> EnvironmentView::ResourceTags(const Binding& binding) const noexcept
{
    return resourceTags.subspan(binding.FirstTag, binding.TagCount);
}

std::span<const UniformMember> EnvironmentView::UniformMembers(const EntryPointConstants& constants) const noexcept
{
    return uniformMembers.subspan(constants.FirstUniformMember, constants.UniformMemberCount);
}

std::span<const EntryPointConstants> EnvironmentView::EntryPointConstantsTable() const noexcept
{
    return entryPointConstants;
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

std::span<const SamplerTexturePair> EnvironmentView::SamplerPairList(uint32_t list_index) const noexcept
{
    return RunOf(samplerPairLists, samplerTexturePairs, list_index);
}

bool EnvironmentView::HasSamplerTexturePairs() const noexcept
{
    constexpr uint16_t k_Recorded = std::to_underlying(ProfileFlags::SamplerTexturePairsRecorded);
    return (ProfileRecord().Flags & k_Recorded) != 0u;
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
    const EntryPointInstance* slot = view.FindEntryPointInstance(entry_point, variant);
    assert(slot != nullptr);
    return view.Source(slot->Source);
}

std::span<const uint32_t> ShaderSourceProvider::SpirvWords(uint32_t entry_point,
                                                           VariantKey variant) const noexcept
{
    const EntryPointInstance* slot = view.FindEntryPointInstance(entry_point, variant);
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
    const EntryPointInstance* slot = view.FindEntryPointInstance(entry_point, variant);
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

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone::manifest
