#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"

#include "model/ContentHash.hpp"
#include "model/CookedLibrary.hpp"
#include "driver/CookerOptions.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "emit/StageDump.hpp"
#include "permute/PermutationValue.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace lodestone;

namespace
{

/** The space dump for this axis is written out in full below. Keep the axis small, because the value
 * of that golden is that a person can read it and see the whole format at once. */
PermutationAxis MakeBoolAxis(std::string name)
{
    return PermutationAxis{ std::move(name),
                            { PermutationValue{ false }, PermutationValue{ true } },
                            AxisKind::Capability,
                            EarliestBindingTime::Cook,
                            AxisValueDomain::Boolean };
}

CompiledVariant MakeVariant(uint32_t index, const std::string& suffix, std::string code)
{
    ReflectedBinding binding;
    binding.Name = "Waves";
    binding.Placement = BoundPlacement{ .Group = 0u, .Binding = 1u };
    binding.Kind = BindingKind::StorageBuffer;
    binding.ElementStride = 16u;
    binding.ArrayCount = 1u;
    binding.Shape = ResourceShape::Buffer;

    CompiledEntryPoint entryPoint;
    entryPoint.Name = "MainCS";
    entryPoint.VariantSuffix = suffix;
    entryPoint.Code = std::move(code);
    entryPoint.Reflection.Name = "MainCS";
    entryPoint.Reflection.Stage = ShaderStageKind::Compute;
    entryPoint.Reflection.Workgroup = WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u };
    entryPoint.Reflection.UsedBindingIndices.push_back(0u);

    CompiledVariant variant;
    variant.VariantIndex = index;
    variant.VariantSuffix = suffix;
    variant.VariantDescription = "USE_FOO=" + std::string{ index == 0u ? "false" : "true" };
    variant.Bindings.emplace_back(std::move(binding));
    variant.Footprints.emplace_back(BufferFootprint{ .ElementCount = 1024u, .Expression = "IFFT_SIZE * 4" });
    variant.EntryPoints.emplace_back(std::move(entryPoint));
    return variant;
}

/** This module registers no axis, so every variant canonicalizes against a space with no axes. */
const PermutationSpace k_EmptySpace{ "", {} };

/** Two variants with different text and one shared layout. The cooked dump must therefore report two
 * sources and one layout, which is the collapse the interner performed. */
InternedModule BuildTinyInternedModule()
{
    InternedModule module;
    module.Name = "TinyModule";
    module.SpaceSize = 2u;
    module.EntryPoints.push_back(LibraryEntryPoint{ .Name = "MainCS", .Stage = ShaderStageKind::Compute });

    const std::array<CompiledVariant, 2u> variants{ MakeVariant(0u, "_USE_FOO_false", "// variant zero\n"),
                                                    MakeVariant(1u, "_USE_FOO_true", "// variant one\n") };

    for (const CompiledVariant& variant : variants)
    {
        const CookError appended =
            AppendVariantToModule(module,
                                  variant,
                                  k_EmptySpace.CanonicalizeAssignment(PermutationAssignment{}));
        if (appended != CookError::Success)
        {
            module.Variants.clear();
            return module;
        }
    }

    return module;
}

CookedModule BuildTinyModule()
{
    return FreezeModuleTables(BuildTinyInternedModule());
}

bool Contains(std::string_view text, std::string_view needle) noexcept
{
    return text.contains(needle);
}

/** Two variants of one module. Stage 3 must give both the same bindings and the same attribute
 * strings, because an attribute argument does not depend on the axis values. Only stage 4 does. */
