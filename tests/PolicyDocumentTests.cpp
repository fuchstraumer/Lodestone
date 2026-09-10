#include "permute/PolicyDocument.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"
#include <string_view>

using lodestone::AxisKind;
using lodestone::AxisValueDomain;
using lodestone::CookError;
using lodestone::EarliestBindingTime;
using lodestone::PermutationAxis;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::PolicyDocument;
using lodestone::StderrDiagnosticSink;
using lodestone::tests::TestRunner;

namespace
{

// A complete file: one module, expected influence, and two target sections. The spirv section lists a
// boolean axis, so it proves a boolean reads back as 0 and 1.
constexpr std::string_view k_ValidPolicy = R"toml(
[[OceanFft.ExpectedInfluence]]
EntryPoint = "IfftPermuteCS"
Axis = "IFFT_USE_WAVE_OPS"
Inert = true

[OceanFft.targets.wgsl]
MaxVariants = 64
CookWhen = "IFFT_USE_WAVE_OPS == 1"

[OceanFft.targets.wgsl.CookValues]
IFFT_SIZE = [256, 512]

[OceanFft.targets.spirv.CookValues]
IFFT_USE_WAVE_OPS = [false, true]
)toml";

void TestParseAndQuery(TestRunner& runner)
{
    runner.BeginSection("parse and query");

    const auto parsed = PolicyDocument::Parse(k_ValidPolicy);
    runner.Check(parsed.has_value(), "a well-formed file parses");
    if (!parsed)
    {
        return;
    }
    const PolicyDocument& document = *parsed;

    runner.Check(document.ModuleCount() == 1u, "the file holds one module");
    runner.Check(document.FindModule("OceanFft") != nullptr, "the named module is found");
    runner.Check(document.FindModule("Nope") == nullptr, "an absent module is not found");

    const auto& wgsl = document.FindTargetPolicy("OceanFft", "wgsl");
    runner.Check(wgsl.MaxVariants == 64u, "MaxVariants reads back");
    runner.Check(wgsl.CookWhen == "IFFT_USE_WAVE_OPS == 1", "CookWhen reads back");
    runner.Check(wgsl.CookValues.size() == 1u, "the target holds one CookValues axis");
    runner.Check(wgsl.CookValues.size() == 1u && wgsl.CookValues[0].Axis == "IFFT_SIZE",
                 "the CookValues axis is named");
    runner.Check(wgsl.CookValues.size() == 1u && wgsl.CookValues[0].Values.size() == 2u
                     && wgsl.CookValues[0].Values[0] == 256 && wgsl.CookValues[0].Values[1] == 512,
                 "the CookValues integers read back in order");

    const auto& spirv = document.FindTargetPolicy("OceanFft", "spirv");
    runner.Check(spirv.CookValues.size() == 1u && spirv.CookValues[0].Values.size() == 2u
                     && spirv.CookValues[0].Values[0] == 0 && spirv.CookValues[0].Values[1] == 1,
                 "a boolean CookValues reads back as 0 and 1");

    const auto& absentTarget = document.FindTargetPolicy("OceanFft", "dxil");
    runner.Check(absentTarget.MaxVariants == 0u && absentTarget.CookValues.empty() && absentTarget.CookWhen.empty(),
                 "an absent target returns the empty policy");
    const auto& absentModule = document.FindTargetPolicy("Nope", "wgsl");
    runner.Check(absentModule.MaxVariants == 0u, "an absent module returns the empty policy");

    const auto influence = document.ExpectedInfluenceFor("OceanFft");
    runner.Check(influence.size() == 1u, "one influence statement reads back");
    runner.Check(influence.size() == 1u && influence[0].Axis == "IFFT_USE_WAVE_OPS" && influence[0].IsInert,
                 "the influence statement reads back its axis and flag");
    runner.Check(document.ExpectedInfluenceFor("Nope").empty(), "an absent module has no influence");
}

