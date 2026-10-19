#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "TestHarness.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ResolveStage.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>


// Stage 4 is a pure function of a `RawVariant` and a symbol table. This test is the proof of that
// claim: it links against no Slang type, it reads no asset, and it runs in milliseconds. This test could
// not be written before stage 3 and stage 4 were separated, because the arithmetic used to live inside
// the file that talks to the compiler.
//
// Every buffer size the engine allocates comes out of this arithmetic, so a wrong answer here is a
// wrong allocation with no other symptom. That is why the rejections get as much surface as the
// successes.

using lodestone::BindingKind;
using lodestone::BoundPlacement;
using lodestone::BufferFootprint;
using lodestone::CompiledVariant;
using lodestone::CookError;
using lodestone::CookResult;
using lodestone::DiagnosticSink;
using lodestone::ExternConstantDefault;
using lodestone::MakeResolveContext;
using lodestone::PermutationAssignment;
using lodestone::PermutationAxis;
using lodestone::PermutationBinding;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::RawDerivedConstant;
using lodestone::DerivedConstantTable;
using lodestone::MakeDerivedConstantTable;
using lodestone::RawBinding;
using lodestone::RawEntryPoint;
using lodestone::RawSizeAttribute;
using lodestone::RawSizeAttributeKind;
using lodestone::RawVariant;
using lodestone::ResolveContext;
using lodestone::ResolveVariant;
using lodestone::ResourcePlacement;
using lodestone::ShaderStageKind;
using lodestone::StderrDiagnosticSink;
using lodestone::TextureFootprint;
using lodestone::tests::TestRunner;