RawModule BuildRawModule()
{
    RawBinding buffer;
    buffer.Name = "Waves";
    buffer.Placement = BoundPlacement{ .Group = 0u, .Binding = 1u };
    buffer.Kind = BindingKind::StorageBuffer;
    buffer.ElementStride = 16u;
    buffer.Shape = ResourceShape::Buffer;

    RawBinding sampler;
    sampler.Name = "LinearSampler";
    sampler.Placement = BoundPlacement{ .Group = 0u, .Binding = 0u };
    sampler.Kind = BindingKind::Sampler;
    sampler.SamplerType = SamplerBindingType::Filtering;

    RawSizeAttribute attribute;
    attribute.BindingIndex = 1u;
    attribute.Kind = RawSizeAttributeKind::ElementCount;
    attribute.Arguments.emplace_back("IFFT_SIZE * 4");

    RawEntryPoint entryPoint;
    entryPoint.Name = "MainCS";
    entryPoint.Stage = ShaderStageKind::Compute;
    entryPoint.Workgroup = WorkgroupSize{ .X = 64u, .Y = 1u, .Z = 1u };
    entryPoint.TargetText = "// the target text never reaches a dump\n";
    entryPoint.UsedBindingIndices.push_back(1u);

    RawModule module;
    module.Name = "TinyModule";
    module.EntryPointNames.emplace_back("MainCS");
    module.ExternDefaults.push_back(ExternConstantDefault{ .Name = "IFFT_SIZE", .Value = 256 });

    for (uint32_t index = 0u; index < 2u; ++index)
    {
        RawVariant variant;
        variant.VariantIndex = index;
        variant.VariantSuffix = index == 0u ? "_A" : "_B";
        variant.VariantDescription = index == 0u ? "USE_FOO=false" : "USE_FOO=true";
        // Deliberately out of placement order, so the sort has something to do.
        variant.Bindings.push_back(sampler);
        variant.Bindings.push_back(buffer);
        variant.SizeAttributes.push_back(attribute);
        variant.EntryPoints.push_back(entryPoint);
        module.Variants.push_back(std::move(variant));
    }

    return module;
}

void CheckStageNames(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("stage names");

    runner.Check(ParseStageDumpKind("space") == StageDumpKind::Space, "space parses");
    runner.Check(ParseStageDumpKind("variants") == StageDumpKind::Variants, "variants parses");
    runner.Check(ParseStageDumpKind("raw") == StageDumpKind::Raw, "raw parses");
    runner.Check(ParseStageDumpKind("resolved") == StageDumpKind::Resolved, "resolved parses");
    runner.Check(ParseStageDumpKind("interned") == StageDumpKind::Interned, "interned parses");
    runner.Check(ParseStageDumpKind("cooked") == StageDumpKind::Cooked, "cooked parses");
    runner.Check(ParseStageDumpKind("all") == StageDumpKind::Invalid,
                 "all is not a kind, it is a shorthand the parser expands");
    runner.Check(ParseStageDumpKind("Cooked") == StageDumpKind::Invalid, "a name is case sensitive");
    runner.Check(ParseStageDumpKind("") == StageDumpKind::Invalid, "an empty name is rejected");

    runner.Check(ToString(StageDumpKind::Cooked) == "cooked", "a kind names itself the way it parses");
    runner.Check(MakeStageDumpFileName("OceanFft", StageDumpKind::Cooked) == "OceanFft.stage-cooked.json",
                 "the artifact name carries the module and the stage");
}

void CheckStageBits(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("stage bits");

    runner.Check(StageDumpBit(StageDumpKind::Invalid) == 0u, "the invalid kind claims no bit");
    runner.Check(StageDumpBit(StageDumpKind::Space) != StageDumpBit(StageDumpKind::Cooked),
                 "two kinds take two bits");

    CookerOptions options;
    runner.Check(!IsStageDumpRequested(options, StageDumpKind::Cooked), "no dump is requested by default");

    options.DumpStageMask = AllStageDumpBits();
    runner.Check(IsStageDumpRequested(options, StageDumpKind::Space) &&
                     IsStageDumpRequested(options, StageDumpKind::Variants) &&
                     IsStageDumpRequested(options, StageDumpKind::Raw) &&
                     IsStageDumpRequested(options, StageDumpKind::Resolved) &&
                     IsStageDumpRequested(options, StageDumpKind::Interned) &&
                     IsStageDumpRequested(options, StageDumpKind::Cooked),
                 "all requests every kind");
    runner.Check(!IsStageDumpRequested(options, StageDumpKind::Invalid),
                 "all does not request the invalid kind");
}