void TestParseRejections(TestRunner& runner)
{
    runner.BeginSection("parse rejects");

    const auto malformed = PolicyDocument::Parse("[OceanFft");
    runner.Check(!malformed.has_value(), "a syntax error is rejected");
    runner.Check(!malformed.has_value() && malformed.error().Line > 0u, "the syntax error carries a line");

    const auto wrongType = PolicyDocument::Parse("[OceanFft.targets.wgsl]\nMaxVariants = \"lots\"\n");
    runner.Check(!wrongType.has_value(), "a wrong-typed MaxVariants is rejected");
    runner.Check(!wrongType.has_value() && wrongType.error().Line > 0u, "the type error carries a line");

    const auto notATable = PolicyDocument::Parse("OceanFft = 5\n");
    runner.Check(!notATable.has_value(), "a non-table top-level entry is rejected");
}

void TestValidationAgainstSpace(TestRunner& runner)
{
    runner.BeginSection("validation against the space");

    const PermutationAxis sizeAxis{ "IFFT_SIZE",
                                    { PermutationValue{ 128u }, PermutationValue{ 256u },
                                      PermutationValue{ 512u }, PermutationValue{ 1024u } },
                                    AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Integral };
    const PermutationAxis waveOpsAxis{ "IFFT_USE_WAVE_OPS", { PermutationValue{ false }, PermutationValue{ true } },
                                       AxisKind::Capability, EarliestBindingTime::Cook, AxisValueDomain::Boolean };
    const PermutationAxis waveSizeAxis{ "IFFT_WAVE_SIZE",
                                        { PermutationValue{ 16u }, PermutationValue{ 32u },
                                          PermutationValue{ 64u }, PermutationValue{ 128u } },
                                        AxisKind::Tuning, EarliestBindingTime::Cook, AxisValueDomain::Integral };
    const PermutationSpace space{ "OceanFft", { sizeAxis, waveOpsAxis, waveSizeAxis } };

    auto validate = [&space](std::string_view toml) -> CookError
    {
        StderrDiagnosticSink sink;
        const auto document = PolicyDocument::Parse(toml);
        if (!document)
        {
            return CookError::Invalid; // these inputs parse; a failure here fails the check below
        }
        return document->ValidateAgainstSpace("OceanFft", space, sink);
    };

    runner.Check(validate(k_ValidPolicy) == CookError::Success, "a policy that names real axes and values validates");

    const CookError unknownCookAxis = validate("[OceanFft.targets.wgsl.CookValues]\nIFFT_SZ = [256]\n");
    runner.Check(unknownCookAxis == CookError::PolicyAxisNotDeclared, "CookValues on an undeclared axis fails");

    const CookError valueNotInAxis = validate("[OceanFft.targets.wgsl.CookValues]\nIFFT_SIZE = [999]\n");
    runner.Check(valueNotInAxis == CookError::PolicyValueNotInAxis, "a CookValues value the axis lacks fails");

    const CookError cookWhenUnknownAxis =
        validate("[OceanFft.targets.wgsl]\nCookWhen = \"NOPE == 1\"\n");
    runner.Check(cookWhenUnknownAxis == CookError::PolicyAxisNotDeclared, "CookWhen naming an undeclared axis fails");

    const CookError cookWhenMalformed = validate("[OceanFft.targets.wgsl]\nCookWhen = \"== 1\"\n");
    runner.Check(cookWhenMalformed == CookError::PolicyCookWhenInvalid, "a malformed CookWhen fails");

    const CookError influenceUnknownAxis =
        validate("[[OceanFft.ExpectedInfluence]]\nEntryPoint = \"cs\"\nAxis = \"GHOST\"\n");
    runner.Check(influenceUnknownAxis == CookError::PolicyAxisNotDeclared,
                 "ExpectedInfluence on an undeclared axis fails");

    const CookError absentModuleValidates = [&space]
    {
        StderrDiagnosticSink sink;
        const auto document = PolicyDocument::Parse(k_ValidPolicy);
        return document ? document->ValidateAgainstSpace("SomeOtherModule", space, sink) : CookError::Invalid;
    }();
    runner.Check(absentModuleValidates == CookError::Success, "a module the file does not name is not an error");
}

} // namespace

int main()
{
    TestRunner runner{ "PolicyDocumentTests" };
    TestParseAndQuery(runner);
    TestParseRejections(runner);
    TestValidationAgainstSpace(runner);
    return runner.Report();
}
