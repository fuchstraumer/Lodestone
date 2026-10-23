#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "VariantKey.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "permute/PolicyDocument.hpp"
#include "TestHarness.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

// the space has the shape of OceanFft's: two independent axes, and one gated on its parent

using lodestone::AxisKind;
using lodestone::AxisValueDomain;
using lodestone::CanonicalAssignment;
using lodestone::CookResult;
using lodestone::EarliestBindingTime;
using lodestone::PermutationAssignment;
using lodestone::PermutationAxis;
using lodestone::PermutationBinding;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::StderrDiagnosticSink;
using lodestone::TargetCookPolicy;
using lodestone::VariantDescriptor;
using lodestone::VariantKey;
using lodestone::VariantSet;

namespace
{

const TargetCookPolicy k_UnboundedTargetPolicy{};

// The space owns its axes, so every axis reference below names a position in it. `ParentIndex` 1 is
// TEST_USE_WAVE_OPS.
const PermutationSpace k_TestSpace{
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
    lodestone::StderrDiagnosticSink sink;

    const CookResult<VariantSet> enumerated = k_TestSpace.EnumerateVariants(k_UnboundedTargetPolicy, sink);
    if (!runner.CheckOk(enumerated, "the test space enumerates"))
    {
        return runner.Report();
    }

    const VariantSet& variants = enumerated.value();

    runner.BeginSection("the space expands to the shape the axes describe");
    runner.CheckEqual(variants.SpaceSize, k_ExpectedSpaceSize,
                      "the dense index range counts every axis, holes included");
    runner.CheckEqual(variants.Variants.size(), k_ExpectedVariantCount,
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
        if (k_TestSpace.ComputeVariantKey(canonical) != descriptor.Key)
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
        runner.CheckEqual(FindBinding(waveOpsOff->Active, &k_WaveSizeAxis), nullptr,
                          "a disabled axis is absent from Active, so it cannot reach the linker");
        runner.CheckEqual(waveOpsOff->Active.size(), 2u, "Active holds only the axes the variant uses");
        runner.CheckEqual(waveOpsOff->Canonical.size(), k_TestSpace.AxisCount(),
                          "Canonical holds every axis in the space");

        const lodestone::PermutationBinding* canonicalWaveSize =
            FindBinding(waveOpsOff->Canonical, &k_WaveSizeAxis);
        runner.Check(canonicalWaveSize != nullptr &&
                         canonicalWaveSize->Value == k_WaveSizeAxis.GetCanonicalValue(),
                     "a disabled axis takes its first value in Canonical");
    }

    runner.BeginSection("a partial assignment resolves to one variant");
    // The design claim: name only the axes you care about, and canonicalization supplies the rest.
    PermutationAssignment partial;
    partial.push_back(PermutationBinding{ .Axis = &k_SizeAxis, .Value = PermutationValue{ 512u } });

    const CanonicalAssignment filled = k_TestSpace.CanonicalizeAssignment(partial);
    runner.CheckEqual(filled.size(), k_TestSpace.AxisCount(),
                      "a partial assignment canonicalizes to every axis");

    const VariantKey partialKey = k_TestSpace.ComputeVariantKey(filled);
    // TEST_SIZE=512 is digit 2, and the missing axes take digit 0: (2 * 2 + 0) * 3 + 0
    runner.CheckEqual(partialKey, VariantKey{ 12u }, "the partial assignment keys the variant with wave operations off");

    const auto match = std::ranges::find(variants.Variants, partialKey, &VariantDescriptor::Key);
    if (runner.Check(match != variants.Variants.end(), "the partial assignment names a variant the cook produced"))
    {
        const lodestone::PermutationBinding* size = FindBinding(match->Active, &k_SizeAxis);
        const lodestone::PermutationBinding* waveOps = FindBinding(match->Active, &k_UseWaveOpsAxis);
        runner.Check(size != nullptr && size->Value == PermutationValue{ 512u }, "that variant has TEST_SIZE=512");
        runner.Check(waveOps != nullptr && waveOps->Value == PermutationValue{ false }, "that variant has wave operations off");
    }

    // A key packs digits against the axis order and value counts, first axis most significant. These values pin that
    // packing, so a change to it fails here and not in a renderer that persisted a key.
    runner.BeginSection("variant keys are stable known values");
    const auto keyOf = [](uint32_t size, bool wave_ops, uint32_t wave_size)
    {
        PermutationAssignment assignment{ PermutationBinding{ .Axis = &k_SizeAxis, .Value = PermutationValue{ size } },
                                          PermutationBinding{ .Axis = &k_UseWaveOpsAxis, .Value = PermutationValue{ wave_ops } } };
        if (wave_ops)
        {
            assignment.push_back(PermutationBinding{ .Axis = &k_WaveSizeAxis, .Value = PermutationValue{ wave_size } });
        }
        return k_TestSpace.ComputeVariantKey(k_TestSpace.CanonicalizeAssignment(assignment));
    };
    runner.CheckEqual(keyOf(128u, false, 0u), VariantKey{ 0u }, "the first value of every axis is key 0");
    runner.CheckEqual(keyOf(512u, true, 64u), VariantKey{ 17u }, "TEST_SIZE=512, wave operations on, wave size 64: (2 * 2 + 1) * 3 + 2");
    runner.CheckEqual(keyOf(1024u, false, 0u), VariantKey{ 18u }, "TEST_SIZE=1024, wave operations off: (3 * 2 + 0) * 3 + 0");
    runner.CheckEqual(keyOf(1024u, true, 64u), VariantKey{ 23u }, "the last value of every axis is the space size less one");

    runner.BeginSection("MaxVariants is the largest variant count that cooks");
    for (const uint32_t budget : { k_ExpectedVariantCount - 1u, k_ExpectedVariantCount, k_ExpectedVariantCount + 1u })
    {
        const TargetCookPolicy policy{ .MaxVariants = budget };
        lodestone::RecordingDiagnosticSink budgetSink;
        const CookResult<VariantSet> budgeted = k_TestSpace.EnumerateVariants(policy, budgetSink);
        const std::string claim = std::format("{} variants against a budget of {}", k_ExpectedVariantCount, budget);
        if (budget < k_ExpectedVariantCount)
        {
            runner.CheckError(budgeted, lodestone::CookError::PermutationVariantBudgetExceeded, claim);
        }
        else
        {
            runner.CheckOk(budgeted, claim);
        }
    }

    runner.BeginSection("a module with no registered space still cooks");
    const PermutationSpace emptySpace{ {} };
    const CookResult<VariantSet> emptyVariants = emptySpace.EnumerateVariants(k_UnboundedTargetPolicy, sink);

    if (runner.CheckOk(emptyVariants, "an empty space enumerates rather than fails"))
    {
        runner.CheckEqual(emptyVariants.value().Variants.size(), 1u, "an empty space gives exactly one variant");
        runner.CheckEqual(emptyVariants.value().SpaceSize, 1, "an empty space has an index range of one");
        runner.CheckEqual(emptyVariants.value().Variants.front().Index, 0, "that one variant takes index zero");
    }

    return runner.Report();
}
