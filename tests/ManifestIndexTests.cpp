#include "ShaderManifestIndex.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "VariantKey.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The query surface (`ManifestIndex` + `ManifestQueryBuilder`) is the one client object a renderer
// drives by hand, so it must reject a bad query with an error, never a crash. This test builds a
// manifest inline through the real emitter, so it exercises the true emit -> read -> query path with
// no asset and no compiler. The module has one axis of each value domain: a Boolean axis, an Enum
// axis (with explicit non-ascending case values), an Integral axis, and a Type axis. The full cross
// product is 2 * 3 * 3 * 2 = 36 variants, and the keys pack densely to 0..35.

#ifdef __clang__
#pragma clang diagnostic push
// this is test scaffolding, not shipping code
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage-in-libc-call"
#endif

using lodestone::AxisKind;
using lodestone::AxisValueDomain;
using lodestone::DecodedVariant;
using lodestone::EarliestBindingTime;
using lodestone::EmitShaderManifest;
using lodestone::ManifestIndex;
using lodestone::ManifestQueryBuilder;
using lodestone::PackVariantKey;
using lodestone::PermutationAxis;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::PinMode;
using lodestone::QueryErrorCode;
using lodestone::QueryPreset;
using lodestone::RawEnumCase;
using lodestone::RawInterfaceImpl;
using lodestone::manifest::BundleView;
using lodestone::manifest::EnvironmentView;
using lodestone::manifest::ManifestResult;
using lodestone::VariantKey;

namespace
{
// The axis order fixes the radices and the digit meaning of each axis.
constexpr uint32_t k_DitherAxis = 0u;  // Boolean, radix 2: {false, true}
constexpr uint32_t k_QualityAxis = 1u; // Enum,    radix 3: {Low=5, High=1, Medium=10}
constexpr uint32_t k_TileAxis = 2u;    // Integral, radix 3: {8, 16, 32}
constexpr uint32_t k_ShadeAxis = 3u;   // Type,    radix 2: {Lambert, Phong}
constexpr std::array<uint32_t, 4> k_Radices{ 2u, 3u, 3u, 2u };
constexpr size_t k_TotalVariants = 36u;

std::vector<PermutationValue> BoolValues()
{
    return { PermutationValue{ false }, PermutationValue{ true } };
}

std::vector<PermutationValue> OrdinalValues(uint32_t count, bool as_enum)
{
    std::vector<PermutationValue> values;
    values.reserve(count);
    for (uint32_t i = 0u; i < count; ++i)
    {
        values.emplace_back(as_enum ? PermutationValue::MakeEnum(i) : PermutationValue::MakeType(i));
    }
    return values;
}

std::vector<PermutationValue> IntegralValues(std::span<const uint32_t> raw)
{
    std::vector<PermutationValue> values;
    values.reserve(raw.size());
    for (const uint32_t value : raw)
    {
        values.emplace_back(value);
    }
    return values;
}

PermutationSpace MakeSpace()
{
    std::vector<PermutationAxis> axes;

    axes.emplace_back("DITHER", BoolValues(), AxisKind::Technique, EarliestBindingTime::Cook,
                      AxisValueDomain::Boolean);

    axes.emplace_back("QUALITY", OrdinalValues(3u, /*as_enum=*/true), AxisKind::Tuning,
                      EarliestBindingTime::Cook, AxisValueDomain::Enum,
                      /*active_when=*/std::string{}, /*root_name=*/"QualityLevel",
                      /*root_module=*/"TestEnumModule",
                      std::vector<RawEnumCase>{ RawEnumCase{ "Low", 5 }, RawEnumCase{ "High", 1 },
                                                RawEnumCase{ "Medium", 10 } });

    const std::array<uint32_t, 3> tileValues{ 8u, 16u, 32u };
    axes.emplace_back("TILE", IntegralValues(tileValues), AxisKind::Tuning,
                      EarliestBindingTime::Cook, AxisValueDomain::Integral);
    // the declared default is 16, not the first value, so the default cannot be confused with the canonical value
    axes.back().SetDeclaredDefaultIndex(1u);

    axes.emplace_back("SHADE", OrdinalValues(2u, /*as_enum=*/false), AxisKind::Technique,
                      EarliestBindingTime::Cook, AxisValueDomain::Type,
                      /*active_when=*/std::string{}, /*root_name=*/"IShade",
                      std::vector<RawInterfaceImpl>{ RawInterfaceImpl{ "TestShadeModule", "Lambert" },
                                                     RawInterfaceImpl{ "TestShadeModule", "Phong" } });

    return PermutationSpace{ std::move(axes) };
}

// Builds the four-axis module, keeping only the variants `keep` accepts. A subset models a cook that
// did not emit every combination: a policy allow-list, or an ActiveWhen gate that pins a child axis when
// its parent is off. The axis schema stays whole either way, because the space is unchanged; only the key
// set shrinks. `active` tells which axes each kept variant uses, and that fills the axis-active mask.
// The module points into `space`, so the space must outlive the emit.
template<typename Keep, typename Active>
lodestone::CookedModule BuildModule(const PermutationSpace& space, Keep keep, Active active)
{
    lodestone::CookedModule module;
    module.Name = "QueryTestModule";
    module.Space = &space;

    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name = "MainCS", .Stage = lodestone::ShaderStageKind::Compute });
    module.Sources.emplace_back(lodestone::tests::BytesOf("// wgsl for the test module"));

    lodestone::ReflectedBinding binding;
    binding.Name = "Data";
    binding.Placement = lodestone::BoundPlacement{ .Group = 0u, .Binding = 0u };
    binding.Kind = lodestone::BindingKind::StorageBuffer;
    binding.ElementStride = 16u;
    binding.Shape = lodestone::ResourceShape::StructuredBuffer;
    module.Resources.push_back(binding);
    module.ResourceLists.push_back(lodestone::ResourceList{ 0u });
    module.FootprintLists.push_back(
        lodestone::FootprintList{ lodestone::BufferFootprint{ .ElementCount = 256u } });
    module.VisibilityLists.push_back(lodestone::VisibilityList{ 0u });
    module.RasterStates.emplace_back();

    // The full cross product, enumerated in declaration order, packs to keys 0..35 in ascending order,
    // parallel to the variant table. Every variant shares the single source, list, and raster entry.
    uint32_t index = 0u;
    for (uint32_t dither = 0u; dither < k_Radices[k_DitherAxis]; ++dither)
    {
        for (uint32_t quality = 0u; quality < k_Radices[k_QualityAxis]; ++quality)
        {
            for (uint32_t tile = 0u; tile < k_Radices[k_TileAxis]; ++tile)
            {
                for (uint32_t shade = 0u; shade < k_Radices[k_ShadeAxis]; ++shade)
                {
                    const std::array<uint32_t, 4> digits{ dither, quality, tile, shade };
                    if (!keep(digits))
                    {
                        continue;
                    }
                    module.VariantKeys.push_back(PackVariantKey(digits, k_Radices));

                    lodestone::LibraryVariant variant;
                    variant.Index = index;
                    for (uint32_t axisIndex = 0u; axisIndex < digits.size(); ++axisIndex)
                    {
                        if (active(digits, axisIndex))
                        {
                            const PermutationAxis& axis = space.Axes()[axisIndex];
                            variant.Active.push_back(lodestone::PermutationBinding{
                                .Axis = &axis, .Value = axis.GetValues()[digits[axisIndex]] });
                        }
                    }
                    variant.ResourceListIndex = 0u;
                    variant.FootprintListIndex = 0u;
                    variant.SourceIndices.push_back(0u);
                    variant.VisibilityIndices.push_back(0u);
                    variant.RasterIndices.push_back(0u);
                    variant.Workgroups.emplace_back(lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u });
                    variant.Canonical = space.CanonicalizeAssignment(variant.Active);
                    module.Variants.emplace_back(std::move(variant));
                    ++index;
                }
            }
        }
    }

    module.SpaceSize = static_cast<uint64_t>(module.VariantKeys.size());
    return module;
}

