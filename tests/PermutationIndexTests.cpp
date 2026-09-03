#include "CookerErrors.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "TestHarness.hpp"
#include "permute/PermutationValue.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

// The dense index is the key that the manifest, the generated C++, and the renderer all resolve a
// variant with. Two properties make it safe, and this file proves both without a compiler and without
// an asset.
//
// First, canonicalization can never change shader output. `Active` drives the Slang linker, and
// `Canonical` drives the index alone.
//
// Second, a caller can name only the axes it cares about. Canonicalization fills in the rest, so a
// partial assignment still resolves to one variant.
//
// The space below has the shape of the OceanFft space: two independent axes, and one axis that only
// contributes values when its parent takes the enabling value.

using lodestone::CanonicalAssignment;
using lodestone::CookResult;
using lodestone::PermutationAssignment;
using lodestone::PermutationAxis;
using lodestone::PermutationBinding;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::VariantDescriptor;
using lodestone::VariantSet;
using lodestone::AxisKind;
using lodestone::EarliestBindingTime;
using lodestone::AxisValueDomain;

namespace
{

// The space owns its axes, so every axis reference below names a position in it. `ParentIndex` 1 is
// TEST_USE_WAVE_OPS.
const PermutationSpace k_TestSpace{
    "TestSpace",
    { PermutationAxis{ "TEST_SIZE",
                       { PermutationValue{ 128u },
                         PermutationValue{ 256u },
                         PermutationValue{ 512u },
                         PermutationValue{ 1024u } },
                       AxisKind::Tuning,
                       EarliestBindingTime::Cook,
                       AxisValueDomain::Integral },
      PermutationAxis{ "TEST_USE_WAVE_OPS",
                       { PermutationValue{ false }, PermutationValue{ true } },
                       AxisKind::Capability,
                       EarliestBindingTime::Cook,
                       AxisValueDomain::Boolean },
      /** Only contributes values when TEST_USE_WAVE_OPS took the value true. */
      PermutationAxis{ "TEST_WAVE_SIZE",
                       { PermutationValue{ 16u }, PermutationValue{ 32u }, PermutationValue{ 64u } },
                       AxisKind::Tuning,
                       EarliestBindingTime::Cook,
                       AxisValueDomain::Integral,
                       "TEST_USE_WAVE_OPS == 1" } } };
const PermutationAxis& k_SizeAxis = k_TestSpace.Axes()[0];
const PermutationAxis& k_UseWaveOpsAxis = k_TestSpace.Axes()[1];
const PermutationAxis& k_WaveSizeAxis = k_TestSpace.Axes()[2];

/** 4 sizes, times 2 wave-op settings, times 3 wave sizes. The dependent axis leaves holes in it. */
constexpr int32_t k_ExpectedSpaceSize = 24;
/** For each size: one variant with wave ops off, and three with wave ops on. */
constexpr uint32_t k_ExpectedVariantCount = 16u;

const lodestone::PermutationBinding* FindBinding(const PermutationAssignment& assignment,
                                                 const PermutationAxis* axis) noexcept
{
    for (const lodestone::PermutationBinding& binding : assignment)
    {
        if (binding.Axis == axis)
        {
            return &binding;
        }
    }

    return nullptr;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "PermutationIndexTests" };

    const CookResult<VariantSet> enumerated = k_TestSpace.EnumerateVariants();
    if (!enumerated)
    {
        runner.Check(false, "the test space enumerates");
        return runner.Report();
    }

    const VariantSet& variants = enumerated.value();

    runner.BeginSection("the space expands to the shape the axes describe");
    runner.Check(variants.SpaceSize == k_ExpectedSpaceSize,
                 "the dense index range counts every axis, holes included");
    runner.Check(variants.Variants.size() == k_ExpectedVariantCount,
                 "a disabled dependent axis removes the variants it would have produced");

