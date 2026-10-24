#include "permute/PolicyDocument.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>
#include <string_view>

using lodestone::AxisKind;
using lodestone::AxisValueDomain;
using lodestone::CookError;
using lodestone::EarliestBindingTime;
using lodestone::PermutationAxis;
using lodestone::PermutationSpace;
using lodestone::PermutationValue;
using lodestone::PolicyDocument;
using lodestone::RecordingDiagnosticSink;
using lodestone::StderrDiagnosticSink;
using lodestone::TargetPolicyCoverage;
using lodestone::tests::TestRunner;

namespace
{

// A complete file: one module, expected influence, and two target sections. The spirv section lists a
// boolean axis, so it proves a boolean reads back as 0 and 1.
constexpr std::string_view k_ValidPolicy = R"toml(
[OceanFft.InertAxesForEntryPoints]
IfftPermuteCS = ["IFFT_USE_WAVE_OPS", "IFFT_WAVE_SIZE"]

[OceanFft.targets.wgsl]
MaxVariants = 64
CookIf = "IFFT_USE_WAVE_OPS == 1"

[OceanFft.targets.wgsl.CookValues]
IFFT_SIZE = [256, 512]

[OceanFft.targets.spirv.CookValues]
IFFT_USE_WAVE_OPS = [false, true]
)toml";

void TestParseAndQuery(TestRunner& runner)
{
    runner.BeginSection("parse and query");

    const auto parsed = PolicyDocument::Parse(k_ValidPolicy);
    if (!runner.CheckOk(parsed, "a well-formed file parses"))
    {
        return;
    }
    const PolicyDocument& document = *parsed;

    runner.CheckEqual(document.ModuleCount(), 1u, "the file holds one module");
    runner.Check(document.FindModule("OceanFft") != nullptr, "the named module is found");
    runner.CheckEqual(document.FindModule("Nope"), nullptr, "an absent module is not found");

    const auto& wgsl = document.FindTargetPolicy("OceanFft", "wgsl");
    runner.CheckEqual(wgsl.MaxVariants, 64u, "MaxVariants reads back");
    runner.CheckEqual(wgsl.CookIf, "IFFT_USE_WAVE_OPS == 1", "CookIf reads back");
    runner.CheckEqual(wgsl.CookValues.size(), 1u, "the target holds one CookValues axis");
    if (runner.CheckEqual(wgsl.CookValues.size(), 1u, "the CookValues axis is named"))
    {
        runner.CheckEqual(wgsl.CookValues[0].Axis, "IFFT_SIZE", "the CookValues axis is named");
    }
    if (runner.CheckEqual(wgsl.CookValues.size(), 1u, "the CookValues integers read back in order"))
    {
        if (runner.CheckEqual(wgsl.CookValues[0].Values.size(), 2u, "the CookValues integers read back in order"))
        {
            runner.CheckEqual(wgsl.CookValues[0].Values[0].AsUInt(), 256u, "the CookValues integers read back in order");
            runner.CheckEqual(wgsl.CookValues[0].Values[1].AsUInt(), 512u, "the CookValues integers read back in order");
        }
    }

    const auto& spirv = document.FindTargetPolicy("OceanFft", "spirv");
    runner.Check(spirv.CookValues.size() == 1u && spirv.CookValues[0].Values.size() == 2u
                     && !spirv.CookValues[0].Values[0].AsBool() && spirv.CookValues[0].Values[1].AsBool(),
                 "a boolean CookValues reads back as 0 and 1");

    const auto& absentTarget = document.FindTargetPolicy("OceanFft", "dxil");
    runner.Check(absentTarget.MaxVariants == 0u && absentTarget.CookValues.empty() && absentTarget.CookIf.empty(),
                 "an absent target returns the empty policy");
    const auto& absentModule = document.FindTargetPolicy("Nope", "wgsl");
    runner.CheckEqual(absentModule.MaxVariants, 0u, "an absent module returns the empty policy");

    const auto inertAxes = document.InertAxesForEntryPoint("OceanFft", "IfftPermuteCS");
    if (runner.CheckEqual(inertAxes.size(), 2u, "InertAxes are preserved, and contain both axes disabled"))
    {
        runner.CheckEqual(inertAxes[0], "IFFT_USE_WAVE_OPS", "InertAxes are preserved, and contain both axes disabled");
        runner.CheckEqual(inertAxes[1], "IFFT_WAVE_SIZE", "InertAxes are preserved, and contain both axes disabled");
    }
    runner.Check(document.InertAxesForEntryPoint("Nope", "IfftPermuteCS").empty(), "an absent module has no influence");
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
    const PermutationSpace space{ { sizeAxis, waveOpsAxis, waveSizeAxis } };

    auto validateAndPrint = [&space](std::string_view toml) -> CookError
    {
        StderrDiagnosticSink sink;
        const auto document = PolicyDocument::Parse(toml);
        if (!document)
        {
            return CookError::Invalid; // these inputs parse; a failure here fails the check below
        }
        return document->ValidateAgainstSpace("OceanFft", space, sink);
    };

    auto validateAndCache = [&space](std::string_view toml) -> CookError
    {
        RecordingDiagnosticSink sink;
        const auto document = PolicyDocument::Parse(toml);
        if (!document)
        {
            return CookError::Invalid; // these inputs parse; a failure here fails the check below
        }
        return document->ValidateAgainstSpace("OceanFft", space, sink);
    };

    runner.CheckEqual(validateAndPrint(k_ValidPolicy), CookError::Success, "a policy that names real axes and values validates");

    const CookError unknownCookAxis = validateAndCache("[OceanFft.targets.wgsl.CookValues]\nIFFT_SZ = [256]\n");
    runner.CheckEqual(unknownCookAxis, CookError::PolicyAxisNotDeclared, "CookValues on an undeclared axis fails");

    const CookError valueNotInAxis = validateAndCache("[OceanFft.targets.wgsl.CookValues]\nIFFT_SIZE = [999]\n");
    runner.CheckEqual(valueNotInAxis, CookError::PolicyValueNotInAxis, "a CookValues value the axis lacks fails");

    const CookError cookWhenUnknownAxis =
        validateAndCache("[OceanFft.targets.wgsl]\nCookIf = \"NOPE == 1\"\n");
    runner.CheckEqual(cookWhenUnknownAxis, CookError::PolicyAxisNotDeclared, "CookIf naming an undeclared axis fails");

    const CookError cookIfMalformed = validateAndCache("[OceanFft.targets.wgsl]\nCookIf = \"== 1\"\n");
    runner.CheckEqual(cookIfMalformed, CookError::PolicyCookIfInvalid, "a malformed CookIf fails");

    const CookError inertAxisUnknownAxis =
        validateAndCache("[OceanFft.InertAxesForEntryPoints]\ncs = [\"GHOST\"]\n");
    runner.CheckEqual(inertAxisUnknownAxis, CookError::PolicyAxisNotDeclared,
                      "InertAxesForEntryPoints on an undeclared axis fails");

    const CookError absentModuleValidates = [&space]
    {
        StderrDiagnosticSink sink;
        const auto document = PolicyDocument::Parse(k_ValidPolicy);
        return document ? document->ValidateAgainstSpace("SomeOtherModule", space, sink) : CookError::Invalid;
    }();
    runner.CheckEqual(absentModuleValidates, CookError::Success, "a module the file does not name is not an error");
}

} // namespace

