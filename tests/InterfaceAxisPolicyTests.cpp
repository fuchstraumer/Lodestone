#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "ShaderManifestLayout.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "emit/StageDump.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// The interface axis GRAPH of PolicyGraph.slang has one tagged type (Constant). The policy file adds an instantiation
// of the generic Composed. The control module writes the same type as a typealias, so it has the layout that Slang
// gives to that expression.

namespace
{

using namespace lodestone;

constexpr std::string_view k_ComposedName = "Composed<LeafA,LeafB>";

struct PolicyCook
{
    CookError Result{ CookError::Invalid };
    std::vector<std::byte> Bytes;
    std::vector<Diagnostic> Records;
    std::string SpaceDump;
};

PolicyCook CookPolicy(const std::filesystem::path& module_path,
                      std::optional<std::filesystem::path> policy_file,
                      std::string_view target_name)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target_name);
    options.OutputPath = "InterfaceAxisPolicy";
    options.DumpStageMask = StageDumpBit(StageDumpKind::Space);
    if (policy_file.has_value())
    {
        options.PolicyFiles.push_back(std::move(*policy_file));
    }

    MemoryOutputSink sink;
    RecordingDiagnosticSink diagnostics;
    const CookResult<CookStatistics> cooked = RunCook(options, sink, diagnostics);

    PolicyCook result;
    result.Result = cooked ? CookError::Success : cooked.error();
    result.Records = diagnostics.Records();
    for (const auto& [name, text] : sink.GetArtifacts())
    {
        if (name == k_ManifestFileName)
        {
            result.Bytes.resize(text.size());
            std::memcpy(result.Bytes.data(), text.data(), result.Bytes.size());
        }
        else if (name.ends_with("_Space.json"))
        {
            result.SpaceDump = text;
        }
    }

    return result;
}

const Diagnostic* FindRecord(const PolicyCook& cook, std::string_view text)
{
    const auto found = std::ranges::find_if(cook.Records,
                                            [text](const Diagnostic& record)
                                            { return record.Severity == DiagnosticSeverity::Error &&
                                                     record.Message.contains(text); });
    return found == cook.Records.end() ? nullptr : &*found;
}

/** The members of the block that holds `gGraph`, with the byte size of the block. */
struct BlockLayout
{
    bool Found{ false };
    uint64_t ByteSize{ 0u };
    std::vector<std::string> Members;
};

BlockLayout ReadGraphBlock(const manifest::EnvironmentView& environment, VariantKey key)
{
    BlockLayout block;
    const std::optional<uint32_t> variant = environment.FindVariant(key);
    if (!variant.has_value())
    {
        return block;
    }

    const manifest::EntryPointInstanceView instance = environment.VariantAt(*variant).EntryPoint(0u);
    for (const manifest::ResolvedResource resource : instance.Layout())
    {
        if (resource.Name() != "gGraph")
        {
            continue;
        }

        block.Found = true;
        block.ByteSize = resource.ByteSize();
        for (const manifest::UniformMemberView member : resource.Members())
        {
            block.Members.push_back(
                std::format("{}@{}+{}", member.Name(), member.Offset(), member.Size()));
        }
    }

    return block;
}