void CheckCommandLine(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("command line");

    const std::array<std::string_view, 4u> accepted{ "-o", "out.hpp", "--dump-stage=cooked", "a.slang" };
    const CookResult<CookerOptions> parsed = ParseCommandLine(accepted);
    runner.Check(parsed.has_value(), "--dump-stage=cooked parses");
    if (parsed)
    {
        runner.Check(IsStageDumpRequested(parsed.value(), StageDumpKind::Cooked),
                     "--dump-stage=cooked requests the cooked dump");
        runner.Check(!IsStageDumpRequested(parsed.value(), StageDumpKind::Space),
                     "--dump-stage=cooked requests nothing else");
    }

    const std::array<std::string_view, 5u> repeated{
        "-o", "out.hpp", "--dump-stage=space", "--dump-stage=variants", "a.slang"
    };
    const CookResult<CookerOptions> twice = ParseCommandLine(repeated);
    runner.Check(twice.has_value(), "the flag repeats");
    if (twice)
    {
        runner.Check(IsStageDumpRequested(twice.value(), StageDumpKind::Space) &&
                         IsStageDumpRequested(twice.value(), StageDumpKind::Variants),
                     "a repeated flag adds to the mask instead of replacing it");
    }

    const std::array<std::string_view, 4u> everything{ "-o", "out.hpp", "--dump-stage=all", "a.slang" };
    const CookResult<CookerOptions> all = ParseCommandLine(everything);
    runner.Check(all.has_value() && all.value().DumpStageMask == AllStageDumpBits(),
                 "--dump-stage=all sets every bit");

    const std::array<std::string_view, 4u> bogus{ "-o", "out.hpp", "--dump-stage=nonsense", "a.slang" };
    const CookResult<CookerOptions> rejected = ParseCommandLine(bogus);
    runner.Check(!rejected.has_value() && rejected.error() == CookError::MalformedArgument,
                 "an unknown stage name fails the command line");
}

/** The one golden literal in this file. It pins the JSON shape, the key names, the key order, and the
 * four-space indent, so a change to any of those fails here rather than in a 105-variant diff. */
void CheckSpaceDump(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("space dump");

    const PermutationSpace space{ "TinyModule", { MakeBoolAxis("USE_FOO") } };

    const std::string expected = R"({
    "stage": "space",
    "module": "TinyModule",
    "axisCount": 1,
    "spaceSize": 2,
    "axes": [
        {
            "name": "USE_FOO",
            "kind": "Capability",
            "earliestBindingTime": "Cook",
            "valueDomain": "Boolean",
            "values": [
                "false",
                "true"
            ],
            "parent": null,
            "requiredParentValue": null
        }
    ]
})";

    const std::string dump = DumpPermutationSpace("TinyModule", space);
    runner.Check(dump == expected, "the space dump matches the golden text");
    if (dump != expected)
    {
        std::println(stdout, "--- expected ---\n{}\n--- actual ---\n{}", expected, dump);
    }
}

void CheckDependentAxisDump(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("dependent axis");

    PermutationAxis child = MakeBoolAxis("FOO_DETAIL");
    child.ActiveWhen = std::string{ "USE_FOO == 1" };

    const PermutationSpace space{ "TinyModule", { MakeBoolAxis("USE_FOO"), std::move(child) } };
    const std::string dump = DumpPermutationSpace("TinyModule", space);

    runner.Check(Contains(dump, R"("parent": "USE_FOO")"), "a dependent axis names its parent");
    runner.Check(Contains(dump, R"("requiredParentValue": "true")"),
                 "a dependent axis states the value that enables it");
    runner.Check(Contains(dump, R"("spaceSize": 4)"),
                 "the space size counts the dense range with the holes included");
}

void CheckVariantDump(lodestone::tests::TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("variants dump");

    const PermutationSpace space{ "TinyModule", { MakeBoolAxis("USE_FOO") } };

    const CookResult<VariantSet> variantSet = space.EnumerateVariants(sink);
    runner.Check(variantSet.has_value(), "the space enumerates");
    if (!variantSet)
    {
        return;
    }

    const std::string dump = DumpVariantSet("TinyModule", variantSet.value());
    runner.Check(Contains(dump, R"("stage": "variants")"), "the dump names its stage");
    runner.Check(Contains(dump, R"("variantCount": 2)"), "both variants reach the dump");
    runner.Check(Contains(dump, R"("axis": "USE_FOO")"), "an assignment names its axis");
    runner.Check(Contains(dump, R"("active")") && Contains(dump, R"("canonical")"),
                 "a variant carries both assignments, because they are two different facts");

    const std::string second = DumpVariantSet("TinyModule", variantSet.value());
    runner.Check(dump == second, "two dumps of one input agree byte for byte");
}