namespace
{

// The axes and the defaults are file scope because `ResolveContext` stores `std::string_view` names.
// The strings must outlive every context this file builds.
const PermutationSpace k_Space{ { PermutationAxis{ "IFFT_SIZE",
                                                   { PermutationValue{ 256u }, PermutationValue{ 512u } },
                                                   lodestone::AxisKind::Tuning,
                                                   lodestone::EarliestBindingTime::Cook,
                                                   lodestone::AxisValueDomain::Integral },
                                  PermutationAxis{ "IFFT_USE_WAVE_OPS",
                                                   { PermutationValue{ false }, PermutationValue{ true } },
                                                   lodestone::AxisKind::Capability,
                                                   lodestone::EarliestBindingTime::Cook,
                                                   lodestone::AxisValueDomain::Boolean } } };
const PermutationAxis& k_SizeAxis = k_Space.Axes()[0];
const PermutationAxis& k_WaveOpsAxis = k_Space.Axes()[1];

const std::array<ExternConstantDefault, 1> k_ExternDefaults{ ExternConstantDefault{
    .Name = "IFFT_NUM_WAVE_CASCADES", .Value = 4 } };

PermutationAssignment MakeAssignment()
{
    return PermutationAssignment{
        PermutationBinding{ .Axis = &k_SizeAxis, .Value = PermutationValue{ 512u } },
        PermutationBinding{ .Axis = &k_WaveOpsAxis, .Value = PermutationValue{ true } }
    };
}

const lodestone::DerivedConstantTable k_NoDerivedConstants{};

/** Returns the symbol table of one variant with no derived constants. */
ResolveContext MakeContext()
{
    StderrDiagnosticSink sink;
    return MakeResolveContext(MakeAssignment(), k_ExternDefaults, k_NoDerivedConstants, RawVariant{}, sink).value();
}

/** One storage buffer, placed at group 0 binding 0, with no annotation. Each test adds what it needs. */
RawVariant MakeVariantWithOneBuffer()
{
    RawBinding binding;
    binding.Name = "SpectrumBuffer";
    binding.Placement = BoundPlacement{ .Group = 0u, .Binding = 0u };
    binding.Kind = BindingKind::StorageBuffer;
    binding.ElementStride = 8u;

    RawVariant variant;
    variant.VariantSuffix = "_S512_W1";
    variant.VariantIndex = 3u;
    variant.Bindings.push_back(std::move(binding));
    return variant;
}

RawSizeAttribute MakeAttribute(RawSizeAttributeKind kind, std::vector<std::string> arguments)
{
    return RawSizeAttribute{ .BindingIndex = 0u, .Kind = kind, .Arguments = std::move(arguments) };
}

/** Resolves a variant with one annotation, and returns the derived size of binding 0. */
CookResult<lodestone::ResourceFootprint> ResolveOneAttribute(RawSizeAttribute attribute, DiagnosticSink& sink)
{
    RawVariant variant = MakeVariantWithOneBuffer();
    variant.SizeAttributes.push_back(std::move(attribute));

    CookResult<CompiledVariant> resolved = ResolveVariant(variant, MakeContext(), sink);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    return resolved.value().Footprints.front();
}

void CheckElementCount(TestRunner& runner,
                       std::string_view expression,
                       uint64_t expected,
                       std::string_view description,
                       DiagnosticSink& sink)
{
    const auto derived = ResolveOneAttribute(
        MakeAttribute(RawSizeAttributeKind::ElementCount, { std::string{ expression } }), sink);
    const BufferFootprint* buffer =
        derived.has_value() ? std::get_if<BufferFootprint>(&derived.value()) : nullptr;
    runner.Check(buffer != nullptr && buffer->ElementCount == expected, description);
}

void CheckRejection(TestRunner& runner,
                    RawSizeAttribute attribute,
                    CookError expected,
                    std::string_view description,
                    DiagnosticSink& sink)
{
    const auto derived = ResolveOneAttribute(std::move(attribute), sink);
    runner.Check(!derived.has_value() && derived.error() == expected, description);
}

void TestSymbolTable(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("symbol table");

    const ResolveContext context = MakeContext();
    runner.Check(context.Symbols.size() == 3u, "one symbol for each extern default and each axis");

    // The axis value wins over nothing here, but the order is the contract: the defaults come first,
    // so an axis of the same name would shadow one.
    runner.Check(context.Symbols.front().Name == "IFFT_NUM_WAVE_CASCADES" &&
                     context.Symbols.front().Value == 4,
                 "the undriven extern default comes first, with its declared value");

    CheckElementCount(runner, "IFFT_SIZE", 512u, "an axis value reaches the evaluator", sink);
    CheckElementCount(runner, "IFFT_NUM_WAVE_CASCADES", 4u, "an extern default reaches the evaluator", sink);
    CheckElementCount(runner, "IFFT_USE_WAVE_OPS", 1u, "a bool axis widens to one", sink);
}

void TestElementCount(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("element count");

    CheckElementCount(runner, "1024", 1024u, "a literal needs no symbol", sink);
    CheckElementCount(runner, "IFFT_SIZE * IFFT_SIZE", 262144u, "the square case the ocean demo needs", sink);
    CheckElementCount(runner,
                      "IFFT_SIZE * IFFT_SIZE * IFFT_NUM_WAVE_CASCADES",
                      1048576u,
                      "an axis and an extern default in one expression",
                      sink);

    const auto derived =
        ResolveOneAttribute(MakeAttribute(RawSizeAttributeKind::ElementCount, { "IFFT_SIZE * 4" }), sink);
    const BufferFootprint* buffer =
        derived.has_value() ? std::get_if<BufferFootprint>(&derived.value()) : nullptr;
    runner.Check(buffer != nullptr && buffer->Expression == "IFFT_SIZE * 4",
                 "the source expression is kept beside the number it evaluated to");
    runner.Check(derived.has_value() && !std::holds_alternative<TextureFootprint>(derived.value()),
                 "an element count is a buffer footprint, never a texture one");
}

void TestExtent(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("extent");

    const auto extent1d = ResolveOneAttribute(MakeAttribute(RawSizeAttributeKind::Extent1d, { "IFFT_SIZE" }), sink);
    const TextureFootprint* line =
        extent1d.has_value() ? std::get_if<TextureFootprint>(&extent1d.value()) : nullptr;
    runner.Check(line != nullptr && line->ExtentX == 512u && line->ExtentY == 1u && line->ExtentZ == 1u,
                 "a 1d extent fills the first axis and leaves the other two at one");

    const auto extent2d = ResolveOneAttribute(
        MakeAttribute(RawSizeAttributeKind::Extent2d, { "IFFT_SIZE", "IFFT_SIZE / 2" }), sink);
    const TextureFootprint* first =
        extent2d.has_value() ? std::get_if<TextureFootprint>(&extent2d.value()) : nullptr;
    runner.Check(first != nullptr && first->ExtentX == 512u && first->ExtentY == 256u,
                 "a 2d extent evaluates each argument on its own");
    runner.Check(first != nullptr && first->ExtentZ == 1u, "a 2d extent leaves the third axis at one");
    runner.Check(extent2d.has_value() && !std::holds_alternative<BufferFootprint>(extent2d.value()),
                 "an extent is a texture footprint, never a buffer one");

    const auto extent3d = ResolveOneAttribute(
        MakeAttribute(RawSizeAttributeKind::Extent3d, { "IFFT_SIZE", "IFFT_SIZE", "IFFT_NUM_WAVE_CASCADES" }),
        sink);
    const TextureFootprint* second =
        extent3d.has_value() ? std::get_if<TextureFootprint>(&extent3d.value()) : nullptr;
    runner.Check(second != nullptr && second->ExtentX == 512u && second->ExtentY == 512u &&
                     second->ExtentZ == 4u,
                 "a 3d extent fills all three axes");
}

void TestNoAnnotation(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("no annotation");

    // Most resources are sized by the caller. An unannotated binding must stay unannotated, because a
    // size that defaults to zero and a size nobody declared are different facts.
    const CookResult<CompiledVariant> resolved =
        ResolveVariant(MakeVariantWithOneBuffer(), MakeContext(), sink);
    runner.Check(resolved.has_value(), "a binding with no annotation is not an error");

    if (!resolved)
    {
        return;
    }

    // A sum makes the third state a value rather than two flags that could disagree.
    runner.Check(std::holds_alternative<std::monostate>(resolved.value().Footprints.front()),
                 "no annotation gives no footprint, which is a state of its own");
}

void TestRejections(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("rejections");

    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::ElementCount, { "IFFT_SIZ" }),
                   CookError::AttributeExpressionUnknownSymbol,
                   "a typo in a symbol name",
                   sink);
    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::ElementCount, { "IFFT_WAVE_SIZE * 4" }),
                   CookError::AttributeExpressionUnknownSymbol,
                   "a constant that no axis drives and no default declares",
                   sink);
    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::ElementCount, {}),
                   CookError::AttributeExpressionParseFailed,
                   "an element count with no argument",
                   sink);
    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::ElementCount, { "0" }),
                   CookError::AttributeExpressionOutOfRange,
                   "an element count of zero cannot size a buffer",
                   sink);
    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::Extent1d, {}),
                   CookError::AttributeExpressionParseFailed,
                   "a 1d extent with no argument",
                   sink);
    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::Extent2d, { "IFFT_SIZE" }),
                   CookError::AttributeExpressionParseFailed,
                   "a 2d extent with one argument",
                   sink);
    CheckRejection(runner,
                   MakeAttribute(RawSizeAttributeKind::Extent2d, { "IFFT_SIZE", "0" }),
                   CookError::AttributeExpressionOutOfRange,
                   "an extent axis of zero is not a texture dimension",
                   sink);

    RawVariant bothExtents = MakeVariantWithOneBuffer();
    bothExtents.SizeAttributes.push_back(
        MakeAttribute(RawSizeAttributeKind::Extent2d, { "IFFT_SIZE", "IFFT_SIZE" }));
    bothExtents.SizeAttributes.push_back(
        MakeAttribute(RawSizeAttributeKind::Extent3d, { "IFFT_SIZE", "IFFT_SIZE", "2" }));
    const CookResult<CompiledVariant> conflict = ResolveVariant(bothExtents, MakeContext(), sink);
    runner.Check(!conflict.has_value() && conflict.error() == CookError::ReflectionSizeUnresolved,
                 "one resource cannot carry both a 2d and a 3d extent");
}

