#include "CookerErrors.hpp"
#include "TestHarness.hpp"

#include "model/CookedLibrary.hpp"
#include "emit/DedupeReport.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "permute/PermutationValue.hpp"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** Proves that dedup changes what the tables cost and never what the cook measures.
 *
 * `--no-dedupe` is the A/B control arm, so both arms must reach the same conclusions about the
 * shader. Every measurement therefore has to read content. A measurement that reads an interner index
 * gives one answer with dedup on and the opposite answer with dedup off, and the module then fails
 * its own policy in the arm that exists to debug the other one.
 *
 * This test needs no Slang, no compiler, and no asset. */
using namespace lodestone;

namespace
{

constexpr std::string_view k_ActiveEntryPoint = "ActiveCS";
constexpr std::string_view k_InertEntryPoint = "InertCS";
constexpr std::string_view k_ConditionalEntryPoint = "ConditionalCS";

PermutationAxis MakeBoolAxis(std::string name)
{
    return PermutationAxis{ std::move(name),
                            { PermutationValue{ false }, PermutationValue{ true } },
                            PermutationAxis::k_NoParent,
                            PermutationValue{},
                            AxisKind::Capability,
                            EarliestBindingTime::Cook,
                            AxisValueDomain::Boolean };
}

ReflectedBinding MakeSharedBinding()
{
    ReflectedBinding binding;
    binding.Name = "Waves";
    binding.Placement = BoundPlacement{ .Group = 0u, .Binding = 0u };
    binding.Kind = BindingKind::StorageBuffer;
    binding.ElementStride = 16u;
    binding.ArrayCount = 1u;
    binding.Shape = ResourceShape::Buffer;
    return binding;
}

CompiledEntryPoint MakeEntryPoint(std::string_view name, std::string code)
{
    CompiledEntryPoint entryPoint;
    entryPoint.Name = name;
    entryPoint.Code = std::move(code);
    entryPoint.Reflection.Name = entryPoint.Name;
    entryPoint.Reflection.Stage = ShaderStageKind::Compute;
    entryPoint.Reflection.Workgroup = WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u };
    entryPoint.Reflection.UsedBindingIndices.push_back(0u);
    return entryPoint;
}

/** `ActiveCS` reads the first axis and nothing else. `InertCS` reads neither. So the correct answer
 * is fixed by construction, and it does not depend on how the tables were filled. */
CompiledVariant MakeVariant(uint32_t index, bool first_axis_value, bool second_axis_value)
{
    CompiledVariant variant;
    variant.VariantIndex = index;
    variant.VariantSuffix = std::format("_{}_{}", first_axis_value, second_axis_value);
    variant.VariantDescription = std::format("AXIS_A={} AXIS_B={}", first_axis_value, second_axis_value);
    variant.Bindings.push_back(MakeSharedBinding());
    variant.EntryPoints.push_back(
        MakeEntryPoint(k_ActiveEntryPoint, std::format("// AXIS_A is {}\n", first_axis_value)));
    variant.EntryPoints.push_back(MakeEntryPoint(k_InertEntryPoint, "// this text never changes\n"));
    return variant;
}

/** One entry point, so the usage mask is the same on every layout. The shared layout claim is only
 * interesting when the layouts can actually be equal, and today the mask is part of the layout key. */
CompiledVariant MakeSingleEntryPointVariant(uint32_t index, bool first_axis_value)
{
    CompiledVariant variant;
    variant.VariantIndex = index;
    variant.VariantSuffix = std::format("_{}", first_axis_value);
    variant.VariantDescription = std::format("AXIS_A={}", first_axis_value);
    variant.Bindings.push_back(MakeSharedBinding());
    variant.EntryPoints.push_back(
        MakeEntryPoint(k_ActiveEntryPoint, std::format("// AXIS_A is {}\n", first_axis_value)));
    return variant;
}

/** `AppendVariantToModule` takes a `CanonicalAssignment`, so the test reaches it the way the cooker
 * does. */