bool KeepAll(const std::array<uint32_t, 4>&)
{
    return true;
}

bool KeepSmallTiles(const std::array<uint32_t, 4>& digits)
{
    return digits[k_TileAxis] != 2u;
}

// A module that cooked one variant, the first one: the case where a query needs no constraint at all.
bool KeepFirstOnly(const std::array<uint32_t, 4>& digits)
{
    return digits == std::array<uint32_t, 4>{ 0u, 0u, 0u, 0u };
}

bool AllActive(const std::array<uint32_t, 4>&, uint32_t)
{
    return true;
}

// The ActiveWhen gate of the gated test: SHADE is active only when DITHER is true.
bool ShadeActiveWhenDither(const std::array<uint32_t, 4>& digits, uint32_t axis_index)
{
    return axis_index != k_ShadeAxis || digits[k_DitherAxis] != 0u;
}

// Emits a library and returns its bytes. The bytes are self-contained, so the modules and their spaces
// can go once this returns. An emit failure returns no bytes, and the open that follows reports it.
std::vector<std::byte> EmitBytes(const lodestone::CookedLibrary& library)
{
    lodestone::CookResult<std::vector<std::byte>> manifest = EmitShaderManifest(library);
    if (!manifest.has_value())
    {
        return {};
    }

    return std::move(*manifest);
}

lodestone::CookedProfile MakeProfile(std::string target_name)
{
    return lodestone::CookedProfile{ .TargetName = std::move(target_name),
                                     .AccessModel = lodestone::PlacementKind::Bound,
                                     .CodeFormat = lodestone::ShaderCodeFormat::Wgsl };
}

// One module, cooked for one profile.
template<typename Keep, typename Active>
std::vector<std::byte> BuildManifest(Keep keep, Active active)
{
    const PermutationSpace space = MakeSpace();
    lodestone::CookedLibrary library;
    library.ModuleNames = { "QueryTestModule" };
    library.Profiles = { MakeProfile("wgsl") };
    library.Environments.emplace_back(BuildModule(space, keep, active));
    return EmitBytes(library);
}

// The full cross product: every combination is a cooked variant, and every axis is active.
std::vector<std::byte> BuildManifestBytes()
{
    return BuildManifest(&KeepAll, &AllActive);
}

// Opens one environment of a bundle. The views contain spans into `bytes`, so the bytes must outlive them.
ManifestResult<EnvironmentView> OpenEnvironment(std::span<const std::byte> bytes,
                                                uint32_t profile_index,
                                                uint32_t module_index)
{
    const ManifestResult<BundleView> bundle = BundleView::Open(bytes);
    if (!bundle.has_value())
    {
        return std::unexpected(bundle.error());
    }

    return bundle->OpenEnvironment(profile_index, module_index);
}

// A module with one integral TILE axis, one variant for each value. The key of a one-axis module is its
// digit, so the keys are 0 to N-1.
PermutationSpace MakeTileSpace(std::span<const uint32_t> tile_values)
{
    std::vector<PermutationAxis> axes;
    axes.emplace_back("TILE", IntegralValues(tile_values), AxisKind::Tuning, EarliestBindingTime::Cook,
                      AxisValueDomain::Integral);
    return PermutationSpace{ std::move(axes) };
}

lodestone::CookedModule BuildTileModule(std::string name, const PermutationSpace& space)
{
    lodestone::CookedModule module;
    module.Name = std::move(name);
    module.Space = &space;
    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name = "MainCS", .Stage = lodestone::ShaderStageKind::Compute });
    module.Sources.emplace_back(lodestone::tests::BytesOf("// wgsl for a tile module"));
    module.ResourceLists.emplace_back();
    module.FootprintLists.emplace_back();
    module.VisibilityLists.emplace_back();
    module.RasterStates.emplace_back();

    const uint32_t valueCount = static_cast<uint32_t>(space.Axes()[0].NumValues());
    for (uint32_t digit = 0u; digit < valueCount; ++digit)
    {
        module.VariantKeys.push_back(VariantKey{ digit });
        lodestone::LibraryVariant variant;
        variant.Index = digit;
        const PermutationAxis& axis = space.Axes()[0];
        variant.Active.push_back(lodestone::PermutationBinding{ .Axis = &axis, .Value = axis.GetValues()[digit] });
        variant.SourceIndices.push_back(0u);
        variant.VisibilityIndices.push_back(0u);
        variant.RasterIndices.push_back(0u);
        variant.Workgroups.emplace_back(lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u });
        variant.Canonical = space.CanonicalizeAssignment(variant.Active);
        module.Variants.emplace_back(std::move(variant));
    }

    module.SpaceSize = valueCount;
    return module;
}

// Two boolean axes and two entry points. `FogCS` has one source for each value of FOG, and `PlainCS` has one
// source for every variant. DEBUG changes nothing. So FOG is relevant to FogCS only, and DEBUG is relevant to
// nothing. `keep_debug_on` false cooks only DEBUG = false, which leaves DEBUG with one cooked value.
PermutationSpace MakeRelevanceSpace()
{
    std::vector<PermutationAxis> axes;
    axes.emplace_back("FOG", BoolValues(), AxisKind::Technique, EarliestBindingTime::Cook, AxisValueDomain::Boolean);
    axes.emplace_back("DEBUG", BoolValues(), AxisKind::Technique, EarliestBindingTime::Cook, AxisValueDomain::Boolean);
    return PermutationSpace{ std::move(axes) };
}

lodestone::CookedModule BuildRelevanceModule(const PermutationSpace& space, bool keep_debug_on)
{
    lodestone::CookedModule module;
    module.Name = "RelevanceModule";
    module.Space = &space;
    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name = "FogCS", .Stage = lodestone::ShaderStageKind::Compute });
    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name = "PlainCS", .Stage = lodestone::ShaderStageKind::Compute });
    module.Sources.emplace_back(lodestone::tests::BytesOf("// fog off"));
    module.Sources.emplace_back(lodestone::tests::BytesOf("// fog on"));
    module.Sources.emplace_back(lodestone::tests::BytesOf("// plain"));
    module.ResourceLists.emplace_back();
    module.FootprintLists.emplace_back();
    module.VisibilityLists.emplace_back();
    module.RasterStates.emplace_back();

    uint32_t index = 0u;
    for (const bool fog : { false, true })
    {
        for (const bool debug : { false, true })
        {
            if (debug && !keep_debug_on)
            {
                continue;
            }

            lodestone::LibraryVariant variant;
            variant.Index = index++;
            variant.Active = lodestone::PermutationAssignment{
                lodestone::PermutationBinding{ .Axis = &space.Axes()[0], .Value = PermutationValue{ fog } },
                lodestone::PermutationBinding{ .Axis = &space.Axes()[1], .Value = PermutationValue{ debug } } };
            variant.Canonical = space.CanonicalizeAssignment(variant.Active);
            module.VariantKeys.push_back(space.ComputeVariantKey(variant.Canonical));
            variant.SourceIndices = { fog ? 1u : 0u, 2u };
            variant.VisibilityIndices = { 0u, 0u };
            variant.RasterIndices = { 0u, 0u };
            variant.Workgroups = { lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u },
                                   lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u } };
            module.Variants.emplace_back(std::move(variant));
        }
    }

    module.SpaceSize = 4u;
    return module;
}

