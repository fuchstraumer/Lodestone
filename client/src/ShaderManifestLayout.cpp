#include "ShaderManifestLayout.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestRead.hpp"
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
#include <expected>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>

namespace lodestone::manifest
{

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif

using detail::BoundPlacementKey;
using detail::k_AxisMaskWordBits;
using detail::RunOf;

namespace
{
    bool SamePairPlacement(const LayoutRange& lhs_layout,
                           const SamplerTexturePair& lhs,
                           const LayoutRange& rhs_layout,
                           const SamplerTexturePair& rhs) noexcept;
    // compare by text, because the two resources can come from two environments
    bool TagsEqual(const ResolvedResource& lhs, const ResolvedResource& rhs) noexcept;
    CreationSource SourceOfFootprint(const ResolvedResource& resource, bool stated) noexcept;
    void DescribeTexture(const ResolvedResource& resource, CreationFields& fields) noexcept;
} // namespace

static_assert(std::input_iterator<LayoutRange::Iterator>);
static_assert(std::ranges::input_range<LayoutRange>);
static_assert(std::input_iterator<UniformMemberRange::Iterator>);
static_assert(std::ranges::input_range<UniformMemberRange>);
static_assert(std::input_iterator<MergedLayoutView::Iterator>);
static_assert(std::input_iterator<MergedLayoutView::PairIterator>);
static_assert(std::ranges::input_range<MergedLayoutView>);

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

std::optional<UniformMemberView> UniformMemberRange::Find(std::string_view path) const noexcept
{
    for (uint32_t position = 0u; position < Size(); ++position)
    {
        const UniformMemberView member = (*this)[position];
        if (member.Name() == path)
        {
            return member;
        }
    }
    
    return std::nullopt;
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

ResourceRoleFlags ResolvedResource::Roles() const noexcept
{
    return static_cast<ResourceRoleFlags>(record->Roles);
}

uint32_t ResolvedResource::TagCount() const noexcept
{
    return record->TagCount;
}

std::string_view ResolvedResource::Tag(uint32_t position) const noexcept
{
    const std::span<const uint32_t> tagIndices = environment->ResourceTags(*record);
    return environment->String(tagIndices[position]);
}

bool ResolvedResource::HasTag(std::string_view tag) const noexcept
{
    const std::span<const uint32_t> tagIndices = environment->ResourceTags(*record);
    auto checkStrIndex = [this, tag](uint32_t string_idx)->bool
    {
        return environment->String(string_idx) == tag;
    };
    return std::ranges::any_of(tagIndices, checkStrIndex);
}

bool CreationFields::IsComplete() const noexcept
{
    const std::array<CreationSource, 9u> sources{ Size,        Usage,     Dimension,   Format,      Extent,
                                                  ArrayLayers, MipLevels, SampleCount, SamplerState };
    return std::ranges::none_of(sources, [](CreationSource source) { return source == CreationSource::Cpu; });
}

CreationFields DescribeCreation(const ResolvedResource& resource) noexcept
{
    CreationFields fields;
    switch (resource.Kind())
    {
    case BindingKind::UniformBuffer:
        fields.Size = CreationSource::Shader;
        fields.Usage = CreationSource::Shader;
        break;
    case BindingKind::StorageBuffer:
        fields.Size = SourceOfFootprint(resource, resource.ElementCount() != 0u);
        fields.Usage = CreationSource::Shader;
        break;
    case BindingKind::StorageTexture:
        DescribeTexture(resource, fields);
        fields.Format = resource.StorageFormat() != TextureFormat::Invalid ? CreationSource::Shader : CreationSource::Cpu;
        break;
    case BindingKind::Texture:
        DescribeTexture(resource, fields);
        fields.Format = CreationSource::Cpu;
        break;
    case BindingKind::CombinedTextureSampler:
        DescribeTexture(resource, fields);
        fields.Format = CreationSource::Cpu;
        fields.SamplerState = CreationSource::Cpu;
        break;
    case BindingKind::Sampler:
        fields.SamplerState = CreationSource::Cpu;
        break;
    default:
        break;
    }
    return fields;
}

std::span<const uint64_t> ResolvedResource::FootprintAxes() const noexcept
{
    return environment->FootprintAxes(Index());
}

bool ResolvedResource::IsFootprintAxis(uint32_t local_axis) const noexcept
{
    return environment->IsFootprintAxis(Index(), local_axis);
}

BufferUsageFlags ResolvedResource::RequiredBufferUsage() const noexcept
{
    switch (Kind())
    {
    case BindingKind::UniformBuffer:
        return BufferUsageFlags::Uniform | BufferUsageOfRoles(Roles());
    case BindingKind::StorageBuffer:
        return BufferUsageFlags::Storage | BufferUsageOfRoles(Roles());
    default:
        return BufferUsageFlags::Invalid;
    }
}

uint32_t ResolvedResource::Index() const noexcept
{

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

bool ResolvedResource::operator==(const ResolvedResource& rhs) const noexcept
{
    return Kind() == rhs.Kind() &&
           Shape() == rhs.Shape() &&
           Access() == rhs.Access() &&
           StorageFormat() == rhs.StorageFormat() &&
           SampleType() == rhs.SampleType() &&
           IsComparisonSampler() == rhs.IsComparisonSampler() &&
           ArrayCount() == rhs.ArrayCount() &&
           ByteSize() == rhs.ByteSize() &&
           Roles() == rhs.Roles() &&
           TagsEqual(*this, rhs);
}

bool ResolvedResource::operator!=(const ResolvedResource& rhs) const noexcept
{
    return !(*this == rhs);
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
    // the only place that resolves visibility -> resource list -> footprint
    const uint32_t local = visible[position];
    const Footprint* footprint = local < footprints.size() ? &footprints[local] : nullptr;
    return ResolvedResource{ environment, &environment->BindingRecord(resources[local]), footprint };
}

EntryPointInstanceView::EntryPointInstanceView(const EnvironmentView* _environment,
                                               const EntryPointInstance* _entry_point_instance,
                                               const Variant* _variant,
                                               uint32_t _entry_point) noexcept
    : environment{ _environment },
      entryPointInstance{ _entry_point_instance },
      variant{ _variant },
      entryPoint{ _entry_point }
{
}

ShaderStageKind EntryPointInstanceView::Stage() const noexcept
{
    const std::span<const EntryPoint> entryPoints = environment->Module().EntryPoints();
    return static_cast<ShaderStageKind>(entryPoints[entryPoint].Stage);
}

std::string_view EntryPointInstanceView::Source() const noexcept
{
    return environment->Source(entryPointInstance->Source);
}

std::span<const uint32_t> EntryPointInstanceView::SpirvWords() const noexcept
{
    return environment->SpirvWords(entryPointInstance->Source);
}

WorkgroupSize EntryPointInstanceView::Workgroup() const noexcept
{
    return WorkgroupSize{
        .X = entryPointInstance->WorkgroupX,
        .Y = entryPointInstance->WorkgroupY,
        .Z = entryPointInstance->WorkgroupZ };
}

uint32_t EntryPointInstanceView::WorkgroupMemoryBytes() const noexcept
{
    return environment->SourceWorkgroupMemoryBytes(entryPointInstance->Source);
}

std::span<const uint64_t> EntryPointInstanceView::RequiredCapabilities() const noexcept
{
    return environment->SourceCapabilities(entryPointInstance->Source);
}

DeviceFit EntryPointInstanceView::Fit(std::span<const uint64_t> supported, const DeviceLimits& limits) const noexcept
{
    const std::span<const uint64_t> required = RequiredCapabilities();
    assert(supported.size() == required.size());
    for (size_t word = 0u; word < required.size(); ++word)
    {
        const uint64_t missing = required[word] & ~supported[word];
        if (missing != 0u)
        {
            const uint32_t bit = static_cast<uint32_t>(std::countr_zero(missing));
            return DeviceFit{ .Reason = DeviceMismatch::MissingCapability,
                              .Detail = static_cast<uint32_t>(word * k_AxisMaskWordBits) + bit };
        }
    }

    if (WorkgroupMemoryBytes() > limits.MaxWorkgroupMemoryBytes)
    {
        return DeviceFit{ .Reason = DeviceMismatch::WorkgroupMemory, .Detail = WorkgroupMemoryBytes() };
    }

    if (Stage() == ShaderStageKind::Compute)
    {
        const WorkgroupSize size = Workgroup();
        const uint64_t invocations = uint64_t{ size.X } * size.Y * size.Z;
        if (invocations > limits.MaxInvocationsPerWorkgroup)
        {
            return DeviceFit{ .Reason = DeviceMismatch::Invocations,
                              .Detail = static_cast<uint32_t>(std::min<uint64_t>(invocations, UINT32_MAX)) };
        }

        if (size.X > limits.MaxWorkgroupSizeX || size.Y > limits.MaxWorkgroupSizeY || size.Z > limits.MaxWorkgroupSizeZ)
        {
            const uint32_t side = size.X > limits.MaxWorkgroupSizeX ? size.X : (size.Y > limits.MaxWorkgroupSizeY ? size.Y : size.Z);
            return DeviceFit{ .Reason = DeviceMismatch::WorkgroupSize, .Detail = side };
        }
    }

    const EntryPointConstantsView constants = Constants();
    if (constants.Delivery() == EntryPointConstantsDelivery::PushConstant && constants.ByteSize() > limits.MaxPushConstantBytes)
    {
        return DeviceFit{ .Reason = DeviceMismatch::PushConstants, .Detail = constants.ByteSize() };
    }

    return DeviceFit{};
}

bool EntryPointInstanceView::RequiresCapability(uint32_t capability_index) const noexcept
{
    return environment->SourceNeedsCapability(entryPointInstance->Source, capability_index);
}

LayoutRange EntryPointInstanceView::Layout() const noexcept
{
    return LayoutRange{ environment,
                        environment->VisibilityList(entryPointInstance->Visibility),
                        environment->ResourceList(variant->ResourceList),
                        environment->FootprintList(variant->FootprintList) };
}

std::span<const SamplerTexturePair> EntryPointInstanceView::SamplerTexturePairs() const noexcept
{
    return environment->SamplerPairList(entryPointInstance->SamplerPairs);
}

EntryPointConstantsView EntryPointInstanceView::Constants() const noexcept
{
    return EntryPointConstantsView{ environment,
                                    &environment->EntryPointConstantsTable()[entryPointInstance->Constants] };
}

EntryPointConstantsView::EntryPointConstantsView(const EnvironmentView* _environment,
                                                 const EntryPointConstants* _record) noexcept
    : environment{ _environment },
      record{ _record }
{
}

EntryPointConstantsDelivery EntryPointConstantsView::Delivery() const noexcept
{
    return static_cast<EntryPointConstantsDelivery>(record->Delivery);
}

uint32_t EntryPointConstantsView::ByteSize() const noexcept
{
    return record->ByteSize;
}

UniformMemberRange EntryPointConstantsView::Members() const noexcept
{
    return UniformMemberRange{ environment, environment->UniformMembers(*record) };
}

uint32_t EntryPointConstantsView::BindingPosition() const noexcept
{
    return record->Binding;
}

const EntryPointConstants& EntryPointConstantsView::Record() const noexcept
{
    return *record;
}

uint32_t EntryPointInstanceView::Raster() const noexcept
{
    return entryPointInstance->Raster;
}

const EntryPointInstance& EntryPointInstanceView::Record() const noexcept
{
    return *entryPointInstance;
}

const EnvironmentView& EntryPointInstanceView::Environment() const noexcept
{
    return *environment;
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
    const std::span<const EntryPointInstance> entryPointInstances = environment->VariantEntryPoints(index);
    return EntryPointInstanceView{ environment, &entryPointInstances[entry_point], &Record(), entry_point };
}

const Variant& VariantView::Record() const noexcept
{
    const std::span<const Variant> variants = environment->Variants();
    return variants[index];
}

namespace
{
    constexpr uint64_t k_NoPlacement = std::numeric_limits<uint64_t>::max();

    /** The next key of the N-way merge over the stages' sorted layout ranges. */
    uint64_t NextPlacement(std::span<const LayoutRange> ranges,
                           std::span<const uint32_t> stage_layout_positions) noexcept
    {
        uint64_t next = k_NoPlacement;
        for (const auto&& [i, range] : std::views::enumerate(ranges))
        {
            if (stage_layout_positions[i] >= range.Size())
            {
                continue;
            }

            const ResolvedResource& resource = range[stage_layout_positions[i]];
            next = std::min(next, BoundPlacementKey(resource.PlacementValue()));
        }
        
        return next;
    }

    /** Writes the stages whose current position is at `key` into `out_stages`, and returns their count. */
    uint32_t StagesAtPlacement(std::span<const LayoutRange> ranges,
                               std::span<const uint32_t> stage_layout_positions,
                               uint64_t key,
                               std::span<uint32_t> out_stages) noexcept
    {
        uint32_t count = 0u;
        for (const auto&& [i, range] : std::views::enumerate(ranges))
        {
            if (stage_layout_positions[i] >= range.Size())
            {
                continue;
            }

            const ResolvedResource& resource = range[stage_layout_positions[i]];
            if (BoundPlacementKey(resource.PlacementValue()) == key)
            {
                out_stages[count] = static_cast<uint32_t>(i);
                ++count;
            }

        }

        return count;
    }

    /** Compares the fields of a layout entry. Names are not compared. */
    bool SameLayoutBinding(const ResolvedResource& lhs, const ResolvedResource& rhs) noexcept
    {
        if (&lhs.Record() == &rhs.Record())
        {
            return true;
        }
        return lhs == rhs;
    }

    // the blocks overlap from offset 0, so the shorter member list must be a prefix of the longer one
    bool SharePushConstantPrefix(const EntryPointConstantsView& lhs, const EntryPointConstantsView& rhs) noexcept
    {
        const UniformMemberRange lhsMembers = lhs.Members();
        const UniformMemberRange rhsMembers = rhs.Members();
        const uint32_t sharedCount = std::min(lhsMembers.Size(), rhsMembers.Size());
        for (uint32_t i = 0u; i < sharedCount; ++i)
        {
            const UniformMemberView left = lhsMembers[i];
            const UniformMemberView right = rhsMembers[i];
            if (left.Offset() != right.Offset() ||
                left.Size() != right.Size() ||
                left.ArrayCount() != right.ArrayCount() ||
                left.ElementStride() != right.ElementStride() ||
                left.Layout() != right.Layout())
            {
                return false;
            }
        }
        return true;
    }

} // namespace

ManifestResult<MergedLayoutView> MergedLayoutView::Merge(std::span<const EntryPointInstanceView> entry_points) noexcept
{
    if (entry_points.size() > k_MaxPipelineStages)
    {
        return std::unexpected(ErrorState{ .Code = ErrorCode::MergedLayoutTooManyStages,
                                           .Detail = static_cast<uint32_t>(entry_points.size()) });
    }

    MergedLayoutView view;
    view.stageCount = static_cast<uint32_t>(entry_points.size());

    ShaderStageKind seenStages = ShaderStageKind::Invalid;
    for (int32_t i = 0u; std::cmp_less(i, view.stageCount); ++i)
    {
        const EntryPointInstanceView& instance = entry_points[i];
        const ShaderStageKind stage = instance.Stage();
        if (HasAnyFlag(seenStages, stage))
        {
            return std::unexpected(ErrorState{ .Code = ErrorCode::MergedLayoutDuplicateStage,
                                               .Table = ShaderManifestTable::EntryPoints,
                                               .Detail = static_cast<uint32_t>(i) });
        }
        seenStages |= stage;

        view.shaderStages[i] = stage;
        view.layoutRanges[i] = instance.Layout();
        view.pairLists[i] = instance.SamplerTexturePairs();
        view.pairsRecorded = view.pairsRecorded && instance.Environment().HasSamplerTexturePairs();

        const EntryPointConstantsView constants = instance.Constants();
        if (constants.Delivery() == EntryPointConstantsDelivery::PushConstant)
        {
            if (!view.pushConstants.has_value())
            {
                view.pushConstants = MergedPushConstants{ .Constants = constants, .Stages = stage };
            }
            else
            {

                if (!SharePushConstantPrefix(view.pushConstants->Constants, constants))
                {
                    return std::unexpected(ErrorState{ .Code = ErrorCode::MergedLayoutPushConstantConflict,
                                                       .Table = ShaderManifestTable::EntryPointConstants,
                                                       .RecordIndex = instance.Record().Constants,
                                                       .Detail = static_cast<uint32_t>(i) });
                }

                if (constants.ByteSize() > view.pushConstants->Constants.ByteSize())
                {
                    view.pushConstants->Constants = constants;
                }

                view.pushConstants->Stages |= stage;
            }
        }

    }

    // the iterator's walk with no output, so that iteration afterwards cannot fail
    const std::span<const LayoutRange> ranges = std::span{ view.layoutRanges }.first(view.stageCount);
    std::array<uint32_t, k_MaxPipelineStages> stageLayoutPositionsArray;
    stageLayoutPositionsArray.fill(0u);
    const std::span<uint32_t> stageLayoutPositions = std::span{ stageLayoutPositionsArray }.first(view.stageCount);

    std::array<uint32_t, k_MaxPipelineStages> stagesAtKey{};

    for (uint64_t key = NextPlacement(ranges, stageLayoutPositions); key != k_NoPlacement;
         key = NextPlacement(ranges, stageLayoutPositions))
    {
        const uint32_t matchCount = StagesAtPlacement(ranges, stageLayoutPositions, key, stagesAtKey);
        const uint32_t firstStage = stagesAtKey[0];
        const LayoutRange& firstRange = ranges[firstStage];
        const ResolvedResource firstResource = firstRange[stageLayoutPositions[firstStage]];
        for (uint32_t match = 1u; match < matchCount; ++match)
        {
            const uint32_t stage = stagesAtKey[match];
            const LayoutRange& range = ranges[stage];
            const ResolvedResource resource = range[stageLayoutPositions[stage]];
            if (!SameLayoutBinding(firstResource, resource))
            {
                return std::unexpected(ErrorState{ .Code = ErrorCode::MergedLayoutConflict,
                                                         .Table = ShaderManifestTable::Bindings,
                                                         .RecordIndex = resource.Index(),
                                                         .Detail = stage });
            }
        }

        for (uint32_t match = 0u; match < matchCount; ++match)
        {
            ++stageLayoutPositions[stagesAtKey[match]];
        }
    }

    return view;
}

MergedLayoutView::Iterator MergedLayoutView::begin() const noexcept
{
    return Iterator{ this, std::array<uint32_t, k_MaxPipelineStages>{} };
}

MergedLayoutView::Iterator MergedLayoutView::end() const noexcept
{
    std::array<uint32_t, k_MaxPipelineStages> positions{};
    for (std::ptrdiff_t i = 0u; std::cmp_less(i, stageCount); ++i)
    {
        const LayoutRange& range = layoutRanges[i];
        positions[i] = range.Size();
    }
    return Iterator{ this, positions };
}

uint32_t MergedLayoutView::StageCount() const noexcept
{
    return stageCount;
}

std::optional<MergedPushConstants> MergedLayoutView::PushConstants() const noexcept
{
    return pushConstants;
}

MergedLayoutView::Iterator::Iterator(const MergedLayoutView* _layout,
                                     const std::array<uint32_t, k_MaxPipelineStages>& _positions) noexcept
    : layout{ _layout },
      positions{ _positions }
{
}

MergedResource MergedLayoutView::Iterator::operator*() const noexcept
{
    const std::span<const LayoutRange> ranges = std::span{ layout->layoutRanges }.first(layout->stageCount);
    const std::span<const uint32_t> activePositions = std::span{ positions }.first(layout->stageCount);
    const uint64_t key = NextPlacement(ranges, activePositions);
    assert(key != k_NoPlacement);

    std::array<uint32_t, k_MaxPipelineStages> stagesAtKey{};
    const uint32_t matchCount = StagesAtPlacement(ranges, activePositions, key, stagesAtKey);
    ShaderStageKind stages = ShaderStageKind::Invalid;
    for (uint32_t match = 0u; match < matchCount; ++match)
    {
        const uint32_t stage = stagesAtKey[match];
        stages |= layout->shaderStages[stage];
    }

    const uint32_t firstStage = stagesAtKey[0];
    const LayoutRange& firstRange = ranges[firstStage];
    return MergedResource{ .Resource = firstRange[activePositions[firstStage]], .Stages = stages };
}

MergedLayoutView::Iterator& MergedLayoutView::Iterator::operator++() noexcept
{
    const std::span<const LayoutRange> ranges = std::span{ layout->layoutRanges }.first(layout->stageCount);
    const std::span<uint32_t> activePositions = std::span{ positions }.first(layout->stageCount);
    const uint64_t key = NextPlacement(ranges, activePositions);

    std::array<uint32_t, k_MaxPipelineStages> stagesAtKey{};
    const uint32_t matchCount = StagesAtPlacement(ranges, activePositions, key, stagesAtKey);
    for (uint32_t match = 0u; match < matchCount; ++match)
    {
        ++activePositions[stagesAtKey[match]];
    }

    return *this;
}

MergedLayoutView::Iterator MergedLayoutView::Iterator::operator++(int) noexcept
{
    Iterator previous = *this;
    ++*this;
    return previous;
}

bool MergedLayoutView::Iterator::operator==(const Iterator& other) const noexcept
{
    return layout == other.layout && positions == other.positions;
}

bool MergedLayoutView::Iterator::operator!=(const Iterator& other) const noexcept
{
    return !(*this == other);
}

MergedLayoutView::PairRange MergedLayoutView::SamplerTexturePairs() const noexcept
{
    return PairRange{ this };
}

bool MergedLayoutView::HasSamplerTexturePairs() const noexcept
{
    return pairsRecorded;
}

MergedLayoutView::PairIterator MergedLayoutView::PairRange::begin() const noexcept
{
    PairIterator first{ layout, 0u, 0u };
    first.skipRepeats();
    return first;
}

MergedLayoutView::PairIterator MergedLayoutView::PairRange::end() const noexcept
{
    return PairIterator{ layout, layout->stageCount, 0u };
}

MergedLayoutView::PairIterator::PairIterator(const MergedLayoutView* _layout, uint32_t _stage, uint32_t _pair) noexcept
    : layout{ _layout },
      stage{ _stage },
      pair{ _pair }
{
}

void MergedLayoutView::PairIterator::skipRepeats() noexcept
{
    // few stages with few pairs each, so scanning the earlier stages is cheap
    while (stage < layout->stageCount)
    {
        const std::span<const SamplerTexturePair> pairs = layout->pairLists[stage];
        if (pair >= pairs.size())
        {
            ++stage;
            pair = 0u;
            continue;
        }

        bool seenEarlier = false;
        for (uint32_t earlier = 0u; earlier < stage && !seenEarlier; ++earlier)
        {
            seenEarlier = std::ranges::any_of(layout->pairLists[earlier], [&](const SamplerTexturePair& candidate) {
                return SamePairPlacement(layout->layoutRanges[earlier], candidate, layout->layoutRanges[stage], pairs[pair]);
            });
        }

        if (!seenEarlier)
        {
            return;
        }
        ++pair;
    }
    pair = 0u;
}

MergedSamplerTexturePair MergedLayoutView::PairIterator::operator*() const noexcept
{
    const LayoutRange& range = layout->layoutRanges[stage];
    const SamplerTexturePair& current = layout->pairLists[stage][pair];
    ShaderStageKind stages = layout->shaderStages[stage];
    for (uint32_t later = stage + 1u; later < layout->stageCount; ++later)
    {
        const bool sampledThere = std::ranges::any_of(layout->pairLists[later], [&](const SamplerTexturePair& candidate) {
            return SamePairPlacement(layout->layoutRanges[later], candidate, range, current);
        });
        if (sampledThere)
        {
            stages |= layout->shaderStages[later];
        }
    }

    return MergedSamplerTexturePair{ .Texture = range[current.Texture],
                                     .Sampler = range[current.Sampler],
                                     .Stages = stages };
}

MergedLayoutView::PairIterator& MergedLayoutView::PairIterator::operator++() noexcept
{
    ++pair;
    skipRepeats();
    return *this;
}

MergedLayoutView::PairIterator MergedLayoutView::PairIterator::operator++(int) noexcept
{
    PairIterator previous = *this;
    ++*this;
    return previous;
}

bool MergedLayoutView::PairIterator::operator==(const PairIterator& other) const noexcept
{
    return layout == other.layout && stage == other.stage && pair == other.pair;
}

bool MergedLayoutView::PairIterator::operator!=(const PairIterator& other) const noexcept
{
    return !(*this == other);
}

namespace
{
    
    bool SamePairPlacement(const LayoutRange& lhs_layout,
                           const SamplerTexturePair& lhs,
                           const LayoutRange& rhs_layout,
                           const SamplerTexturePair& rhs) noexcept
    {
        return BoundPlacementKey(lhs_layout[lhs.Texture].PlacementValue()) ==
                   BoundPlacementKey(rhs_layout[rhs.Texture].PlacementValue()) &&
               BoundPlacementKey(lhs_layout[lhs.Sampler].PlacementValue()) ==
                   BoundPlacementKey(rhs_layout[rhs.Sampler].PlacementValue());
    }

    bool TagsEqual(const ResolvedResource& lhs, const ResolvedResource& rhs) noexcept
    {
        if (lhs.TagCount() != rhs.TagCount())
        {
            return false;
        }
        for (uint32_t i = 0u; i < lhs.TagCount(); ++i)
        {
            if (lhs.Tag(i) != rhs.Tag(i))
            {
                return false;
            }
        }
        return true;
    }

    CreationSource SourceOfFootprint(const ResolvedResource& resource, bool stated) noexcept
    {
        if (!stated)
        {
            return CreationSource::Cpu;
        }
        const std::span<const uint64_t> axes = resource.FootprintAxes();
        const bool varies = std::ranges::any_of(axes, [](uint64_t word) { return word != 0u; });
        return varies ? CreationSource::ShaderPerVariant : CreationSource::Shader;
    }

    void DescribeTexture(const ResolvedResource& resource, CreationFields& fields) noexcept
    {
        const ResourceShape shape = resource.Shape();
        fields.Usage = CreationSource::Shader;
        fields.Dimension = CreationSource::Shader;
        fields.Extent = SourceOfFootprint(resource, resource.ExtentX() != 0u);
        fields.ArrayLayers = ResourceShapeIsArray(shape) ? CreationSource::Cpu : CreationSource::Shader;
        fields.MipLevels = CreationSource::Cpu;
        fields.SampleCount = ResourceShapeIsMultisample(shape) ? CreationSource::Cpu : CreationSource::Shader;
    }

} // namespace

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone::manifest