CanonicalAssignment MakeAssignment(const PermutationSpace& space,
                                   const PermutationAxis& first_axis,
                                   const PermutationAxis& second_axis,
                                   bool first_axis_value,
                                   bool second_axis_value)
{
    const PermutationAssignment active{
        PermutationBinding{ .Axis = &first_axis, .Value = PermutationValue{ first_axis_value } },
        PermutationBinding{ .Axis = &second_axis, .Value = PermutationValue{ second_axis_value } }
    };

    return space.CanonicalizeAssignment(active);
}

/** `ConditionalCS` reads the first axis only when the second axis is true. So the variants that hold
 * the second axis false all share one source, and only the other group can find the first axis
 * Active. A measurement that reads one group and drops the next reports Inert here. */
CompiledVariant MakeConditionalVariant(uint32_t index, bool first_axis_value, bool second_axis_value)
{
    CompiledVariant variant;
    variant.VariantIndex = index;
    variant.VariantSuffix = std::format("_{}_{}", first_axis_value, second_axis_value);
    variant.VariantDescription = std::format("AXIS_A={} AXIS_B={}", first_axis_value, second_axis_value);
    variant.Bindings.push_back(MakeSharedBinding());
    variant.EntryPoints.push_back(
        MakeEntryPoint(k_ConditionalEntryPoint,
                       second_axis_value ? std::format("// AXIS_A is {}\n", first_axis_value)
                                         : std::string{ "// AXIS_A does not reach this text\n" }));
    return variant;
}

CookedModule BuildModule(const PermutationSpace& space, bool dedupe_enabled)
{
    InternedModule module;
    module.Name = "InfluenceModule";
    module.Space = &space;
    module.SpaceSize = 4u;
    module.EntryPoints.push_back(
        LibraryEntryPoint{ .Name = std::string{ k_ActiveEntryPoint }, .Stage = ShaderStageKind::Compute });
    module.EntryPoints.push_back(
        LibraryEntryPoint{ .Name = std::string{ k_InertEntryPoint }, .Stage = ShaderStageKind::Compute });

    if (!dedupe_enabled)
    {
        DisableDedupe(module);
    }

    uint32_t index = 0u;
    for (const bool firstAxisValue : { false, true })
    {
        for (const bool secondAxisValue : { false, true })
        {
            const CompiledVariant variant = MakeVariant(index, firstAxisValue, secondAxisValue);
            const CanonicalAssignment canonical =
                MakeAssignment(space, space.Axes()[0], space.Axes()[1], firstAxisValue, secondAxisValue);

            const CookError appended = AppendVariantToModule(module, variant, canonical);
            if (appended != CookError::Success)
            {
                module.Variants.clear();
                return FreezeModuleTables(std::move(module));
            }

            ++index;
        }
    }

    return FreezeModuleTables(std::move(module));
}

CookedModule BuildSingleEntryPointModule(const PermutationSpace& space, bool dedupe_enabled)
{
    InternedModule module;
    module.Name = "SharedLayoutModule";
    module.Space = &space;
    module.SpaceSize = 2u;
    module.EntryPoints.push_back(
        LibraryEntryPoint{ .Name = std::string{ k_ActiveEntryPoint }, .Stage = ShaderStageKind::Compute });

    if (!dedupe_enabled)
    {
        DisableDedupe(module);
    }

    uint32_t index = 0u;
    for (const bool firstAxisValue : { false, true })
    {
        const CompiledVariant variant = MakeSingleEntryPointVariant(index, firstAxisValue);
        // Only the first axis is named. Canonicalization supplies the second.
        const CanonicalAssignment canonical =
            space.CanonicalizeAssignment(PermutationAssignment{ PermutationBinding{
                .Axis = space.Axes().data(), .Value = PermutationValue{ firstAxisValue } } });

        const CookError appended = AppendVariantToModule(module, variant, canonical);
        if (appended != CookError::Success)
        {
            module.Variants.clear();
            return FreezeModuleTables(std::move(module));
        }

        ++index;
    }

    return FreezeModuleTables(std::move(module));
}