void CheckAxisRelevance(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("axis relevance for each entry point");

    // local axis 0 is FOG, 1 is DEBUG. entry point 0 is FogCS, 1 is PlainCS
    const PermutationSpace space = MakeRelevanceSpace();
    for (const bool keepDebugOn : { true, false })
    {
        lodestone::CookedLibrary library;
        library.ModuleNames = { "RelevanceModule" };
        library.Profiles = { MakeProfile("wgsl") };
        library.Environments.emplace_back(BuildRelevanceModule(space, keepDebugOn));
        const std::vector<std::byte> bytes = EmitBytes(library);
        const ManifestResult<EnvironmentView> opened = OpenEnvironment(bytes, 0u, 0u);
        runner.Check(opened.has_value(), "the relevance module opens");
        if (!opened.has_value())
        {
            return;
        }

        const EnvironmentView& view = opened.value();
        if (keepDebugOn)
        {
            runner.Check(view.IsAxisRelevant(0u, 0u), "an axis that changes the code is relevant to that entry point");
            runner.Check(!view.IsAxisRelevant(1u, 0u), "the same axis is not relevant to an entry point it never changes");
            runner.Check(!view.IsAxisRelevant(0u, 1u) && !view.IsAxisRelevant(1u, 1u),
                         "an axis that changes nothing is relevant to no entry point");

            // SingleFor counts only the axes the named entry points read
            const ManifestIndex index{ view };
            const std::array<std::string_view, 1u> fog{ "FogCS" };
            const std::array<std::string_view, 1u> plain{ "PlainCS" };
            const std::array<std::string_view, 2u> pipeline{ "FogCS", "PlainCS" };
            const std::expected<VariantKey, lodestone::QueryError> allAxes = index.Query().Where("FOG", true).Single();
            runner.Check(!allAxes.has_value() && allAxes.error().Code == lodestone::QueryErrorCode::AmbiguousVariant &&
                             allAxes.error().AxisName == "DEBUG",
                         "Single still counts an axis that changes nothing");
            const std::expected<VariantKey, lodestone::QueryError> fogPinned = index.Query().Where("FOG", true).SingleFor(fog);
            const lodestone::QueryResult<std::vector<VariantKey>> fogKeys = index.Query().Where("FOG", true).Keys();
            runner.Check(fogPinned.has_value() && fogKeys.has_value() && fogPinned.value() == std::ranges::min(*fogKeys),
                         "SingleFor ignores an irrelevant axis and returns the smallest matching key");
            const std::expected<VariantKey, lodestone::QueryError> fogUnpinned = index.Query().SingleFor(fog);
            runner.Check(!fogUnpinned.has_value() &&
                             fogUnpinned.error().Code == lodestone::QueryErrorCode::AmbiguousVariant &&
                             fogUnpinned.error().AxisName == "FOG" && fogUnpinned.error().Detail == 4u,
                         "SingleFor names only the relevant axes that still vary");
            runner.Check(index.Query().SingleFor(plain).has_value(),
                         "an entry point that no axis changes needs no pin at all");

            // NotesFor names each pin that cannot change the named entry points. DEBUG changes nothing, and
            // FOG changes FogCS only
            const ManifestQueryBuilder bothPinned = index.Query().Where("FOG", true).Where("DEBUG", true);
            const std::expected<std::vector<lodestone::QueryNote>, lodestone::QueryError> fogNotes =
                bothPinned.NotesFor(fog);
            runner.Check(fogNotes.has_value() && fogNotes->size() == 1u &&
                             fogNotes->front().Code == lodestone::QueryNoteCode::PinAxisIrrelevant &&
                             fogNotes->front().AxisName == "DEBUG",
                         "NotesFor names a pin that cannot change the entry point");
            const std::expected<std::vector<lodestone::QueryNote>, lodestone::QueryError> plainNotes =
                bothPinned.NotesFor(plain);
            runner.Check(plainNotes.has_value() && plainNotes->size() == 2u,
                         "NotesFor names every pin when no pinned axis changes the entry point");
            const std::expected<std::vector<lodestone::QueryNote>, lodestone::QueryError> pipelineNotes =
                bothPinned.NotesFor(pipeline);
            runner.Check(pipelineNotes.has_value() && pipelineNotes->size() == 1u,
                         "NotesFor joins the masks of a pipeline, as SingleFor does");
            const std::span<const std::string_view> noStages{};
            const std::expected<std::vector<lodestone::QueryNote>, lodestone::QueryError> noStageNotes =
                bothPinned.NotesFor(noStages);
            runner.Check(!noStageNotes.has_value() &&
                             noStageNotes.error().Code == lodestone::QueryErrorCode::EmptyEntryPointSet,
                         "NotesFor with no entry point is an error");
            const std::expected<VariantKey, lodestone::QueryError> joined = index.Query().SingleFor(pipeline);
            runner.Check(!joined.has_value() && joined.error().AxisName == "FOG",
                         "a pipeline counts every axis that any of its entry points reads");
            const std::array<std::string_view, 1u> typo{ "FogC" };
            const std::expected<VariantKey, lodestone::QueryError> unknown = index.Query().SingleFor(typo);
            runner.Check(!unknown.has_value() && unknown.error().Code == lodestone::QueryErrorCode::UnknownEntryPoint &&
                             unknown.error().AxisName == "FogC" && unknown.error().Suggestion == "FogCS",
                         "an unknown entry point name is an error with a suggestion");
            const std::expected<VariantKey, lodestone::QueryError> empty =
                index.Query().SingleFor(std::span<const std::string_view>{});
            runner.Check(!empty.has_value() && empty.error().Code == lodestone::QueryErrorCode::EmptyEntryPointSet,
                         "SingleFor with no entry point is an error");

            // a bit past the module's two axes is damage, and the reader rejects it when the environment opens
            lodestone::manifest::Header header;
            std::memcpy(&header, bytes.data(), sizeof(header));
            lodestone::manifest::EnvironmentDirectoryEntry entry;
            std::memcpy(&entry, bytes.data() + header.EnvironmentDirectoryOffset, sizeof(entry));
            lodestone::manifest::EnvironmentHeader environment;
            std::memcpy(&environment, bytes.data() + entry.ExtentOffset, sizeof(environment));
            const size_t wordOffset = entry.ExtentOffset + environment.AxisRelevanceTableOffset;

            std::vector<std::byte> damaged = bytes;
            uint64_t word = 0u;
            std::memcpy(&word, damaged.data() + wordOffset, sizeof(word));
            word |= uint64_t{ 1u } << 63u;
            std::memcpy(damaged.data() + wordOffset, &word, sizeof(word));
            const ManifestResult<EnvironmentView> damagedOpen = OpenEnvironment(damaged, 0u, 0u);
            runner.Check(!damagedOpen.has_value() &&
                             damagedOpen.error().Code == lodestone::manifest::ErrorCode::InvalidAxisRelevance,
                         "a relevance bit past the module's axes is rejected when the environment opens");
        }
        else
        {
            runner.Check(view.IsAxisRelevant(0u, 1u) && view.IsAxisRelevant(1u, 1u),
                         "an axis with one cooked value cannot show a difference, so it counts as relevant");
            runner.Check(view.IsAxisRelevant(0u, 0u) && !view.IsAxisRelevant(1u, 0u),
                         "the other axis is still measured");
        }
    }
}

// Three boolean axes, and a sparse set of cooked variants: (0,0,0), (1,1,0), (0,0,1), (1,0,1), (0,1,1). Only
// (1,1,0) has different code. A and B each have a pair that differs in that axis alone, (x,x,1), and that pair
// shares one instance. C has one such pair, (0,0,0) and (0,0,1), and it shares one instance too. So the per-axis
// pass finds every axis Inert. But (0,0,0) and (1,1,0) differ in their code, and no cooked variant links them.
// This is what a sparse cook policy can leave, and what the closing pass of `model/AxisInfluence` exists for.
PermutationSpace MakeChainSpace()
{
    std::vector<PermutationAxis> axes;
    for (const std::string_view name : { "A", "B", "C" })
    {
        axes.emplace_back(std::string{ name }, BoolValues(), AxisKind::Technique, EarliestBindingTime::Cook,
                          AxisValueDomain::Boolean);
    }
    return PermutationSpace{ std::move(axes) };
}

