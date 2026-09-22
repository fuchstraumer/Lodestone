#include "ShaderManifestIndex.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "VariantKey.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The query surface (`ManifestIndex` + `ManifestQueryBuilder`) is the one client object a renderer
// drives by hand, so it must reject a bad query with an error, never a crash. This test builds a
// manifest inline through the real emitter, so it exercises the true emit -> read -> query path with
// no asset and no compiler. The module carries one axis of each value domain: a Boolean axis, an Enum
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
using lodestone::QueryErrorCode;
using lodestone::RawEnumCase;
using lodestone::RawInterfaceImpl;
using lodestone::ShaderManifestView;
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

    axes.emplace_back("SHADE", OrdinalValues(2u, /*as_enum=*/false), AxisKind::Technique,
                      EarliestBindingTime::Cook, AxisValueDomain::Type,
                      /*active_when=*/std::string{}, /*root_name=*/"IShade",
                      std::vector<RawInterfaceImpl>{ RawInterfaceImpl{ "TestShadeModule", "Lambert" },
                                                     RawInterfaceImpl{ "TestShadeModule", "Phong" } });

    return PermutationSpace{ std::move(axes) };
}

// Emits the manifest bytes for the four-axis module, keeping only the variants `keep` accepts. The
// space and the module live only for the emit, because the returned bytes are self-contained. A subset
// models a manifest whose cook did not emit every combination: a policy allow-list, or an ActiveWhen
// gate that pins a child axis when its parent is off. The axis schema stays whole either way, because
// the space is unchanged; only the key set shrinks.
template<typename Keep>
std::vector<std::byte> BuildManifest(Keep keep)
{
    const PermutationSpace space = MakeSpace();

    lodestone::CookedModule module;
    module.Name = "QueryTestModule";
    module.Space = &space;

    module.EntryPoints.push_back(
        lodestone::LibraryEntryPoint{ .Name = "MainCS", .Stage = lodestone::ShaderStageKind::Compute });
    module.Sources.emplace_back("// wgsl for the test module");

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
                    variant.ResourceListIndex = 0u;
                    variant.FootprintListIndex = 0u;
                    variant.SourceIndices.push_back(0u);
                    variant.VisibilityIndices.push_back(0u);
                    variant.RasterIndices.push_back(0u);
                    variant.Workgroups.emplace_back(lodestone::WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u });
                    module.Variants.emplace_back(std::move(variant));
                    ++index;
                }
            }
        }
    }

    module.SpaceSize = static_cast<uint64_t>(module.VariantKeys.size());

    const std::string manifest = EmitShaderManifest(module);
    std::vector<std::byte> bytes(manifest.size());
    std::memcpy(bytes.data(), manifest.data(), manifest.size());
    return bytes;
}