void CheckRawPrimitives(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("raw placement and attributes");

    const RawPlacement unplaced{};
    const RawPlacement first{ BoundPlacement{ .Group = 0u, .Binding = 1u } };
    const RawPlacement second{ BoundPlacement{ .Group = 1u, .Binding = 0u } };

    runner.Check(GetBoundPlacement(unplaced) == nullptr,
                 "a default placement is not group 0 binding 0, it is no placement at all");
    runner.Check(GetBoundPlacement(first) != nullptr, "a bound placement reports itself");

    runner.Check(PlacementLess(first, second), "group orders before binding");
    runner.Check(!PlacementLess(second, first), "and the order is not symmetric");
    runner.Check(PlacementLess(first, unplaced) && !PlacementLess(unplaced, first),
                 "an unplaced resource sorts after every placed one");

    runner.Check(ArgumentCountOf(RawSizeAttributeKind::ElementCount) == 1u, "element count takes one");
    runner.Check(ArgumentCountOf(RawSizeAttributeKind::Extent2d) == 2u, "a 2d extent takes two");
    runner.Check(ArgumentCountOf(RawSizeAttributeKind::Extent3d) == 3u, "a 3d extent takes three");
    runner.Check(ArgumentCountOf(RawSizeAttributeKind::Invalid) == 0u, "the invalid kind takes none");

    runner.Check(ToString(RawSizeAttributeKind::ElementCount) == "vx_element_count",
                 "a kind names the attribute the shader author writes");
}

void CheckRawDump(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("raw dump");

    const RawModule module = BuildRawModule();
    const std::string dump = DumpRawModule(module);

    runner.Check(Contains(dump, R"("stage": "raw")"), "the dump names its stage");
    runner.Check(Contains(dump, R"("variantCount": 2)"), "both variants reach the dump");

    runner.Check(Contains(dump, R"("model": "Bound")"),
                 "placement states which access model it belongs to, so another model can join it");
    runner.Check(Contains(dump, R"("attribute": "vx_element_count")"), "an attribute names itself");
    runner.Check(Contains(dump, R"("IFFT_SIZE * 4")"),
                 "the attribute argument is still the string the author wrote. Stage 3 carries it and "
                 "does not understand it");
    runner.Check(!Contains(dump, R"("elementCount")"),
                 "stage 3 holds no evaluated size, so a reader cannot confuse not-yet-resolved with "
                 "the shader declaring nothing");

    runner.Check(Contains(dump, R"("name": "IFFT_SIZE")") && Contains(dump, R"("value": 256)"),
                 "the extern defaults travel with the module, because stage 4 needs them and must not "
                 "call Slang to get them");

    runner.Check(!Contains(dump, "the target text never reaches a dump"), "no target text reaches the dump");
    runner.Check(Contains(dump, R"("targetTextByteLength": 40)"), "the text appears as a length");

    const std::string second = DumpRawModule(module);
    runner.Check(dump == second, "two dumps of one module agree byte for byte");
}