lodestone::CookedModule BuildChainModule(const PermutationSpace& space)
{
    lodestone::CookedModule module;
    module.Name = "ChainModule";
    module.Space = &space;
    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name = "MainCS", .Stage = lodestone::ShaderStageKind::Compute });
    module.Sources.emplace_back(lodestone::tests::BytesOf("// common"));
    module.Sources.emplace_back(lodestone::tests::BytesOf("// only A and B together"));
    module.ResourceLists.emplace_back();
    module.FootprintLists.emplace_back();
    module.VisibilityLists.emplace_back();
    module.RasterStates.emplace_back();

    constexpr std::array<std::array<bool, 3u>, 5u> k_Cooked{ { { false, false, false },
                                                               { true, true, false },
                                                               { false, false, true },
                                                               { true, false, true },
                                                               { false, true, true } } };
    uint32_t index = 0u;
    for (const std::array<bool, 3u>& values : k_Cooked)
    {
        lodestone::LibraryVariant variant;
        variant.Index = index++;
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
            variant.Active.push_back(
                lodestone::PermutationBinding{ .Axis = &space.Axes()[axis], .Value = PermutationValue{ values[axis] } });
        }
        variant.Canonical = space.CanonicalizeAssignment(variant.Active);
        module.VariantKeys.push_back(space.ComputeVariantKey(variant.Canonical));
        variant.SourceIndices = { (values[0] && values[1] && !values[2]) ? 1u : 0u };
        variant.VisibilityIndices = { 0u };
        variant.RasterIndices = { 0u };
        variant.Workgroups = { lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u } };
        module.Variants.emplace_back(std::move(variant));
    }

    // the emitter wants the variants in key order
    std::vector<size_t> order(module.Variants.size());
    std::ranges::iota(order, size_t{ 0u });
    std::ranges::sort(order, {}, [&](size_t i) { return module.VariantKeys[i]; });
    std::vector<lodestone::LibraryVariant> sortedVariants;
    std::vector<VariantKey> sortedKeys;
    for (const size_t i : order)
    {
        sortedVariants.push_back(module.Variants[i]);
        sortedKeys.push_back(module.VariantKeys[i]);
    }
    for (size_t i = 0u; i < sortedVariants.size(); ++i)
    {
        sortedVariants[i].Index = i;
    }
    module.Variants = std::move(sortedVariants);
    module.VariantKeys = std::move(sortedKeys);
    module.SpaceSize = 8u;
    return module;
}

void CheckAxisRelevanceValidator(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("the axis relevance validator reads the written manifest");

    // a stored mask that the written instances contradict
    const PermutationSpace relevanceSpace = MakeRelevanceSpace();
    lodestone::CookedLibrary relevanceLibrary;
    relevanceLibrary.ModuleNames = { "RelevanceModule" };
    relevanceLibrary.Profiles = { MakeProfile("wgsl") };
    relevanceLibrary.Environments.emplace_back(BuildRelevanceModule(relevanceSpace, true));
    const std::vector<std::byte> bytes = EmitBytes(relevanceLibrary);
    runner.Check(lodestone::VerifyManifestRoundTrip(relevanceLibrary, bytes) == lodestone::CookError::Success,
                 "the relevance module passes the validator");

    lodestone::manifest::Header header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    lodestone::manifest::EnvironmentDirectoryEntry entry;
    std::memcpy(&entry, bytes.data() + header.EnvironmentDirectoryOffset, sizeof(entry));
    lodestone::manifest::EnvironmentHeader environment;
    std::memcpy(&environment, bytes.data() + entry.ExtentOffset, sizeof(environment));
    const size_t fogWordOffset = entry.ExtentOffset + environment.AxisRelevanceTableOffset;
    std::vector<std::byte> damaged = bytes;
    uint64_t word = 0u;
    std::memcpy(&word, damaged.data() + fogWordOffset, sizeof(word));
    word &= ~uint64_t{ 1u }; // FOG, for FogCS
    std::memcpy(damaged.data() + fogWordOffset, &word, sizeof(word));
    runner.Check(lodestone::VerifyManifestRoundTrip(relevanceLibrary, damaged) ==
                     lodestone::CookError::ManifestAxisRelevanceMismatch,
                 "a stored mask that calls a code-changing axis irrelevant fails the validator");

    // the broken chain: every axis is Inert in the per-axis pass, and the closing pass must mark all three,
    // because (0,0,0) and (1,1,0) hold different code and the whole cook is one class of the empty mask
    const PermutationSpace chainSpace = MakeChainSpace();
    lodestone::CookedLibrary chainLibrary;
    chainLibrary.ModuleNames = { "ChainModule" };
    chainLibrary.Profiles = { MakeProfile("wgsl") };
    chainLibrary.Environments.emplace_back(BuildChainModule(chainSpace));
    const std::vector<std::byte> chainBytes = EmitBytes(chainLibrary);
    const ManifestResult<EnvironmentView> chainView = OpenEnvironment(chainBytes, 0u, 0u);
    runner.Check(chainView.has_value() && chainView->IsAxisRelevant(0u, 0u) && chainView->IsAxisRelevant(0u, 1u) &&
                     chainView->IsAxisRelevant(0u, 2u),
                 "the closing pass marks every axis that varies in a class with two instances");
    runner.Check(lodestone::VerifyManifestRoundTrip(chainLibrary, chainBytes) == lodestone::CookError::Success,
                 "the closed mask passes the validator");
    if (chainView.has_value())
    {
        const ManifestIndex chainIndex{ *chainView };
        const std::array<std::string_view, 1u> mainCS{ "MainCS" };
        // C = false matches (0,0,0) and (1,1,0), which hold different code, so this must not pick one of them
        const std::expected<VariantKey, lodestone::QueryError> highQuality =
            chainIndex.Query().Where("C", false).SingleFor(mainCS);
        runner.Check(!highQuality.has_value() &&
                         highQuality.error().Code == lodestone::QueryErrorCode::AmbiguousVariant &&
                         highQuality.error().AxisName == "A, B",
                     "SingleFor does not choose between the two code paths of the broken chain");
    }

    // the per-axis pass alone derives an empty mask here, so only a validator that closes the mask too can see
    // that an empty stored mask is wrong
    const lodestone::manifest::Header chainHeader = [&] {
        lodestone::manifest::Header value;
        std::memcpy(&value, chainBytes.data(), sizeof(value));
        return value;
    }();
    lodestone::manifest::EnvironmentDirectoryEntry chainEntry;
    std::memcpy(&chainEntry, chainBytes.data() + chainHeader.EnvironmentDirectoryOffset, sizeof(chainEntry));
    lodestone::manifest::EnvironmentHeader chainEnvironment;
    std::memcpy(&chainEnvironment, chainBytes.data() + chainEntry.ExtentOffset, sizeof(chainEnvironment));
    std::vector<std::byte> unclosed = chainBytes;
    const uint64_t noAxes = 0u;
    std::memcpy(unclosed.data() + chainEntry.ExtentOffset + chainEnvironment.AxisRelevanceTableOffset, &noAxes,
                sizeof(noAxes));
    runner.Check(lodestone::VerifyManifestRoundTrip(chainLibrary, unclosed) ==
                     lodestone::CookError::ManifestAxisRelevanceMismatch,
                 "a stored mask that the per-axis pass alone would accept fails the validator");
}