void TestAttributeTargeting(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("attribute targeting");

    // `BindingIndex` is the only thing that joins an annotation to a resource. If the join ever went
    // by position or by name, this check would fail.
    RawVariant variant = MakeVariantWithOneBuffer();
    RawBinding second;
    second.Name = "DisplacementBuffer";
    second.Placement = BoundPlacement{ .Group = 0u, .Binding = 1u };
    second.Kind = BindingKind::StorageBuffer;
    variant.Bindings.push_back(std::move(second));

    RawSizeAttribute attribute = MakeAttribute(RawSizeAttributeKind::ElementCount, { "IFFT_SIZE" });
    attribute.BindingIndex = 1u;
    variant.SizeAttributes.push_back(std::move(attribute));

    const CookResult<CompiledVariant> resolved = ResolveVariant(variant, MakeContext(), sink);
    runner.Check(resolved.has_value(), "a two binding variant resolves");

    if (!resolved)
    {
        return;
    }

    runner.Check(std::holds_alternative<std::monostate>(resolved.value().Footprints[0]),
                 "the annotation does not reach the binding it does not name");
    const BufferFootprint* named = std::get_if<BufferFootprint>(&resolved.value().Footprints[1]);
    runner.Check(named != nullptr && named->ElementCount == 512u,
                 "the annotation reaches the binding it names");
}