void CheckCookedDump(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("cooked dump");

    const CookedModule module = BuildTinyModule();
    runner.Check(module.Variants.size() == 2u, "both variants reached the tables");

    const std::string dump = DumpCookedModule(module);

    runner.Check(Contains(dump, R"("stage": "cooked")"), "the dump names its stage");
    runner.Check(Contains(dump, R"("module": "TinyModule")"), "the dump names its module");
    runner.Check(Contains(dump, R"("sourceCount": 2)"), "two different texts stay two sources");
    runner.Check(Contains(dump, R"("resourceCount": 1)"),
                 "one shared resource collapses to one entry, and the dump shows the collapse");
    runner.Check(Contains(dump, R"("footprintListCount": 1)"),
                 "both variants ask for the same size, so one footprint list serves both");
    runner.Check(Contains(dump, R"("rasterStateCount": 1)"), "a compute module has exactly one raster entry");

    runner.Check(Contains(dump, R"("name": "Waves")"), "a binding reaches the layout table");
    runner.Check(Contains(dump, R"("expression": "IFFT_SIZE * 4")"),
                 "the size expression survives, because a diagnostic must quote what the author wrote");
    runner.Check(Contains(dump, R"("kind": "buffer")"), "a footprint names which kind it is");
    runner.Check(Contains(dump, R"("elementCount": 1024)"), "the evaluated size reaches the dump");
    // Read the name from `k_HashName`. A literal here fails on the next hash swap rather than
    // proving the swap arrived.
    runner.Check(Contains(dump, std::format(R"("hashName": "{}")", k_HashName)),
                 "the dump names the hash, because the name reaches the output");
    runner.Check(Contains(dump, R"("hashCollisions": 0)"), "the interner reports its collision count");

    runner.Check(!Contains(dump, "// variant zero"),
                 "no target text reaches the dump, because it already ships in three other artifacts");
    runner.Check(Contains(dump, R"("byteLength": 16)"), "a source appears as a length");
    runner.Check(Contains(dump, R"("contentHash")"), "a source appears as a hash");

    const std::string second = DumpCookedModule(module);
    runner.Check(dump == second, "two dumps of one module agree byte for byte");
}

/** Stage 6 has a boundary type as of D8, so `interned` is no longer a name that parses and writes
 * nothing. The dump earns its place by showing the one thing the freeze destroys. */
void CheckInternedDump(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("interned dump");

    const InternedModule module = BuildTinyInternedModule();
    const std::string dump = DumpInternedModule(module);

    runner.Check(Contains(dump, R"("stage": "interned")"), "the dump names its stage");
    runner.Check(Contains(dump, R"("module": "TinyModule")"), "the dump names its module");
    runner.Check(Contains(dump, std::format(R"("hashName": "{}")", k_HashName)),
                 "the dump names the hash, because the name reaches the output");

    // The reason this dump exists. `FreezeModuleTables` copies the unique entries out and leaves the
    // provenance behind, so after the freeze nothing can say which variants collapsed onto which
    // entry. Only this stage can answer it.
    runner.Check(Contains(dump, R"("provenance")"), "the dump carries what the freeze discards");
    runner.Check(Contains(dump, R"("mappedFrom")"), "each unique entry lists the artifacts that reached it");
    runner.Check(Contains(dump, R"("variantIndex": 1)"),
                 "provenance names the variant, so a surprising collapse is traceable");

    runner.Check(!Contains(dump, "// variant zero"),
                 "no target text reaches the dump, for the same reason the cooked dump holds none");

    const std::string second = DumpInternedModule(module);
    runner.Check(dump == second, "two dumps of one module agree byte for byte");
}

/** A dump that cannot tell two modules apart is worth nothing as a regression harness, so prove it
 * moves when the model moves. */
void CheckCookedDumpDetectsChange(lodestone::tests::TestRunner& runner)
{
    runner.BeginSection("cooked dump sensitivity");

    const CookedModule original = BuildTinyModule();

    CookedModule changed = BuildTinyModule();
    runner.Check(!changed.Sources.empty(), "the module has a source to change");
    if (changed.Sources.empty())
    {
        return;
    }

    changed.Sources[0].push_back('X');
    runner.Check(DumpCookedModule(original) != DumpCookedModule(changed),
                 "one changed byte of source moves the dump");

    CookedModule reordered = BuildTinyModule();
    runner.Check(reordered.Variants.size() == 2u, "the module has two variants to reorder");
    if (reordered.Variants.size() != 2u)
    {
        return;
    }

    std::swap(reordered.Variants[0], reordered.Variants[1]);
    runner.Check(DumpCookedModule(original) != DumpCookedModule(reordered),
                 "variant order is part of the dump, so an unordered container cannot hide in it");
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "StageDump" };
    lodestone::StderrDiagnosticSink sink;

    CheckStageNames(runner);
    CheckStageBits(runner);
    CheckCommandLine(runner);
    CheckSpaceDump(runner);
    CheckDependentAxisDump(runner);
    CheckVariantDump(runner, sink);
    CheckInternedDump(runner);
    CheckRawPrimitives(runner);
    CheckRawDump(runner);
    CheckCookedDump(runner);
    CheckCookedDumpDetectsChange(runner);

    return runner.Report();
}
