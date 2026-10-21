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
using detail::RunOf;

namespace
{
    /** True when pair `lhs` of `lhs_layout` and pair `rhs` of `rhs_layout` name the same texture placement and
     * the same sampler placement. `Merge` checked that every resource is Bound. */
    bool SamePairPlacement(const LayoutRange& lhs_layout,
                           const SamplerTexturePair& lhs,
                           const LayoutRange& rhs_layout,
                           const SamplerTexturePair& rhs) noexcept;
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

bool ResolvedResource::operator==(const ResolvedResource& rhs) const noexcept
{
    return Kind() == rhs.Kind() &&
           Shape() == rhs.Shape() &&
           Access() == rhs.Access() &&
           StorageFormat() == rhs.StorageFormat() &&
           SampleType() == rhs.SampleType() &&
           IsComparisonSampler() == rhs.IsComparisonSampler() &&
           ArrayCount() == rhs.ArrayCount() &&
           ByteSize() == rhs.ByteSize();
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
    // visibility entries are positions in the variant's resource list, and the footprint list has the
    // same order. this is the only place that resolves that chain
    const uint32_t local = visible[position];
    // recall that footprints can be null, this is expected behavior - some resources may not have compile-time sizing info
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
    // no Bound key reaches this value: a group of 0xFFFFFFFF with a binding of 0xFFFFFFFF
    constexpr uint64_t k_NoPlacement = std::numeric_limits<uint64_t>::max();

    /**The smallest placement under any cursor, where the cursor is the position within each entry 
     * entry point's layout range. All the ranges are in strictly ascending order already, so this
     * is the next key of a N-way merge - where N is the number of stages being merged. */
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

    /** Writes the index of each stage whose layout position index sits on `key` into `out_stages`,
      * and returns how many stages matched that key. This tells us how many entry points / stages
      * are "sharing" a resource placement. */
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
            // PlacementValue is just a generic container of data per placement binding model...
            if (BoundPlacementKey(resource.PlacementValue()) == key)
            {
                // if the current key matches the current entry points resource at it's active
                // position, we record this stage as one that uses the given placement key.
                // this is the culmination of the merge at this step, effectively giving us the result we are after
                out_stages[count] = static_cast<uint32_t>(i);
                ++count;
            }

        }

        return count;
    }

    /** True when two resources at one placement can share one layout entry. Compares the fields read by a
     * bind group layout entry or a descriptor set layout binding. Names are not compared. */
    bool SameLayoutBinding(const ResolvedResource& lhs, const ResolvedResource& rhs) noexcept
    {
        // one binding record: the same environment and the same resource, so we can shortcut
        if (&lhs.Record() == &rhs.Record())
        {
            return true;
        }
        // otherwise we have to compare the fields
        return lhs == rhs;
    }

    // The blocks overlap from offset 0, so the bytes of the shorter block must have the same meaning in the
    // longer block. The members are in declaration order, so the shorter list must be a prefix of the longer list.
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

    // don't allow duplicate stages
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

        // will need to validate/verify layouts as bound/pointer/bindless etc here as appropriate, too
    }

    // The conflict pass is the same N-way walk the iterator does, with no output. after this,
    // iteration can't fail and can just run unchecked with no concern for the layouts
    const std::span<const LayoutRange> ranges = std::span{ view.layoutRanges }.first(view.stageCount);
    // stageLayoutPositions is the index (per entry point) in that entry point's layout range, 
    // telling us where in our walk through the layout ranges each stage currently is.
    // we need that so we can correctly merge the layout ranges across stages.
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
    // a dereference of end() is undefined, as it is for every iterator: begin() != end() means a key exists
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

    // every stage that reads this placement moves past it together
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
    // a pipeline has a few stages and each stage a few pairs, so a scan of the earlier stages is cheap and
    // needs no storage. the pairs of one stage are unique already
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

} // namespace

#ifdef __clang__
#pragma clang diagnostic pop
#endif

} // namespace lodestone::manifest