void CheckPolicyTarget(tests::TestRunner& runner, const std::filesystem::path& asset_directory, std::string_view target)
{
    const tests::ScopedContext context = runner.Context(std::string{ target });

    const PolicyCook subject =
        CookPolicy(asset_directory / "PolicyGraph.slang", asset_directory / "PolicyGood.toml", target);
    const PolicyCook control = CookPolicy(asset_directory / "PolicyGraphControl.slang", std::nullopt, target);
    runner.CheckEqual(subject.Result, CookError::Success, "the module with a policy type cooks");
    runner.CheckEqual(control.Result, CookError::Success, "the control module cooks");

    {
        const tests::ScopedSection section = runner.Section("the space has the tagged type and the policy type");
        runner.Check(subject.SpaceDump.contains("\"Constant\"") && subject.SpaceDump.contains("\"Composed<LeafA,LeafB>\""),
                     std::format("the space dump lists both values: {}", subject.SpaceDump));
        runner.Check(subject.SpaceDump.find("\"Constant\"") < subject.SpaceDump.find("\"Composed<LeafA,LeafB>\""),
                     "the policy type comes after the tagged type");

        const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(subject.Bytes);
        if (!runner.CheckOk(bundle, "the manifest opens"))
        {
            return;
        }

        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
        if (!runner.CheckOk(environment, "the environment opens"))
        {
            return;
        }

        const ManifestIndex index{ *environment };
        const QueryResult<std::vector<QueryAxisValue>> values = index.AxisValues("GRAPH");
        if (runner.CheckOk(values, "the manifest lists the values of GRAPH") &&
            runner.CheckEqual(values->size(), 2u, "GRAPH has two values"))
        {
            runner.CheckEqual((*values)[0].Name, "Constant", "the first value is the tagged type");
            runner.CheckEqual((*values)[1].Name, k_ComposedName,
                              "the policy type is stored with the policy string, without whitespace");
        }

        runner.CheckEqual(index.Query().Where("USE_BIAS", false).Keys()->size(), 2u,
                          "each value of GRAPH is cooked for each value of USE_BIAS");

        const std::expected<VariantKey, QueryError> subjectKey = index.Query()
                                                                     .Where("GRAPH", AxisValueDomain::Type, k_ComposedName)
                                                                     .Where("USE_BIAS", false)
                                                                     .Single();
        if (!runner.CheckOk(subjectKey, "the policy value is found by name"))
        {
            return;
        }

        const manifest::ManifestResult<manifest::BundleView> controlBundle = manifest::BundleView::Open(control.Bytes);
        if (!runner.CheckOk(controlBundle, "the control manifest opens"))
        {
            return;
        }

        const manifest::ManifestResult<manifest::EnvironmentView> controlEnvironment =
            controlBundle->OpenEnvironment(0u, 0u);
        if (!runner.CheckOk(controlEnvironment, "the control environment opens"))
        {
            return;
        }

        const ManifestIndex controlIndex{ *controlEnvironment };
        const std::expected<VariantKey, QueryError> controlKey = controlIndex.Query().Single();
        if (!runner.CheckOk(controlKey, "the control has one variant"))
        {
            return;
        }

        const BlockLayout subjectBlock = ReadGraphBlock(*environment, *subjectKey);
        const BlockLayout controlBlock = ReadGraphBlock(*controlEnvironment, *controlKey);
        if (runner.Check(subjectBlock.Found && controlBlock.Found, "both modules have the block gGraph"))
        {
            runner.Check(controlBlock.ByteSize > 0u && !controlBlock.Members.empty(), "the control block has members");
            runner.CheckEqual(subjectBlock.ByteSize, controlBlock.ByteSize,
                              "the policy type has the byte size of the typealias");
            runner.CheckEqual(subjectBlock.Members, controlBlock.Members,
                              "the policy type has the member offsets of the typealias");
        }
    }
}

struct RejectionCase
{
    std::string_view PolicyFile;
    CookError Expected;
    std::string_view MessagePart;
    std::string_view Claim;
};

constexpr std::array<RejectionCase, 5u> k_RejectionCases
{
    RejectionCase{ .PolicyFile = "PolicyNonConforming.toml",
                   .Expected = CookError::PolicyInterfaceTypeInvalid,
                   .MessagePart = "Composed<LeafA,BadLeaf>",
                   .Claim = "a policy type that does not conform fails the cook" },
    RejectionCase{ .PolicyFile = "PolicyUnknownAxis.toml",
                   .Expected = CookError::PolicyAxisNotInterface,
                   .MessagePart = "NO_SUCH_AXIS",
                   .Claim = "a policy entry for an axis that the module does not declare fails the cook" },
    RejectionCase{ .PolicyFile = "PolicyNotInterface.toml",
                   .Expected = CookError::PolicyAxisNotInterface,
                   .MessagePart = "USE_BIAS",
                   .Claim = "a policy entry for an axis that is not an interface axis fails the cook" },
    RejectionCase{ .PolicyFile = "PolicyAlias.toml",
                   .Expected = CookError::PolicyInterfaceTypeNameMismatch,
                   .MessagePart = "write 'Composed<LeafA,LeafB>'",
                   .Claim = "a policy type spelled through a typealias fails the name check" },
    RejectionCase{ .PolicyFile = "PolicyDuplicate.toml",
                   .Expected = CookError::PolicyInterfaceTypeInvalid,
                   .MessagePart = "repeats",
                   .Claim = "a policy type that repeats a tagged type fails the cook" },
};