// The full cross product: every combination is a cooked variant.
std::vector<std::byte> BuildManifestBytes()
{
    return BuildManifest([](const std::array<uint32_t, 4>&) { return true; });
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
    const lodestone::ManifestResult<ShaderManifestView> opened = ShaderManifestView::Open(bytes);
    runner.Check(opened.has_value(), "the emitter produces a manifest the reader accepts");
    if (!opened.has_value())
    {
        return runner.Report();
    }

    const ManifestIndex index{ opened.value() };

    runner.BeginSection("the index enumerates and decodes every variant");
    runner.Check(index.Enumerate().size() == k_TotalVariants, "Enumerate returns every variant");
    runner.Check(index.View().Axes().size() == 4u, "the manifest carries four axes");

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

    runner.BeginSection("First returns the smallest matching key");
    const ManifestQueryBuilder tileQuery = index.Query().Where("TILE", 16u);
    const lodestone::QueryResult<VariantKey> firstKey = tileQuery.First();
    const lodestone::QueryResult<std::vector<VariantKey>> tileKeys = tileQuery.Keys();
    runner.Check(firstKey.has_value() && tileKeys.has_value(), "both terminals resolve");
    if (firstKey.has_value() && tileKeys.has_value() && !tileKeys->empty())
    {
        runner.Check(*firstKey == tileKeys->front(), "First equals the first of the sorted keys");
    }

    runner.BeginSection("VariantsFromKeys agrees with Variants");
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
    runner.Check(!unknownAxis.First().has_value() &&
                 unknownAxis.First().error() == QueryErrorCode::UnknownAxis,
                 "First returns the query error, not a variant");

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

    // manifest where one axis value was left out: queries are still valid, because it is an axis value,
    // but it was not cooked into any variant so the query resolves to an empty span
    runner.BeginSection("a valid value with no cooked variant returns an empty result");
    const std::vector<std::byte> sparseBytes =
        BuildManifest([](const std::array<uint32_t, 4>& digits) { return digits[k_TileAxis] != 2u; });
    const lodestone::ManifestResult<ShaderManifestView> sparseOpened = ShaderManifestView::Open(sparseBytes);
    runner.Check(sparseOpened.has_value(), "the sparse manifest opens");
    if (sparseOpened.has_value())
    {
        const ManifestIndex sparse{ sparseOpened.value() };
        runner.Check(sparse.Enumerate().size() == 24u, "only the kept variants exist");
        runner.Check(sparse.View().Axis(k_TileAxis).ValueCount == 3u,
                     "the axis schema still declares all three TILE values");
        runner.Check(KeyCount(sparse.Query().Where("TILE", 8u)) == 12u, "a present value still matches");
        const ManifestQueryBuilder absentValue = sparse.Query().Where("TILE", 32u);
        runner.Check(absentValue.IsValid(), "a declared value is not an error");
        runner.Check(KeyCount(absentValue) == 0u, "but it names no cooked variant, so the result is empty");
        runner.Check(!absentValue.First().has_value() &&
                     absentValue.First().error() == QueryErrorCode::NoVariantForConstraints,
                     "First reports no variant for the constraints");
    }

    // An ActiveWhen gate appears at the query layer as a structured hole, not as a schema field: the
    // query layer has no ActiveWhen concept. Here SHADE is gated on DITHER: when DITHER is false, SHADE
    // is pinned to its first value (Lambert), so no variant has DITHER false and SHADE Phong.
    // an ActiveWhen gate is a policy-level selection filter, so it's a structured hole in the manifest
    // the manifest still declares all axis values, but some combinations are absent from the cooked variants
    runner.BeginSection("an ActiveWhen-gated axis is a hole in the key set, not a schema change");
    const std::vector<std::byte> gatedBytes =
        BuildManifest([](const std::array<uint32_t, 4>& digits)
                      { return digits[k_DitherAxis] != 0u || digits[k_ShadeAxis] == 0u; });
    const lodestone::ManifestResult<ShaderManifestView> gatedOpened = ShaderManifestView::Open(gatedBytes);
    runner.Check(gatedOpened.has_value(), "the gated manifest opens");
    if (gatedOpened.has_value())
    {
        const ManifestIndex gated{ gatedOpened.value() };
        // if DITHER is false, SHADE is set to Lambert
        // if DITHER is true, SHADE is allowed to vary fully without restriction
        runner.Check(gated.Enumerate().size() == 27u, "the gated-off combinations are absent");
        // as noted: the axis values aren't changed. this is important for the query layer
        // to behave consistently between cooks that may gate values (so code doesn't need to respond differently)
        runner.Check(gated.View().Axis(k_ShadeAxis).ValueCount == 2u,
                     "the gated axis still declares both values in the schema");
        runner.Check(KeyCount(gated.Query().Where("SHADE", AxisValueDomain::Type, "Phong")) == 9u,
                     "the gated value exists only where its parent enables it");
        const ManifestQueryBuilder gatedOff =
            gated.Query().Where("DITHER", false).Where("SHADE", AxisValueDomain::Type, "Phong");
        runner.Check(gatedOff.IsValid(), "the gated-off combination uses two valid values");
        runner.Check(KeyCount(gatedOff) == 0u, "yet it names no variant");
        runner.Check(!gatedOff.First().has_value() &&
                     gatedOff.First().error() == QueryErrorCode::NoVariantForConstraints,
                     "First reports no variant for the gated-off combination");
    }

    return runner.Report();
}

#ifdef __clang__
#pragma clang diagnostic pop
#endif