void TestEnumCaseNames(TestRunner& runner)
{
    runner.BeginSection("CookValues of an enum axis are case names, resolved against the axis");

    // the axis takes Low and High, and not Medium
    std::vector<lodestone::RawEnumCase> cases{ { .Name = "Low", .Value = 5 },
                                               { .Name = "Medium", .Value = 10 },
                                               { .Name = "High", .Value = 1 } };
    std::vector<PermutationAxis> axes;
    axes.emplace_back("QUALITY",
                      std::vector<PermutationValue>{ PermutationValue::MakeEnum(0u), PermutationValue::MakeEnum(2u) },
                      AxisKind::Tuning,
                      EarliestBindingTime::Cook,
                      AxisValueDomain::Enum,
                      "",
                      "Quality",
                      "Probe",
                      std::move(cases));
    axes.emplace_back("FOG",
                      std::vector<PermutationValue>{ PermutationValue{ false }, PermutationValue{ true } },
                      AxisKind::Technique,
                      EarliestBindingTime::Cook,
                      AxisValueDomain::Boolean);
    const PermutationSpace space{ std::move(axes) };

    auto validate = [&space](std::string_view toml) -> CookError
    {
        RecordingDiagnosticSink sink;
        const auto document = PolicyDocument::Parse(toml);
        return document ? document->ValidateAgainstSpace("Probe", space, sink) : CookError::Invalid;
    };

    const auto document = PolicyDocument::Parse("[Probe.targets.spirv.CookValues]\nQUALITY = [\"High\"]\n");
    RecordingDiagnosticSink sink;
    if (runner.CheckOk(document, "a case that the axis takes validates"))
    {
        runner.CheckEqual(document->ValidateAgainstSpace("Probe", space, sink), CookError::Success,
                          "a case that the axis takes validates");
    }
    if (document.has_value())
    {
        const lodestone::CookResult<lodestone::VariantSet> variants =
            space.EnumerateVariants(document->FindTargetPolicy("Probe", "spirv"), sink);
        bool onlyHigh = variants.has_value() && variants->Variants.size() == 2u;
        for (size_t i = 0u; onlyHigh && i < variants->Variants.size(); ++i)
        {
            onlyHigh = variants->Variants[i].Active[0].Value == PermutationValue::MakeEnum(2u);
        }
        runner.Check(onlyHigh, "the cook takes High alone, crossed with both FOG values");
    }

    runner.CheckEqual(validate("[Probe.targets.spirv.CookValues]\nQUALITY = [\"Hihg\"]\n"), CookError::PolicyValueNotInAxis,
                      "a misspelled case fails");
    runner.CheckEqual(validate("[Probe.targets.spirv.CookValues]\nQUALITY = [\"Medium\"]\n"), CookError::PolicyValueNotInAxis,
                      "a case of the enum that the axis does not take fails");
    runner.CheckEqual(validate("[Probe.targets.spirv.CookValues]\nFOG = [\"true\"]\n"), CookError::PolicyValueNotInAxis,
                      "a name on an axis that is not an enum fails");
}