void CheckRejection(tests::TestRunner& runner,
                    const std::filesystem::path& asset_directory,
                    const RejectionCase& rejection,
                    std::string_view target)
{
    const tests::ScopedContext context = runner.Context(std::string{ target });
    const std::filesystem::path policyPath = asset_directory / rejection.PolicyFile;
    const PolicyCook cook = CookPolicy(asset_directory / "PolicyGraph.slang", policyPath, target);
    runner.CheckEqual(cook.Result, rejection.Expected, rejection.Claim);

    const Diagnostic* record = FindRecord(cook, rejection.MessagePart);
    if (!runner.Check(record != nullptr, std::format("{}: an error record names '{}'", rejection.Claim, rejection.MessagePart)))
    {
        return;
    }

    runner.Check(record->File == policyPath.generic_string() || record->File == policyPath.string(),
                 std::format("{}: the record names the policy file, not '{}'", rejection.Claim, record->File));
    runner.CheckEqual(record->Range.StartLine, 2, std::format("{}: the record names the line of the entry", rejection.Claim));
}

void CheckSpacing(tests::TestRunner& runner, const std::filesystem::path& asset_directory, std::string_view target)
{
    const tests::ScopedContext context = runner.Context(std::string{ target });
    const PolicyCook cook =
        CookPolicy(asset_directory / "PolicyGraph.slang", asset_directory / "PolicySpacing.toml", target);
    if (!runner.CheckEqual(cook.Result, CookError::Success, "a policy type with extra and missing spaces cooks"))
    {
        return;
    }

    runner.Check(cook.SpaceDump.contains("\"Composed<LeafA,LeafB>\""),
                 std::format("the space stores the type without whitespace: {}", cook.SpaceDump));
}

constexpr std::string_view k_CookValuesPolicy = R"toml(
[PolicyGraph.axes.GRAPH]
Types = [ { Type = "Composed<LeafA, LeafB>", Imports = ["PolicyLeaves"] } ]

[PolicyGraph.targets.wgsl.CookValues]
GRAPH = ["Composed < LeafA,LeafB >"]
)toml";

void CheckCookValuesByTypeName(tests::TestRunner& runner, const std::filesystem::path& asset_directory)
{
    const tests::ScopedSection section = runner.Section("CookValues lists interface values by type name");
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / "LodestoneCookValuesTypeNames";
    std::filesystem::create_directories(scratch);

    const std::filesystem::path knownPath = scratch / "Known.toml";
    {
        std::ofstream file{ knownPath };
        file << k_CookValuesPolicy;
    }

    const std::filesystem::path unknownPath = scratch / "Unknown.toml";
    {
        std::ofstream file{ unknownPath };
        file << "[PolicyGraph.targets.wgsl.CookValues]\nGRAPH = [\"Constnat\"]\n";
    }

    const PolicyCook known = CookPolicy(asset_directory / "PolicyGraph.slang", knownPath, "wgsl");
    runner.CheckEqual(known.Result, CookError::Success, "a policy type name in CookValues cooks");
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(known.Bytes);
    if (runner.CheckOk(bundle, "the manifest opens"))
    {
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
        if (runner.CheckOk(environment, "the environment opens"))
        {
            const ManifestIndex index{ *environment };
            runner.CheckEqual(index.Query().Keys()->size(), 2u, "only the listed type is cooked, for each USE_BIAS");
            runner.Check(index.Query().Where("GRAPH", AxisValueDomain::Type, k_ComposedName).Keys()->size() == 2u,
                         "the cooked type is the listed one");
        }
    }

    const PolicyCook unknown = CookPolicy(asset_directory / "PolicyGraph.slang", unknownPath, "wgsl");
    runner.CheckEqual(unknown.Result, CookError::PolicyValueNotInAxis, "an unknown type name in CookValues fails");
    runner.Check(FindRecord(unknown, "Constnat") != nullptr, "the error names the value");

    std::error_code ignored;
    std::filesystem::remove_all(scratch, ignored);
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "InterfaceAxisPolicy" };

    for (const std::string_view target : { "wgsl", "spirv" })
    {
        CheckPolicyTarget(runner, assetDirectory, target);
    }

    for (const std::string_view target : { "wgsl", "spirv" })
    {
        const tests::ScopedSection section = runner.Section("a policy type with unusual spacing");
        CheckSpacing(runner, assetDirectory, target);
    }

    CheckCookValuesByTypeName(runner, assetDirectory);

    for (const RejectionCase& rejection : k_RejectionCases)
    {
        const tests::ScopedSection section = runner.Section(rejection.Claim);
        for (const std::string_view target : { "wgsl", "spirv" })
        {
            CheckRejection(runner, assetDirectory, rejection, target);
        }
    }

    return runner.Report();
}