CookedModule BuildConditionalModule(const PermutationSpace& space)
{
    InternedModule module;
    module.Name = "ConditionalModule";
    module.Space = &space;
    module.SpaceSize = 4u;
    module.EntryPoints.push_back(
        LibraryEntryPoint{ .Name = std::string{ k_ConditionalEntryPoint }, .Stage = ShaderStageKind::Compute });

    uint32_t index = 0u;
    for (const bool firstAxisValue : { false, true })
    {
        for (const bool secondAxisValue : { false, true })
        {
            const CompiledVariant variant = MakeConditionalVariant(index, firstAxisValue, secondAxisValue);
            const CanonicalAssignment canonical =
                MakeAssignment(space, space.Axes()[0], space.Axes()[1], firstAxisValue, secondAxisValue);

            const CookError appended = AppendVariantToModule(module, variant, canonical);
            if (appended != CookError::Success)
            {
                module.Variants.clear();
                return FreezeModuleTables(std::move(module));
            }

            ++index;
        }
    }

    return FreezeModuleTables(std::move(module));
}

AxisInfluence InfluenceOf(const ModuleInfluence& influence,
                          std::string_view entry_point_name,
                          size_t axis_index)
{
    for (const EntryPointInfluence& entry : influence.EntryPoints)
    {
        if (entry.EntryPointName == entry_point_name && axis_index < entry.Axes.size())
        {
            return entry.Axes[axis_index];
        }
    }

    return AxisInfluence::Invalid;
}

/** If both arms filled the tables the same way, the comparison below proves nothing. */
void CheckTheTwoArmsReallyDiffer(lodestone::tests::TestRunner& runner,
                                 const CookedModule& deduped,
                                 const CookedModule& raw)
{
    runner.BeginSection("the two arms differ in cost");

    runner.Check(deduped.Variants.size() == 4u && raw.Variants.size() == 4u, "both arms hold every variant");
    runner.Check(deduped.Sources.size() == 3u, "dedup collapses eight artifacts onto three unique texts");
    runner.Check(raw.Sources.size() == 8u, "no-dedupe gives each of the eight artifacts its own entry");
    // D8b took the usage mask and the footprint out of the resource key, so one resource at one
    // place is one row however many entry points read it.
    runner.Check(deduped.Resources.size() == 1u,
                 "two entry points binding one resource at one place give one resource");
    runner.Check(deduped.ResourceLists.size() == 1u, "every variant declares the same one resource");
    runner.Check(deduped.VisibilityLists.size() == 1u,
                 "both entry points read that resource, so both see the same visibility list");
    // Four, not eight. A resource belongs to the variant, so it is interned once for each variant and
    // not once for each entry point that reads it. The sources stay at eight, because a source does
    // belong to an entry point.
    runner.Check(raw.Resources.size() == 4u,
                 "no-dedupe gives each variant its own resource entry, one for each of the four");
}