// Counts the keys of a query, or returns SIZE_MAX when the query is in an error state.
size_t KeyCount(const ManifestQueryBuilder& query)
{
    const lodestone::QueryResult<std::vector<VariantKey>> keys = query.Keys();
    return keys.has_value() ? keys->size() : SIZE_MAX;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "ManifestIndexTests" };

    const std::vector<std::byte> bytes = BuildManifestBytes();
    const ManifestResult<EnvironmentView> opened = OpenEnvironment(bytes, 0u, 0u);
    runner.Check(opened.has_value(), "the emitter produces a manifest the reader accepts");
    if (!opened.has_value())
    {
        return runner.Report();
    }

    const ManifestIndex index{ opened.value() };

    runner.BeginSection("the index enumerates and decodes every variant");
    runner.Check(index.Enumerate().size() == k_TotalVariants, "Enumerate returns every variant");
    runner.Check(index.View().Module().AxisCount() == 4u, "the module carries four axes");

    // Decode a known key: digits [dither=1, quality=2 (Medium), tile=1 (16), shade=0 (Lambert)].
    const std::array<uint32_t, 4> knownDigits{ 1u, 2u, 1u, 0u };
    const VariantKey knownKey = PackVariantKey(knownDigits, k_Radices);
    const std::vector<lodestone::QueryAxisValue> decoded = index.Decode(knownKey);
    runner.Check(decoded.size() == 4u, "Decode returns one value per axis");
    if (decoded.size() == 4u)
    {
        runner.Check(decoded[k_DitherAxis].Type == AxisValueDomain::Boolean &&
                     decoded[k_DitherAxis].IntegralValue == 1u, "the boolean axis decodes to true");
        runner.Check(decoded[k_QualityAxis].Type == AxisValueDomain::Enum &&
                     decoded[k_QualityAxis].Name == "Medium", "the enum axis decodes to the case name");
        runner.Check(decoded[k_TileAxis].Type == AxisValueDomain::Integral &&
                     decoded[k_TileAxis].IntegralValue == 16u, "the integral axis decodes to its value");
        runner.Check(decoded[k_ShadeAxis].Type == AxisValueDomain::Type &&
                     decoded[k_ShadeAxis].Name == "Lambert", "the type axis decodes to the impl name");
    }

    runner.BeginSection("the index names its axes and their values");
    {
        const std::vector<std::string_view> names = index.AxisNames();
        runner.Check(names.size() == 4u && names[k_DitherAxis] == "DITHER" && names[k_QualityAxis] == "QUALITY" &&
                         names[k_TileAxis] == "TILE" && names[k_ShadeAxis] == "SHADE",
                     "AxisNames lists every axis in axis order");
        const lodestone::QueryResult<std::vector<lodestone::QueryAxisValue>> quality = index.AxisValues("QUALITY");
        runner.Check(quality.has_value() && quality->size() == 3u && (*quality)[0].Name == "Low" &&
                         (*quality)[1].Name == "High" && (*quality)[2].Name == "Medium",
                     "AxisValues lists an enum axis by case name, in digit order");
        const lodestone::QueryResult<std::vector<lodestone::QueryAxisValue>> tile = index.AxisValues("TILE");
        runner.Check(tile.has_value() && tile->size() == 3u && (*tile)[2].IntegralValue == 32u,
                     "AxisValues lists an integral axis by value");
        runner.Check(!index.AxisValues("TIEL").has_value() &&
                         index.AxisValues("TIEL").error() == QueryErrorCode::UnknownAxis,
                     "AxisValues reports an unknown axis");
    }

    runner.BeginSection("the index reads the declared default of each axis");
    {
        runner.Check(index.View().Module().AxisDefaultDigit(k_TileAxis) == 1u,
                     "the module stores the default as a digit of its own key");
        const lodestone::QueryResult<lodestone::QueryAxisValue> tileDefault = index.AxisDefault("TILE");
        runner.Check(tileDefault.has_value() && tileDefault->IntegralValue == 16u,
                     "AxisDefault returns the declared default, not the first value");
        const lodestone::QueryResult<lodestone::QueryAxisValue> qualityDefault = index.AxisDefault("QUALITY");
        runner.Check(qualityDefault.has_value() && qualityDefault->Name == "Low",
                     "an axis declared with its first value as the default reads that value");
        runner.Check(!index.AxisDefault("TIEL").has_value() &&
                         index.AxisDefault("TIEL").error() == QueryErrorCode::UnknownAxis,
                     "AxisDefault reports an unknown axis");

        // damage the default digit of TILE so that it reaches past the axis radix
        const ManifestResult<BundleView> bundle = BundleView::Open(bytes);
        if (bundle.has_value())
        {
            const lodestone::manifest::ModuleView module = bundle->Module(0u);
            const std::span<const lodestone::manifest::ModuleAxis> moduleAxes = module.ModuleAxes();
            const lodestone::manifest::ModuleAxis* tileRecord = &moduleAxes[k_TileAxis];
            const size_t recordOffset = static_cast<size_t>(reinterpret_cast<const std::byte*>(tileRecord) - bytes.data());
            std::vector<std::byte> badDefault = bytes;
            const uint32_t pastRadix = k_Radices[k_TileAxis];
            std::memcpy(badDefault.data() + recordOffset + offsetof(lodestone::manifest::ModuleAxis, DefaultDigit),
                        &pastRadix,
                        sizeof(pastRadix));
            const ManifestResult<BundleView> damaged = BundleView::Open(badDefault);
            runner.Check(!damaged.has_value() &&
                             damaged.error().Code == lodestone::manifest::ErrorCode::InvalidModuleAxisDefault,
                         "a default digit at or past the radix is rejected when the bundle opens");
        }
    }

    runner.BeginSection("an empty query matches every variant");
    runner.Check(KeyCount(index.Query()) == k_TotalVariants, "no constraint leaves every variant");

    runner.BeginSection("a single Where narrows one axis");
    runner.Check(KeyCount(index.Query().Where("DITHER", true)) == 18u, "one boolean value halves the set");
    runner.Check(KeyCount(index.Query().Where("QUALITY", AxisValueDomain::Enum, "Low")) == 12u,
                 "one enum case is a third of the set");
    runner.Check(KeyCount(index.Query().Where("TILE", 16u)) == 12u, "one integral value is a third");
    runner.Check(KeyCount(index.Query().Where("SHADE", AxisValueDomain::Type, "Phong")) == 18u,
                 "one type impl halves the set");

    runner.BeginSection("constraints on two axes intersect");
    runner.Check(KeyCount(index.Query().Where("DITHER", true).Where("TILE", 16u)) == 6u,
                 "two axes constrained is the product of the fractions");

    runner.BeginSection("a filtered query decodes to the value it asked for");
    const lodestone::QueryResult<std::vector<DecodedVariant>> ditherOn =
        index.Query().Where("DITHER", true).Variants();
    runner.Check(ditherOn.has_value(), "the query resolves");
    if (ditherOn.has_value())
    {
        bool allTrue = true;
        for (const DecodedVariant& variant : *ditherOn)
        {
            allTrue = allTrue && variant.Values[k_DitherAxis].IntegralValue == 1u;
        }
        runner.Check(allTrue, "every returned variant has the boolean value set");
    }

    runner.BeginSection("WhereAnyOf unions values within one axis");
    const std::array<uint32_t, 2> tilePair{ 8u, 32u };
    runner.Check(KeyCount(index.Query().WhereAnyOf("TILE", tilePair)) == 24u,
                 "two of three integral values is two thirds");
    const std::array<std::string_view, 2> qualityPair{ "Low", "Medium" };
    runner.Check(KeyCount(index.Query().WhereAnyOf("QUALITY", AxisValueDomain::Enum, qualityPair)) == 24u,
                 "two of three enum cases is two thirds");
    runner.Check(KeyCount(index.Query().WhereAnyOfBoolean("DITHER")) == k_TotalVariants,
                 "both boolean values leave every variant");

    runner.BeginSection("WhereNoneOf keeps the complement");
    const std::array<uint32_t, 1> tileEight{ 8u };
    runner.Check(KeyCount(index.Query().WhereNoneOf("TILE", tileEight)) == 24u,
                 "excluding one integral value keeps the other two");
    const std::array<std::string_view, 1> qualityHigh{ "High" };
    runner.Check(KeyCount(index.Query().WhereNoneOf("QUALITY", AxisValueDomain::Enum, qualityHigh)) == 24u,
                 "excluding one enum case keeps the other two");
    runner.Check(KeyCount(index.Query().WhereNoneOf("DITHER", true)) == 18u,
                 "excluding one boolean value keeps the other");

    runner.BeginSection("two Wheres on one axis coalesce into a union");
    runner.Check(KeyCount(index.Query().Where("TILE", 8u).Where("TILE", 16u)) == 24u,
                 "a second Where on the same axis widens the constraint");

    runner.BeginSection("VariantsFromKeys agrees with Variants");
    const ManifestQueryBuilder tileQuery = index.Query().Where("TILE", 16u);
    const lodestone::QueryResult<std::vector<VariantKey>> tileKeys = tileQuery.Keys();
    if (tileKeys.has_value())
    {
        const std::vector<DecodedVariant> fromKeys = tileQuery.VariantsFromKeys(*tileKeys);
        const lodestone::QueryResult<std::vector<DecodedVariant>> direct = tileQuery.Variants();
        runner.Check(direct.has_value() && direct->size() == fromKeys.size(),
                     "the two paths return the same count");
    }

    runner.BeginSection("a valid query reports no error");
    const ManifestQueryBuilder goodQuery = index.Query().Where("TILE", 16u);
    runner.Check(goodQuery.IsValid(), "a resolved query is valid");
    runner.Check(goodQuery.Errors().empty(), "a resolved query has no errors");

    runner.BeginSection("an unknown axis reports UnknownAxis with a suggestion");
    const ManifestQueryBuilder unknownAxis = index.Query().Where("DITHR", true);
    runner.Check(!unknownAxis.IsValid(), "an unknown axis invalidates the query");
    runner.Check(!unknownAxis.Errors().empty() &&
                 unknownAxis.Errors().front().Code == QueryErrorCode::UnknownAxis,
                 "the error names the unknown axis");
    if (!unknownAxis.Errors().empty())
    {
        runner.Check(unknownAxis.Errors().front().Suggestion == "DITHER", "the nearest axis name is suggested");
    }
    runner.Check(!unknownAxis.Keys().has_value() &&
                 unknownAxis.Keys().error() == QueryErrorCode::UnknownAxis,
                 "the terminal returns the error code");

    runner.BeginSection("a value in the wrong domain reports IncorrectValueDomain");
    const ManifestQueryBuilder wrongDomain = index.Query().Where("DITHER", 5u);
    runner.Check(!wrongDomain.IsValid() &&
                 wrongDomain.Errors().front().Code == QueryErrorCode::IncorrectValueDomain,
                 "an integral value on a boolean axis is rejected");

    runner.BeginSection("a value not in the axis reports ValueNotInAxis");
    const ManifestQueryBuilder badInt = index.Query().Where("TILE", 999u);
    runner.Check(!badInt.IsValid() && badInt.Errors().front().Code == QueryErrorCode::ValueNotInAxis,
                 "an integral value the axis does not hold is rejected");
    const ManifestQueryBuilder badEnum = index.Query().Where("QUALITY", AxisValueDomain::Enum, "Loww");
    runner.Check(!badEnum.IsValid() && badEnum.Errors().front().Code == QueryErrorCode::ValueNotInAxis,
                 "an enum case the axis does not hold is rejected");
    if (!badEnum.Errors().empty())
    {
        runner.Check(badEnum.Errors().front().Suggestion == "Low", "the nearest case name is suggested");
    }

    runner.BeginSection("errors accumulate and the terminal reports the first");
    const ManifestQueryBuilder twoErrors = index.Query().Where("NOPE", true).Where("ALSONOPE", 1u);
    runner.Check(twoErrors.Errors().size() == 2u, "each bad Where adds one error");
    runner.Check(!twoErrors.Keys().has_value() &&
                 twoErrors.Keys().error() == QueryErrorCode::UnknownAxis,
                 "the terminal returns the first error code");

    // make sure that empty value sets into constraints report back as expected, but still
    // make sure the unknown axis is reported before the empty set
    runner.BeginSection("an empty value set reports EmptyConstraintSet");
    const std::span<const uint32_t> noInts;
    const std::span<const std::string_view> noNames;
    const ManifestQueryBuilder emptyAny = index.Query().WhereAnyOf("TILE", noInts);
    runner.Check(!emptyAny.IsValid() &&
                 emptyAny.Errors().front().Code == QueryErrorCode::EmptyConstraintSet,
                 "WhereAnyOf with no values is rejected");
    const ManifestQueryBuilder emptyNone = index.Query().WhereNoneOf("TILE", noInts);
    runner.Check(!emptyNone.IsValid() &&
                 emptyNone.Errors().front().Code == QueryErrorCode::EmptyConstraintSet,
                 "WhereNoneOf with no values is rejected");
    const ManifestQueryBuilder emptyNames = index.Query().WhereAnyOf("QUALITY", AxisValueDomain::Enum, noNames);
    runner.Check(!emptyNames.IsValid() &&
                 emptyNames.Errors().front().Code == QueryErrorCode::EmptyConstraintSet,
                 "an empty named value set is rejected");
    const ManifestQueryBuilder emptyUnknown = index.Query().WhereAnyOf("NOPE", noInts);
    runner.Check(!emptyUnknown.IsValid() &&
                 emptyUnknown.Errors().front().Code == QueryErrorCode::UnknownAxis,
                 "an unknown axis is reported before the empty set");

    // Single() is the terminal a renderer uses to pick a variant. It returns one key or an error, and
    // never chooses between two matches.
    runner.BeginSection("Single returns the one match, and names the axes when there are more");
    const std::expected<VariantKey, lodestone::QueryError> pinned = index.Query()
                                                                        .Where("DITHER", true)
                                                                        .Where("QUALITY", AxisValueDomain::Enum, "High")
                                                                        .Where("TILE", 16u)
                                                                        .Where("SHADE", AxisValueDomain::Type, "Phong")
                                                                        .Single();
    runner.Check(pinned.has_value() &&
                 *pinned == PackVariantKey(std::array<uint32_t, 4>{ 1u, 1u, 1u, 1u }, k_Radices),
                 "four pinned axes give the one matching key");
    const std::expected<VariantKey, lodestone::QueryError> partial =
        index.Query().Where("DITHER", true).Where("TILE", 16u).Single();
    runner.Check(!partial.has_value() && partial.error().Code == QueryErrorCode::AmbiguousVariant,
                 "two pinned axes of four are ambiguous");
    if (!partial.has_value())
    {
        runner.Check(partial.error().AxisName == "QUALITY, SHADE", "the error names the unpinned axes, in axis order");
        runner.Check(partial.error().Detail == 6u, "the error counts the matches");
    }
    const std::expected<VariantKey, lodestone::QueryError> badSingle = index.Query().Where("DITHR", true).Single();
    runner.Check(!badSingle.has_value() && badSingle.error().Code == QueryErrorCode::UnknownAxis &&
                 badSingle.error().Suggestion == "DITHER",
                 "a builder error reaches Single whole, with its suggestion");

    const std::vector<std::byte> oneVariantBytes = BuildManifest(&KeepFirstOnly, &AllActive);
    const ManifestResult<EnvironmentView> oneVariantOpened = OpenEnvironment(oneVariantBytes, 0u, 0u);
    runner.Check(oneVariantOpened.has_value(), "the one-variant manifest opens");
    if (oneVariantOpened.has_value())
    {
        const ManifestIndex oneVariant{ oneVariantOpened.value() };
        const std::expected<VariantKey, lodestone::QueryError> only = oneVariant.Query().Single();
        runner.Check(only.has_value() && *only == PackVariantKey(std::array<uint32_t, 4>{ 0u, 0u, 0u, 0u }, k_Radices),
                     "a query with no constraint on a one-variant module gives that variant");
    }

    // The query errors fill Suggestion through NearestName. A renderer calls it the same way for a name
    // of its own, through the client header alone.
    runner.BeginSection("NearestName gives the query's suggestion to any caller");
    const std::vector<std::string_view> axisNames = index.AxisNames();
    runner.Check(lodestone::NearestName("QUALTY", axisNames) == "QUALITY", "one missing letter finds the axis");
    runner.Check(lodestone::NearestName("TLIE", axisNames) == "TILE", "a transposition counts as one edit");
    runner.Check(lodestone::NearestName("BLOOM", axisNames).empty(), "a name far from every candidate gives nothing");

    // manifest where one axis value was left out: queries are still valid, because it is an axis value,
    // but it was not cooked into any variant so the query resolves to an empty span
    runner.BeginSection("a valid value with no cooked variant returns an empty result");
    const std::vector<std::byte> sparseBytes = BuildManifest(&KeepSmallTiles, &AllActive);
    const ManifestResult<EnvironmentView> sparseOpened = OpenEnvironment(sparseBytes, 0u, 0u);
    runner.Check(sparseOpened.has_value(), "the sparse manifest opens");
    if (sparseOpened.has_value())
    {
        const ManifestIndex sparse{ sparseOpened.value() };
        runner.Check(sparse.Enumerate().size() == 24u, "only the kept variants exist");
        runner.Check(sparse.View().Module().AxisValueCount(k_TileAxis) == 3u,
                     "the axis schema still declares all three TILE values");
        runner.Check(KeyCount(sparse.Query().Where("TILE", 8u)) == 12u, "a present value still matches");
        const ManifestQueryBuilder absentValue = sparse.Query().Where("TILE", 32u);
        runner.Check(absentValue.IsValid(), "a declared value is not an error");
        runner.Check(KeyCount(absentValue) == 0u, "but it names no cooked variant, so the result is empty");
        const std::expected<VariantKey, lodestone::QueryError> absentSingle = absentValue.Single();
        runner.Check(!absentSingle.has_value() &&
                     absentSingle.error().Code == QueryErrorCode::NoVariantForConstraints,
                     "Single reports the empty result as NoVariantForConstraints");
    }

    // An ActiveWhen gate appears at the query layer as a structured hole, not as a schema field: the
    // query layer has no ActiveWhen concept. Here SHADE is gated on DITHER: when DITHER is false, SHADE
    // is pinned to its first value (Lambert), so no variant has DITHER false and SHADE Phong.
    // an ActiveWhen gate is a policy-level selection filter, so it's a structured hole in the manifest
    // the manifest still declares all axis values, but some combinations are absent from the cooked variants
    runner.BeginSection("an ActiveWhen-gated axis is a hole in the key set, not a schema change");
    const std::vector<std::byte> gatedBytes =
        BuildManifest([](const std::array<uint32_t, 4>& digits)
                      { return digits[k_DitherAxis] != 0u || digits[k_ShadeAxis] == 0u; },
                      &ShadeActiveWhenDither);
    const ManifestResult<EnvironmentView> gatedOpened = OpenEnvironment(gatedBytes, 0u, 0u);
    runner.Check(gatedOpened.has_value(), "the gated manifest opens");
    if (gatedOpened.has_value())
    {
        const ManifestIndex gated{ gatedOpened.value() };
        // if DITHER is false, SHADE is set to Lambert
        // if DITHER is true, SHADE is allowed to vary fully without restriction
        runner.Check(gated.Enumerate().size() == 27u, "the gated-off combinations are absent");
        // as noted: the axis values aren't changed. this is important for the query layer
        // to behave consistently between cooks that may gate values (so code doesn't need to respond differently)
        runner.Check(gated.View().Module().AxisValueCount(k_ShadeAxis) == 2u,
                     "the gated axis still declares both values in the schema");
        runner.Check(KeyCount(gated.Query().Where("SHADE", AxisValueDomain::Type, "Phong")) == 9u,
                     "the gated value exists only where its parent enables it");
        const ManifestQueryBuilder gatedOff =
            gated.Query().Where("DITHER", false).Where("SHADE", AxisValueDomain::Type, "Phong");
        runner.Check(gatedOff.IsValid(), "the gated-off combination uses two valid values");
        runner.Check(KeyCount(gatedOff) == 0u, "yet it names no variant");

        // The gated-off variants store SHADE at its default digit (Lambert), but SHADE is inactive there.
        // A digit test alone returns them for a Lambert query. The axis-active mask removes them.
        runner.Check(KeyCount(gated.Query().Where("SHADE", AxisValueDomain::Type, "Lambert")) == 9u,
                     "a gated axis at its default value matches only where the axis is active");
        const std::array<std::string_view, 1> phong{ "Phong" };
        runner.Check(KeyCount(gated.Query().WhereNoneOf("SHADE", AxisValueDomain::Type, phong)) == 9u,
                     "WhereNoneOf on a gated axis also selects only where the axis is active");
        runner.Check(KeyCount(gated.Query().Where("DITHER", false)) == 9u,
                     "a query that does not name the gated axis still returns the gated-off variants");
        const std::expected<VariantKey, lodestone::QueryError> gatedSingle = gated.Query().Where("DITHER", false).Single();
        runner.Check(!gatedSingle.has_value() && gatedSingle.error().AxisName == "QUALITY, TILE",
                     "an axis that is inactive in every match does not vary, so Single does not name it");

        const VariantKey gatedOffKey = PackVariantKey(std::array<uint32_t, 4>{ 0u, 0u, 0u, 0u }, k_Radices);
        const std::vector<lodestone::QueryAxisValue> decoded = gated.Decode(gatedOffKey);
        runner.Check(!decoded[k_ShadeAxis].Active && decoded[k_DitherAxis].Active,
                     "Decode marks the gated axis inactive, and the others active");
        bool enumerateAgrees = true;
        for (const DecodedVariant& variant : gated.Enumerate())
        {
            const bool ditherOn = variant.Values[k_DitherAxis].IntegralValue != 0u;
            enumerateAgrees = enumerateAgrees && variant.Values[k_ShadeAxis].Active == ditherOn;
        }
        runner.Check(enumerateAgrees, "Enumerate marks SHADE active exactly where DITHER is on");
    }

    // L4: a WhenActive pin constrains a gated axis only where the axis is active, so it keeps the gated-off
    // variants. A preset applies one set of pins to any module, and skips a pin on an axis the module does not
    // declare. The gated manifest holds 27 variants: 9 with DITHER off (SHADE inactive), and 18 with DITHER on.
    runner.BeginSection("a WhenActive pin keeps the gated-off variants, and a preset skips an undeclared axis");
    if (gatedOpened.has_value())
    {
        const ManifestIndex gated{ gatedOpened.value() };
        const VariantKey gatedOffKey = PackVariantKey(std::array<uint32_t, 4>{ 0u, 0u, 0u, 0u }, k_Radices);
        runner.Check(KeyCount(gated.Query().Where("SHADE", AxisValueDomain::Type, "Phong")) == 9u,
                     "a Required pin on a gated axis removes the gated-off variants");
        // Phong is digit 1 and the gated-off variants store digit 0, so this row also proves that filterKeys
        // keeps digit 0 in the key band for a WhenActive constraint
        runner.Check(KeyCount(gated.Query().WhereWhenActive("SHADE", AxisValueDomain::Type, "Phong")) == 18u,
                     "a WhenActive pin on a gated axis keeps the 9 gated-off variants and the 9 active Phong variants");
        runner.Check(KeyCount(gated.Query().WhereWhenActive("SHADE", AxisValueDomain::Type, "Phong").Where("DITHER", true)) == 9u,
                     "a WhenActive pin and a pin on its gate select only the active Phong variants");
        runner.Check(KeyCount(gated.Query()
                                  .Where("SHADE", AxisValueDomain::Type, "Lambert")
                                  .WhereWhenActive("SHADE", AxisValueDomain::Type, "Phong")) == 27u,
                     "a Required and a WhenActive pin on one axis merge to WhenActive over both values");

        QueryPreset preset;
        preset.Pin("DITHER", false, PinMode::Required);
        preset.Pin("QUALITY", AxisValueDomain::Enum, "Low", PinMode::Required);
        preset.Pin("TILE", 8u, PinMode::Required);
        preset.Pin("SHADE", AxisValueDomain::Type, "Phong", PinMode::WhenActive);
        preset.Pin("FOG", true, PinMode::Required);
        const ManifestQueryBuilder applied = gated.Query().Apply(preset);
        runner.Check(applied.IsValid(), "a pin on an undeclared axis is not an error");
        runner.Check(applied.Notes().size() == 1u &&
                         applied.Notes().front().Code == lodestone::QueryNoteCode::PinAxisNotDeclared &&
                         applied.Notes().front().AxisName == "FOG",
                     "a pin on an undeclared axis gives one note that names the axis");
        const std::expected<VariantKey, lodestone::QueryError> presetSingle = applied.Single();
        runner.Check(presetSingle.has_value() && presetSingle.value() == gatedOffKey,
                     "a WhenActive pin does not stop Single from returning a gated-off variant");
        const std::array<std::string_view, 1> mainStage{ "MainCS" };
        const std::expected<VariantKey, lodestone::QueryError> presetSingleFor = applied.SingleFor(mainStage);
        runner.Check(presetSingleFor.has_value() && presetSingleFor.value() == gatedOffKey,
                     "SingleFor takes the same preset");

        const ManifestQueryBuilder repeated = gated.Query().Where("TILE", 8u).Where("TILE", 8u);
        runner.Check(repeated.IsValid() && repeated.Notes().size() == 1u &&
                         repeated.Notes().front().Code == lodestone::QueryNoteCode::DuplicateValue &&
                         repeated.Notes().front().AxisName == "TILE",
                     "a value that the axis constraint already names gives one note");
        const std::array<uint32_t, 2> eightTwice{ 8u, 8u };
        runner.Check(gated.Query().WhereAnyOf("TILE", eightTwice).Notes().size() == 1u,
                     "a value repeated within one call gives one note");
        const std::array<uint32_t, 1> eight{ 8u };
        const std::array<uint32_t, 1> sixteen{ 16u };
        runner.Check(gated.Query().WhereNoneOf("TILE", eight).WhereNoneOf("TILE", sixteen).Notes().empty(),
                     "the overlap of two WhereNoneOf complements gives no note, because the caller named no value twice");

        QueryPreset misspelled;
        misspelled.Pin("SHADE", AxisValueDomain::Type, "Phongg", PinMode::WhenActive);
        const ManifestQueryBuilder misspelledQuery = gated.Query().Apply(misspelled);
        runner.Check(!misspelledQuery.IsValid() &&
                         misspelledQuery.Errors().front().Code == QueryErrorCode::ValueNotInAxis &&
                         misspelledQuery.Notes().empty(),
                     "a value that a declared axis does not have is an error, not a note");
    }

    // Keys are per module, and each profile applies its own policy, so two profiles of one module can cook
    // different subsets. An index reads one environment, so it answers for that profile alone.
    runner.BeginSection("each profile keys its own subset of one module");
    {
        const PermutationSpace space = MakeSpace();
        lodestone::CookedLibrary library;
        library.ModuleNames = { "QueryTestModule" };
        library.Profiles = { MakeProfile("wgsl"), MakeProfile("mobile") };
        library.Environments.emplace_back(BuildModule(space, &KeepAll, &AllActive));
        library.Environments.emplace_back(BuildModule(space, &KeepSmallTiles, &AllActive));
        const std::vector<std::byte> twoProfileBytes = EmitBytes(library);

        const ManifestResult<EnvironmentView> full = OpenEnvironment(twoProfileBytes, 0u, 0u);
        const ManifestResult<EnvironmentView> pruned = OpenEnvironment(twoProfileBytes, 1u, 0u);
        runner.Check(full.has_value() && pruned.has_value(), "both environments open");
        if (full.has_value() && pruned.has_value())
        {
            const ManifestIndex fullIndex{ *full };
            const ManifestIndex prunedIndex{ *pruned };
            runner.Check(fullIndex.Enumerate().size() == k_TotalVariants, "the first profile holds every variant");
            runner.Check(prunedIndex.Enumerate().size() == 24u, "the second profile holds only its subset");
            runner.Check(KeyCount(fullIndex.Query().Where("TILE", 32u)) == 12u &&
                         KeyCount(prunedIndex.Query().Where("TILE", 32u)) == 0u,
                         "one query gives each profile its own answer");
            runner.Check(fullIndex.Decode(knownKey)[k_TileAxis].IntegralValue ==
                             prunedIndex.Decode(knownKey)[k_TileAxis].IntegralValue,
                         "one key decodes the same way in both profiles");
        }
    }

    // Two modules that declare TILE store it once when their values agree in order. The module's value mask
    // selects its values, so a digit still means the module's own value.
    runner.BeginSection("two modules share a root axis when their values agree in order");
    {
        const std::array<uint32_t, 3> smallTiles{ 8u, 16u, 32u };
        const std::array<uint32_t, 3> largeTiles{ 16u, 32u, 64u };
        const std::array<uint32_t, 2> reversedTiles{ 32u, 8u };
        const PermutationSpace smallSpace = MakeTileSpace(smallTiles);
        const PermutationSpace largeSpace = MakeTileSpace(largeTiles);
        const PermutationSpace reversedSpace = MakeTileSpace(reversedTiles);

        lodestone::CookedLibrary library;
        library.ModuleNames = { "SmallTiles", "LargeTiles", "ReversedTiles" };
        library.Profiles = { MakeProfile("wgsl") };
        library.Environments.emplace_back(BuildTileModule("SmallTiles", smallSpace));
        library.Environments.emplace_back(BuildTileModule("LargeTiles", largeSpace));
        library.Environments.emplace_back(BuildTileModule("ReversedTiles", reversedSpace));
        const std::vector<std::byte> sharedBytes = EmitBytes(library);

        const ManifestResult<BundleView> bundle = BundleView::Open(sharedBytes);
        runner.Check(bundle.has_value(), "the three-module bundle opens");
        if (bundle.has_value())
        {
            runner.Check(bundle->Axes().size() == 2u,
                         "the two ordered modules share one root axis, and the reversed one gets its own");
            runner.Check(bundle->Module(0u).ModuleAxes()[0].AxisIndex == bundle->Module(1u).ModuleAxes()[0].AxisIndex,
                         "the small and large modules name the same root axis");
            runner.Check(bundle->AxisValues(bundle->Module(1u).ModuleAxes()[0].AxisIndex).size() == 4u,
                         "the shared root axis holds the union of the values");
        }

        const ManifestResult<EnvironmentView> large = OpenEnvironment(sharedBytes, 0u, 1u);
        const ManifestResult<EnvironmentView> reversed = OpenEnvironment(sharedBytes, 0u, 2u);
        runner.Check(large.has_value() && reversed.has_value(), "the module environments open");
        if (large.has_value() && reversed.has_value())
        {
            const ManifestIndex largeIndex{ *large };
            const ManifestIndex reversedIndex{ *reversed };
            runner.Check(largeIndex.Decode(VariantKey{ 0u })[0].IntegralValue == 16u &&
                             largeIndex.Decode(VariantKey{ 2u })[0].IntegralValue == 64u,
                         "a digit of the large module decodes to the large module's value");
            runner.Check(KeyCount(largeIndex.Query().Where("TILE", 64u)) == 1u, "a value only one module has resolves");
            runner.Check(!largeIndex.Query().Where("TILE", 8u).IsValid(),
                         "a value only the other module has is not in this module's axis");
            runner.Check(reversedIndex.Decode(VariantKey{ 0u })[0].IntegralValue == 32u,
                         "the reversed module keeps its own value order");
        }
    }

    CheckAxisRelevance(runner);

    CheckAxisRelevanceValidator(runner);

    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