    runner.BeginSection("every index is unique and inside the range");
    std::vector<bool> seen(static_cast<size_t>(variants.SpaceSize), false);
    bool everyIndexIsUnique = true;
    bool everyIndexIsInRange = true;
    for (const VariantDescriptor& descriptor : variants.Variants)
    {
        if (descriptor.Index >= variants.SpaceSize)
        {
            everyIndexIsInRange = false;
            continue;
        }

        if (seen[static_cast<size_t>(descriptor.Index)])
        {
            everyIndexIsUnique = false;
        }

        seen[static_cast<size_t>(descriptor.Index)] = true;
    }

    runner.Check(everyIndexIsInRange, "no index reaches past the dense range");
    runner.Check(everyIndexIsUnique, "no two variants claim one index");

    runner.BeginSection("the retrieval path returns the index the variant already has");
    bool everyRoundTripAgrees = true;
    for (const VariantDescriptor& descriptor : variants.Variants)
    {
        const CanonicalAssignment canonical = k_TestSpace.CanonicalizeAssignment(descriptor.Active);
        if (k_TestSpace.ComputeVariantIndex(canonical) != descriptor.Index)
        {
            everyRoundTripAgrees = false;
        }
    }

    runner.Check(everyRoundTripAgrees,
                 "canonicalizing Active and indexing it returns the descriptor's own index");

    runner.BeginSection("a disabled axis leaves Active and fills Canonical");
    const VariantDescriptor* waveOpsOff = nullptr;
    for (const VariantDescriptor& descriptor : variants.Variants)
    {
        const lodestone::PermutationBinding* useWaveOps = FindBinding(descriptor.Active, &k_UseWaveOpsAxis);
        if (useWaveOps != nullptr && useWaveOps->Value == PermutationValue{ false })
        {
            waveOpsOff = &descriptor;
            break;
        }
    }

    runner.Check(waveOpsOff != nullptr, "the space produces a variant with wave ops off");
    if (waveOpsOff != nullptr)
    {
        runner.Check(FindBinding(waveOpsOff->Active, &k_WaveSizeAxis) == nullptr,
                     "a disabled axis is absent from Active, so it cannot reach the linker");
        runner.Check(waveOpsOff->Active.size() == 2u, "Active holds only the axes the variant uses");
        runner.Check(waveOpsOff->Canonical.size() == k_TestSpace.AxisCount(),
                     "Canonical holds every axis in the space");

        const lodestone::PermutationBinding* canonicalWaveSize =
            FindBinding(waveOpsOff->Canonical, &k_WaveSizeAxis);
        runner.Check(canonicalWaveSize != nullptr &&
                         canonicalWaveSize->Value == k_WaveSizeAxis.GetDefault(),
                     "a disabled axis takes its first value in Canonical");
    }

    runner.BeginSection("a partial assignment resolves to one variant");
    // The design claim: name only the axes you care about, and canonicalization supplies the rest.
    PermutationAssignment partial;
    partial.push_back(PermutationBinding{ .Axis = &k_SizeAxis, .Value = PermutationValue{ 512u } });

    const CanonicalAssignment filled = k_TestSpace.CanonicalizeAssignment(partial);
    runner.Check(filled.size() == k_TestSpace.AxisCount(),
                 "a partial assignment canonicalizes to every axis");

    const int32_t partialIndex = k_TestSpace.ComputeVariantIndex(filled);

    bool matchesRealVariant = false;
    for (const VariantDescriptor& descriptor : variants.Variants)
    {
        if (descriptor.Index == partialIndex)
        {
            matchesRealVariant = true;
        }
    }

    runner.Check(matchesRealVariant, "the partial assignment names a variant the cook produced");

    runner.BeginSection("a module with no registered space still cooks");
    const PermutationSpace emptySpace{ "", {} };
    const CookResult<VariantSet> emptyVariants = emptySpace.EnumerateVariants();

    runner.Check(emptyVariants.has_value(), "an empty space enumerates rather than fails");
    if (emptyVariants)
    {
        runner.Check(emptyVariants.value().Variants.size() == 1u, "an empty space gives exactly one variant");
        runner.Check(emptyVariants.value().SpaceSize == 1, "an empty space has an index range of one");
        runner.Check(emptyVariants.value().Variants.front().Index == 0, "that one variant takes index zero");
    }

    return runner.Report();
}