void CheckInfluenceAgrees(lodestone::tests::TestRunner& runner,
                          const CookedModule& deduped,
                          const CookedModule& raw)
{
    runner.BeginSection("influence does not depend on dedup");

    const ModuleInfluence dedupedInfluence = ComputeAxisInfluence(deduped);
    const ModuleInfluence rawInfluence = ComputeAxisInfluence(raw);

    runner.Check(InfluenceOf(dedupedInfluence, k_ActiveEntryPoint, 0u) == AxisInfluence::Active,
                 "with dedup on, the axis the shader reads is Active");
    runner.Check(InfluenceOf(rawInfluence, k_ActiveEntryPoint, 0u) == AxisInfluence::Active,
                 "with dedup off, the axis the shader reads is still Active");

    runner.Check(InfluenceOf(dedupedInfluence, k_ActiveEntryPoint, 1u) == AxisInfluence::Inert,
                 "with dedup on, the axis the shader ignores is Inert");
    runner.Check(InfluenceOf(rawInfluence, k_ActiveEntryPoint, 1u) == AxisInfluence::Inert,
                 "with dedup off, the axis the shader ignores is still Inert. This is the check that "
                 "an index comparison fails");

    runner.Check(InfluenceOf(dedupedInfluence, k_InertEntryPoint, 0u) == AxisInfluence::Inert &&
                     InfluenceOf(dedupedInfluence, k_InertEntryPoint, 1u) == AxisInfluence::Inert,
                 "with dedup on, an entry point that reads no axis is inert on both");
    runner.Check(InfluenceOf(rawInfluence, k_InertEntryPoint, 0u) == AxisInfluence::Inert &&
                     InfluenceOf(rawInfluence, k_InertEntryPoint, 1u) == AxisInfluence::Inert,
                 "with dedup off, an entry point that reads no axis is inert on both");

    runner.Check(dedupedInfluence.EntryPoints.size() == rawInfluence.EntryPoints.size(),
                 "both arms measure the same entry points");
}

void CheckSharedLayoutAgrees(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("the shared layout claim does not depend on dedup");

    const CookedModule deduped = BuildSingleEntryPointModule(space, true);
    const CookedModule raw = BuildSingleEntryPointModule(space, false);

    runner.Check(deduped.Resources.size() == 1u && raw.Resources.size() == 2u,
                 "the two arms hold the same one resource in a different number of entries");
    runner.Check(AllVariantsShareOneLayout(deduped), "with dedup on, every permutation shares one layout");
    runner.Check(AllVariantsShareOneLayout(raw),
                 "with dedup off, every permutation still shares one layout, because the claim is "
                 "about the content and not about the table size");
}

/** The claim must be false when it is false, or it says nothing when it is true. */
void CheckSharedLayoutRejectsADifference(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("the shared layout claim is falsifiable");

    CookedModule module = BuildSingleEntryPointModule(space, true);
    runner.Check(AllVariantsShareOneLayout(module), "the module starts with one shared layout");

    ReflectedBinding moved = module.Resources.front();
    moved.Placement = BoundPlacement{ .Group = 0u, .Binding = 7u };
    module.Resources.push_back(std::move(moved));
    module.ResourceLists.push_back(ResourceList{ static_cast<uint32_t>(module.Resources.size() - 1u) });
    module.Variants.front().ResourceListIndex = static_cast<uint32_t>(module.ResourceLists.size() - 1u);

    runner.Check(!AllVariantsShareOneLayout(module), "one variant with a different layout ends the claim");
}

/** The measurement must read every group of variants, not the first one it finds. */
void CheckEveryGroupIsMeasured(lodestone::tests::TestRunner& runner, const PermutationSpace& space)
{
    runner.BeginSection("influence reads every group");

    const CookedModule module = BuildConditionalModule(space);
    const ModuleInfluence influence = ComputeAxisInfluence(module);

    runner.Check(InfluenceOf(influence, k_ConditionalEntryPoint, 0u) == AxisInfluence::Active,
                 "the first axis is Active, and only the second group of variants says so");
    runner.Check(InfluenceOf(influence, k_ConditionalEntryPoint, 1u) == AxisInfluence::Active,
                 "the second axis decides whether the text changes, so it is Active as well");
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "DedupeInfluence" };

    const PermutationSpace space{ "TestSpace", { MakeBoolAxis("AXIS_A"), MakeBoolAxis("AXIS_B") } };

    const CookedModule deduped = BuildModule(space, true);
    const CookedModule raw = BuildModule(space, false);

    CheckTheTwoArmsReallyDiffer(runner, deduped, raw);
    CheckInfluenceAgrees(runner, deduped, raw);
    CheckSharedLayoutAgrees(runner, space);
    CheckSharedLayoutRejectsADifference(runner, space);
    CheckEveryGroupIsMeasured(runner, space);

    return runner.Report();
}
