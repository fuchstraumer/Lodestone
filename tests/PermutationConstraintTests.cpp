#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"
#include <algorithm>
#include <initializer_list>

using lodestone::AxisKind;
using lodestone::AxisValueDomain;
using lodestone::DiagnosticSeverity;
using lodestone::EarliestBindingTime;
using lodestone::CookError;
using lodestone::PermutationAxis;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::RecordingDiagnosticSink;
using lodestone::StderrDiagnosticSink;
using lodestone::tests::TestRunner;

namespace
{

void TestActiveWhenGating(TestRunner& runner)
{
    runner.BeginSection("ActiveWhen Gating");
    StderrDiagnosticSink sink;

    const PermutationAxis gateAxis{ "GATE", { PermutationValue{ false }, PermutationValue{ true } },
                                   AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean };
    const PermutationAxis widthAxis{ "WIDTH", { PermutationValue{ 16u }, PermutationValue{ 32u } },
                                    AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Integral,
                                    "GATE == 1" };
    const PermutationSpace space{ "Gated", { gateAxis, widthAxis } };

    const auto variants = space.EnumerateVariants(sink);
    runner.Check(variants.has_value(), "Enumeration succeeds.");
    // check variant count: should be 3 (GATE=false, GATE=true with WIDTH=16, GATE=true with WIDTH=32)
    runner.Check(variants && variants->Variants.size() == 3, "Correct number of variants.");
}

void TestRequirePruning(TestRunner& runner)
{
    runner.BeginSection("Require pruning");
    StderrDiagnosticSink sink;

    const std::initializer_list<PermutationValue> tiles{ PermutationValue{ 8u }, PermutationValue{ 16u }, PermutationValue{ 32u } };
    const std::initializer_list<PermutationValue> regs { PermutationValue{ 32u }, PermutationValue{ 64u }, PermutationValue{ 128u } };
    const PermutationAxis tileAxis{ "TILE", tiles, AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Integral };
    const PermutationAxis regAxis{ "REG",  regs,  AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Integral };

    auto makeAxes = [&]{ return std::initializer_list<PermutationAxis>{ tileAxis, regAxis }; };

    const PermutationSpace control{ "TileControl", makeAxes() };                 // no Require
    const PermutationSpace gated  { "TileGated",   makeAxes(),
                                    { "TILE * TILE * REG <= 65536" } };          // forbids 32x128 = 131072

    const auto full   = control.EnumerateVariants(sink);
    const auto pruned = gated.EnumerateVariants(sink);
    runner.Check(full && full->Variants.size() == 9u, "Control cooks the full variant grid");
    runner.Check(pruned && pruned->Variants.size() == 8u, "Require drops exactly one cell");
}

void TestValidationRejections(TestRunner& runner)
{
    runner.BeginSection("constraint validation");

    // Forward reference: EARLY (index 0) names LATE (index 1), declared after it.
    {
        StderrDiagnosticSink sink;
        const PermutationAxis earlyAxis{ "EARLY", { PermutationValue{ false }, PermutationValue{ true } },
                                         AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean, "LATE == 1" };
        const PermutationAxis lateAxis{ "LATE",  { PermutationValue{ false }, PermutationValue{ true } },
                                        AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean };
        const PermutationSpace space{ "Fwd", { earlyAxis, lateAxis } };
        runner.Check(space.ValidateConstraints(sink) == CookError::PermutationConstraintForwardReference,
                     "a forward reference fails at load");
    }
    // Unknown symbol: ActiveWhen names an identifier no axis declares.
    // //   ... "NOPE == 1" -> CookError::PermutationConstraintUnknownSymbol
    {
        StderrDiagnosticSink sink;
        const PermutationAxis axis{ "AXIS", { PermutationValue{ false }, PermutationValue{ true } },
                                    AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean, "NOPE == 1" };
        const PermutationSpace space{ "UnknownSymbol", { axis } };
        runner.Check(space.ValidateConstraints(sink) == CookError::PermutationConstraintUnknownSymbol,
                     "an unknown symbol fails validation");
    }
    // Malformed expression: a syntactically broken ActiveWhen.
    //   ... "== 1" (or "A &&") -> CookError::PermutationConstraintInvalidExpression
    {
        StderrDiagnosticSink sink;
        const PermutationAxis axis{ "AXIS", { PermutationValue{ false }, PermutationValue{ true } },
                                    AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean, "== 1" };
        const PermutationSpace space{ "MalformedExpression", { axis } };
        runner.Check(space.ValidateConstraints(sink) == CookError::PermutationConstraintInvalidExpression,
                     "a malformed expression fails validation");
    }
}

// Verify that later axes depending on conditional axes trigger a default substitution warning
void TestDefaultSubstitutionWarning(TestRunner& runner)
{
    runner.BeginSection("default-substitution warning");
    // verifying this requires recording the warnings
    RecordingDiagnosticSink sink;

    // LEAF depends on MID, and MID is itself conditional (depends on GATE).
    const PermutationAxis gateAxis{ "GATE", { PermutationValue{ false }, PermutationValue{ true } },
                                    AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean };
    const PermutationAxis midAxis{ "MID",  { PermutationValue{ false }, PermutationValue{ true } },
                                   AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean, "GATE == 1" };
    const PermutationAxis leafAxis{ "LEAF", { PermutationValue{ 16u }, PermutationValue{ 32u } },
                                   AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Integral, "MID == 1" };
    const PermutationSpace space{ "Chain", { gateAxis, midAxis, leafAxis } };

    runner.Check(space.ValidateConstraints(sink) == CookError::Success, "a conditional chain still validates");

    const bool warned = std::ranges::any_of(sink.Records(),
        [](const auto& d){ return d.Severity == DiagnosticSeverity::Warning; });
    runner.Check(warned, "referencing a conditional axis warns about default substitution");
}

}

int main()
{
    TestRunner runner{ "PermutationConstraintTests" };
    TestActiveWhenGating(runner);
    TestRequirePruning(runner);
    TestValidationRejections(runner);
    TestDefaultSubstitutionWarning(runner);
    return runner.Report();
}