void TestPlacementAndPassthrough(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("placement and passthrough");

    RawVariant variant = MakeVariantWithOneBuffer();
    variant.Bindings.front().Placement = BoundPlacement{ .Group = 2u, .Binding = 3u };

    RawBinding unplaced;
    unplaced.Name = "PushConstants";
    unplaced.Kind = BindingKind::UniformBuffer;
    unplaced.ByteSize = 64u;
    variant.Bindings.push_back(std::move(unplaced));

    const CookResult<CompiledVariant> resolved = ResolveVariant(variant, MakeContext(), sink);
    runner.Check(resolved.has_value(), "a variant with an unplaced resource resolves");

    if (!resolved)
    {
        return;
    }

    const CompiledVariant& value = resolved.value();
    runner.Check(value.Bindings[0].Placement == ResourcePlacement{ BoundPlacement{ 2u, 3u } },
                 "a bound placement reaches the resource unflattened");
    // A resource with no placement is not a resource at group 0 binding 0, and the sum says so.
    runner.Check(std::holds_alternative<std::monostate>(value.Bindings[1].Placement),
                 "an unplaced resource holds no placement at all");
    runner.Check(value.Bindings[1].ByteSize == 64u, "the declared byte size passes through");
    runner.Check(value.VariantSuffix == "_S512_W1" && value.VariantIndex == 3u,
                 "the variant identity passes through");
}

void TestEntryPoints(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("entry points");

    RawEntryPoint raw;
    raw.Name = "SpectrumUpdate";
    raw.VariantSuffix = "_S512_W1";
    raw.Stage = ShaderStageKind::Compute;
    raw.Workgroup = { .X = 8u, .Y = 8u, .Z = 1u };
    raw.TargetCode = lodestone::tests::BytesOf("@compute fn SpectrumUpdate() {}");
    raw.UsedBindingIndices = { 0u };

    RawVariant variant = MakeVariantWithOneBuffer();
    variant.EntryPoints.push_back(std::move(raw));

    const CookResult<CompiledVariant> resolved = ResolveVariant(variant, MakeContext(), sink);
    runner.Check(resolved.has_value() && resolved.value().EntryPoints.size() == 1u,
                 "one raw entry point gives one compiled entry point");

    if (!resolved || resolved.value().EntryPoints.empty())
    {
        return;
    }

    const auto& entryPoint = resolved.value().EntryPoints.front();
    runner.Check(entryPoint.Code == lodestone::tests::BytesOf("@compute fn SpectrumUpdate() {}"),
                 "the target text becomes the code, unread and unchanged");
    runner.Check(entryPoint.Name == "SpectrumUpdate" && entryPoint.Reflection.Name == "SpectrumUpdate",
                 "the entry point name reaches both the record and its reflection");
    runner.Check(entryPoint.Reflection.Stage == ShaderStageKind::Compute, "the stage passes through");
    runner.Check(entryPoint.Reflection.Workgroup.X == 8u && entryPoint.Reflection.Workgroup.Y == 8u &&
                     entryPoint.Reflection.Workgroup.Z == 1u,
                 "the workgroup size passes through");
    runner.Check(entryPoint.Reflection.UsedBindingIndices.size() == 1u &&
                     entryPoint.Reflection.UsedBindingIndices.front() == 0u,
                 "visibility passes through as indices into the shared binding list");
}