void TestTargetCoverage(TestRunner& runner)
{
    runner.BeginSection("target coverage");

    constexpr std::string_view k_CoveragePolicy = R"toml(
[InertOnly.InertAxesForEntryPoints]
MainCS = ["QUALITY"]

[WgslOnly.targets.wgsl]
MaxVariants = 64
)toml";

    const auto parsed = PolicyDocument::Parse(k_CoveragePolicy);
    if (!runner.CheckOk(parsed, "the coverage file parses"))
    {
        return;
    }
    const PolicyDocument& document = *parsed;

    runner.CheckEqual(document.FindTargetCoverage("Absent", "spirv"), TargetPolicyCoverage::NoModuleEntry,
                      "a module the file does not name has no entry");
    runner.CheckEqual(document.FindTargetCoverage("InertOnly", "spirv"), TargetPolicyCoverage::NoTargetSections,
                      "a module with influence statements only has no target sections");
    runner.CheckEqual(document.FindTargetCoverage("WgslOnly", "wgsl"), TargetPolicyCoverage::Present,
                      "a target with its own section is present");
    runner.CheckEqual(document.FindTargetCoverage("WgslOnly", "spirv"), TargetPolicyCoverage::OtherTargetsOnly,
                      "a target with no section, beside one that has a section, is the suspicious case");
}

// A target key the build does not know is a misspelling until proven otherwise, as an unknown --target is.
void TestTargetNames(TestRunner& runner)
{
    runner.BeginSection("target names");

    constexpr std::array<std::string_view, 2u> k_KnownTargets{ "wgsl", "spirv" };

    const auto known = PolicyDocument::Parse(k_ValidPolicy);
    if (runner.CheckOk(known, "the valid file parses"))
    {
        RecordingDiagnosticSink sink;
        runner.CheckEqual(known->ValidateTargetNames(k_KnownTargets, sink), CookError::Success,
                          "sections for known targets are accepted");
    }

    const auto misspelled = PolicyDocument::Parse("[OceanFft.targets.spriv]\nMaxVariants = 8\n");
    if (runner.CheckOk(misspelled, "a misspelled target still parses"))
    {
        RecordingDiagnosticSink sink;
        runner.CheckEqual(misspelled->ValidateTargetNames(k_KnownTargets, sink), CookError::PolicyUnknownTargetName,
                          "a section for an unknown target is rejected by name");
        if (runner.CheckEqual(sink.Records().size(), 1u, "the rejection reports one record"))
        {
            const std::string& message = sink.Records().front().Message;
            runner.Check(message.contains("spriv"), std::format("the record names the misspelled target: '{}'", message));
            runner.Check(message.contains("OceanFft"), std::format("the record names the module: '{}'", message));
            runner.Check(message.contains("Did you mean 'spirv'?"),
                         std::format("the record suggests the nearest target: '{}'", message));
        }
    }

    const auto noTargets = PolicyDocument::Parse("[OceanFft.InertAxesForEntryPoints]\nMainCS = [\"A\"]\n");
    if (noTargets)
    {
        RecordingDiagnosticSink sink;
        runner.CheckEqual(noTargets->ValidateTargetNames(k_KnownTargets, sink), CookError::Success,
                          "a file with no target sections has nothing to reject");
    }
}

int main()
{
    TestRunner runner{ "PolicyDocumentTests" };
    TestParseAndQuery(runner);
    TestParseRejections(runner);
    TestValidationAgainstSpace(runner);
    TestEnumCaseNames(runner);
    TestTargetCoverage(runner);
    TestTargetNames(runner);
    return runner.Report();
}