RawDerivedConstant MakeDerived(std::string name, std::string expression, uint32_t line)
{
    return RawDerivedConstant{ .Name = std::move(name),
                               .Expression = std::move(expression),
                               .SourceFile = "Derived.slang",
                               .SourceLine = line };
}

/** Builds the derived constant table, then resolves one element count attribute with `expression` against it.
 * Returns the element count, or the first error of the three steps. */
CookResult<uint64_t> ResolveDerivedCount(std::span<const RawDerivedConstant> constants,
                                         std::string expression,
                                         DiagnosticSink& sink)
{
    const CookResult<DerivedConstantTable> table =
        MakeDerivedConstantTable(constants, k_Space.Axes(), k_ExternDefaults, sink);
    if (!table)
    {
        return std::unexpected(table.error());
    }

    RawVariant variant = MakeVariantWithOneBuffer();
    variant.SizeAttributes.push_back(MakeAttribute(RawSizeAttributeKind::ElementCount, { std::move(expression) }));
    const CookResult<ResolveContext> context =
        MakeResolveContext(MakeAssignment(), k_ExternDefaults, table.value(), variant, sink);
    if (!context)
    {
        return std::unexpected(context.error());
    }

    const CookResult<CompiledVariant> resolved = ResolveVariant(variant, context.value(), sink);
    if (!resolved)
    {
        return std::unexpected(resolved.error());
    }

    const BufferFootprint* buffer = std::get_if<BufferFootprint>(&resolved.value().Footprints.front());
    return buffer != nullptr ? buffer->ElementCount : 0u;
}

void CheckDerivedCount(TestRunner& runner,
                       std::span<const RawDerivedConstant> constants,
                       std::string expression,
                       uint64_t expected,
                       std::string_view description,
                       DiagnosticSink& sink)
{
    const CookResult<uint64_t> count = ResolveDerivedCount(constants, std::move(expression), sink);
    runner.Check(count.has_value() && count.value() == expected, description);
}

void CheckDerivedRejection(TestRunner& runner,
                           std::span<const RawDerivedConstant> constants,
                           std::string expression,
                           CookError expected,
                           std::string_view description,
                           DiagnosticSink& sink)
{
    const CookResult<uint64_t> count = ResolveDerivedCount(constants, std::move(expression), sink);
    runner.Check(!count.has_value() && count.error() == expected, description);
}

/** Returns how many entries of the symbol table of `TOP` in the diamond below have the name `BASE`. */
size_t CountDiamondBaseSymbols(std::span<const RawDerivedConstant> diamond, DiagnosticSink& sink)
{
    const CookResult<DerivedConstantTable> table =
        MakeDerivedConstantTable(diamond, k_Space.Axes(), k_ExternDefaults, sink);
    if (!table)
    {
        return 0u;
    }

    RawVariant variant = MakeVariantWithOneBuffer();
    variant.SizeAttributes.push_back(MakeAttribute(RawSizeAttributeKind::ElementCount, { "TOP" }));
    const CookResult<ResolveContext> context =
        MakeResolveContext(MakeAssignment(), k_ExternDefaults, table.value(), variant, sink);
    if (!context)
    {
        return 0u;
    }

    return static_cast<size_t>(std::ranges::count(context.value().Symbols,
                                                  std::string_view{ "BASE" },
                                                  &lodestone::AttrExprSymbol::Name));
}

// in this file IFFT_SIZE is 512, IFFT_USE_WAVE_OPS is 1, and the extern IFFT_NUM_WAVE_CASCADES is 4
void TestDerivedConstants(TestRunner& runner, DiagnosticSink& sink)
{
    runner.BeginSection("derived constants");

    const std::array oneLevel{ MakeDerived("HALF", "IFFT_SIZE / 2u", 1u) };
    CheckDerivedCount(runner, oneLevel, "HALF", 256u, "a derived constant of an axis", sink);
    CheckDerivedCount(runner, oneLevel, "HALF * 2", 512u, "a derived constant inside a larger expression", sink);

    // SLOTS is declared before CLUSTER, so declaration order cannot be the evaluation order
    const std::array twoLevels{ MakeDerived("SLOTS", "CLUSTER * 16u", 1u),
                                MakeDerived("CLUSTER", "IFFT_SIZE * IFFT_NUM_WAVE_CASCADES", 2u) };
    CheckDerivedCount(runner,
                      twoLevels,
                      "SLOTS",
                      32768u,
                      "a derived constant of a derived constant, declared before it",
                      sink);

    const std::array diamond{ MakeDerived("TOP", "LEFT + RIGHT", 1u),
                              MakeDerived("LEFT", "BASE * 2u", 2u),
                              MakeDerived("RIGHT", "BASE + 1u", 3u),
                              MakeDerived("BASE", "IFFT_SIZE", 4u) };
    CheckDerivedCount(runner, diamond, "TOP", 1537u, "a diamond: two constants use one base", sink);
    runner.Check(CountDiamondBaseSymbols(diamond, sink) == 1u,
                 "the base of a diamond is evaluated once, though two paths reach it");

    const std::array sameTwice{ MakeDerived("N", "4u", 1u), MakeDerived("N", "4u", 9u) };
    CheckDerivedCount(runner, sameTwice, "N", 4u, "two declarations with one expression are one constant", sink);

    const std::array unusedFloat{ MakeDerived("PI", "3.14159", 1u), MakeDerived("HALF", "IFFT_SIZE / 2u", 2u) };
    CheckDerivedCount(runner,
                      unusedFloat,
                      "HALF",
                      256u,
                      "a constant that cannot be evaluated is harmless while no attribute reaches it",
                      sink);

    runner.BeginSection("derived constant rejections");

    CheckDerivedRejection(runner,
                          unusedFloat,
                          "PI",
                          CookError::DerivedConstantUnreadable,
                          "a reached constant that does not parse",
                          sink);

    const std::array unknownName{ MakeDerived("BAD", "NOT_DECLARED * 2u", 1u) };
    CheckDerivedRejection(runner,
                          unknownName,
                          "BAD",
                          CookError::DerivedConstantUnreadable,
                          "a reached constant that uses an unknown symbol",
                          sink);

    const std::array cycle{ MakeDerived("A", "B + 1u", 1u), MakeDerived("B", "A * 2u", 2u) };
    CheckDerivedRejection(runner, cycle, "A", CookError::DerivedConstantCycle, "two constants that use each other", sink);

    const std::array selfCycle{ MakeDerived("A", "A + 1u", 1u) };
    CheckDerivedRejection(runner, selfCycle, "A", CookError::DerivedConstantCycle, "a constant that uses itself", sink);

    const std::array ambiguous{ MakeDerived("N", "4u", 3u), MakeDerived("N", "8u", 9u) };
    CheckDerivedRejection(runner,
                          ambiguous,
                          "N",
                          CookError::DerivedConstantAmbiguous,
                          "two declarations of one name with different expressions",
                          sink);
    CheckDerivedRejection(runner,
                          ambiguous,
                          "IFFT_SIZE",
                          CookError::DerivedConstantAmbiguous,
                          "an ambiguous name fails the module even when no attribute uses it",
                          sink);

    const std::array shadowsAxis{ MakeDerived("IFFT_SIZE", "64u", 1u) };
    CheckDerivedRejection(runner,
                          shadowsAxis,
                          "IFFT_SIZE",
                          CookError::DerivedConstantShadowsSymbol,
                          "a derived constant with the name of an axis",
                          sink);

    const std::array shadowsExtern{ MakeDerived("IFFT_NUM_WAVE_CASCADES", "2u", 1u) };
    CheckDerivedRejection(runner,
                          shadowsExtern,
                          "1",
                          CookError::DerivedConstantShadowsSymbol,
                          "a derived constant with the name of an extern constant, even when no attribute uses it",
                          sink);
}

} // namespace

int main()
{
    TestRunner runner{ "ResolveStageTests" };

    lodestone::StderrDiagnosticSink sink;
    TestSymbolTable(runner, sink);
    TestElementCount(runner, sink);
    TestExtent(runner, sink);
    TestNoAnnotation(runner, sink);
    TestRejections(runner, sink);
    TestAttributeTargeting(runner, sink);
    TestPlacementAndPassthrough(runner, sink);
    TestEntryPoints(runner, sink);
    TestDerivedConstants(runner, sink);

    return runner.Report();
}
