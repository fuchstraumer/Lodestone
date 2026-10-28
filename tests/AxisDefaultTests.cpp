#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// Every default in `AxisDefaults.slang` is a value other than the first, so a cook that confuses the declared default
// with the canonical value fails here.

namespace
{

using namespace lodestone;

struct RejectionCase
{
    std::string_view ModuleFile;
    CookError Expected;
    std::string_view Claim;
};

constexpr std::array<RejectionCase, 3u> k_RejectionCases
{
    RejectionCase{ .ModuleFile = "AxisDefaultMissing.slang",
                   .Expected = CookError::AxisDefaultMissing,
                   .Claim = "an axis with no initializer fails the cook" },
    RejectionCase{ .ModuleFile = "AxisDefaultNotDeclared.slang",
                   .Expected = CookError::AxisDefaultNotDeclared,
                   .Claim = "an initializer that is not one of the axis values fails the cook" },
    RejectionCase{ .ModuleFile = "AxisKindUnknown.slang",
                   .Expected = CookError::AxisKindUnknown,
                   .Claim = "an ls_axis_kind that spells no kind fails the cook" },
};

CookResult<CookStatistics> CookIntoMemory(const std::filesystem::path& module_path,
                                          std::optional<std::filesystem::path> policy_file,
                                          MemoryOutputSink& sink)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back("wgsl");
    options.OutputPath = "AxisDefault";
    options.PolicyFile = std::move(policy_file);
    return RunCook(options, sink);
}

/** Copies the manifest out of the sink into a buffer that the reader can map in place. */
std::vector<std::byte> ManifestBytes(const MemoryOutputSink& sink)
{
    const std::map<std::string, std::string>& artifacts = sink.GetArtifacts();
    const auto manifest = artifacts.find(std::string{ k_ManifestFileName });
    if (manifest == artifacts.end())
    {
        return {};
    }

    std::vector<std::byte> bytes(manifest->second.size());
    std::memcpy(bytes.data(), manifest->second.data(), bytes.size());
    return bytes;
}

void CheckIntegralDefault(tests::TestRunner& runner,
                          const ManifestIndex& index,
                          std::string_view axis_name,
                          AxisValueType expected,
                          std::string_view claim)
{
    const QueryResult<QueryAxisValue> value = index.AxisDefault(axis_name);
    if (runner.CheckOk(value, claim))
    {
        runner.CheckEqual(value->IntegralValue, expected, claim);
    }
}

void CheckNamedDefault(tests::TestRunner& runner,
                       const ManifestIndex& index,
                       std::string_view axis_name,
                       std::string_view expected,
                       std::string_view claim)
{
    const QueryResult<QueryAxisValue> value = index.AxisDefault(axis_name);
    if (runner.CheckOk(value, claim))
    {
        runner.CheckEqual(value->Name, expected, claim);
    }
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat gives no arguments, so CMake sets LODESTONE_TEST_ASSET_DIR as the fallback
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "AxisDefault" };

    for (const RejectionCase& testCase : k_RejectionCases)
    {
        runner.BeginSection(testCase.Claim);
        MemoryOutputSink sink;
        const CookResult<CookStatistics> result =
            CookIntoMemory(assetDirectory / testCase.ModuleFile, std::nullopt, sink);
        const CookError actual = result ? CookError::Success : result.error();
        runner.CheckEqual(actual, testCase.Expected, std::format("{}: {}", testCase.ModuleFile, testCase.Claim));
    }

    runner.BeginSection("a misspelled axis kind names the axis and suggests the kind it is closest to");
    {
        CookerOptions options;
        options.ModulePaths.push_back(assetDirectory / "AxisKindUnknown.slang");
        options.TargetNames.emplace_back("wgsl");
        options.OutputPath = "AxisDefault";
        MemoryOutputSink kindSink;
        RecordingDiagnosticSink diagnostics;
        const CookResult<CookStatistics> kindCook = RunCook(options, kindSink, diagnostics);
        runner.CheckEqual(kindCook ? CookError::Success : kindCook.error(), CookError::AxisKindUnknown,
                          "the cook fails on the misspelled kind");

        const std::vector<Diagnostic>& records = diagnostics.Records();
        const auto kindRecord = std::ranges::find_if(records,
                                                     [](const Diagnostic& record)
                                                     { return record.Message.contains("is not an axis kind"); });
        if (runner.Check(kindRecord != records.end(), "the cook reports one record for the unknown kind"))
        {
            const std::string& message = kindRecord->Message;
            runner.Check(message.contains("USE_WAVE_OPS"), std::format("the record names the axis: '{}'", message));
            runner.Check(message.contains("Did you mean 'Capability'?"),
                         std::format("the record suggests the nearest kind: '{}'", message));
        }
    }

    runner.BeginSection("an enum axis that no code reads is pruned, and its initializer is not read as a value");
    {
        MemoryOutputSink unreadSink;
        const CookResult<CookStatistics> unreadCook =
            CookIntoMemory(assetDirectory / "AxisDefaultUnread.slang", std::nullopt, unreadSink);
        runner.CheckOk(unreadCook, "the module cooks");
        const std::vector<std::byte> unreadBytes = ManifestBytes(unreadSink);
        const manifest::ManifestResult<manifest::BundleView> unreadBundle = manifest::BundleView::Open(unreadBytes);
        const manifest::ManifestResult<manifest::EnvironmentView> unreadEnvironment =
            unreadBundle.has_value() ? unreadBundle->OpenEnvironment(0u, 0u) : std::unexpected(unreadBundle.error());
        runner.CheckOk(unreadEnvironment, "the environment opens");
        if (unreadEnvironment.has_value())
        {
            const ManifestIndex unreadIndex{ unreadEnvironment.value() };
            runner.Check(!unreadIndex.AxisDefault("DETAIL").has_value() && unreadIndex.AxisDefault("USE_FOG").has_value(),
                         "DETAIL left the space and USE_FOG stayed");
        }
    }

    runner.BeginSection("every declared default reaches the client, and none of them is the first value");
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = CookIntoMemory(assetDirectory / "AxisDefaults.slang", std::nullopt, sink);
    runner.CheckOk(cooked, "the module with non-first defaults cooks");
    const std::vector<std::byte> bytes = ManifestBytes(sink);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (!runner.CheckOk(bundle, "the manifest opens"))
    {
        return runner.Report();
    }

    const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
    if (!runner.CheckOk(environment, "the environment opens"))
    {
        return runner.Report();
    }

    const ManifestIndex index{ environment.value() };
    CheckIntegralDefault(runner, index, "IFFT_SIZE", 512u, "a literal initializer: 512, the last of three values");
    CheckIntegralDefault(runner, index, "SAMPLES", 4u, "an expression initializer: 2u * 2u reads as 4");
    CheckIntegralDefault(runner, index, "USE_FOG", 1u, "a boolean initializer: true");
    CheckNamedDefault(runner, index, "DETAIL", "Medium", "an enum initializer: the case, not its tag or index");
    CheckNamedDefault(runner, index, "TINT", "WarmTint", "an interface initializer, which only the source text has");
    runner.Check(!index.AxisDefault("NOT_AN_AXIS").has_value(), "an unknown axis name is an error");

    // the cook prints a warning for this case. the test cannot read stderr, so it checks what the warning
    // describes: the cook succeeds, the default stays declared, and a query for it finds no variant
    runner.BeginSection("a policy that drops the default is a warning, not a cook failure");
    MemoryOutputSink policySink;
    const CookResult<CookStatistics> policyCook =
        CookIntoMemory(assetDirectory / "AxisDefaults.slang", assetDirectory / "AxisDefaults.toml", policySink);
    runner.CheckOk(policyCook, "the cook succeeds when the policy drops IFFT_SIZE = 512");
    const std::vector<std::byte> policyBytes = ManifestBytes(policySink);
    const manifest::ManifestResult<manifest::BundleView> policyBundle = manifest::BundleView::Open(policyBytes);
    if (!runner.CheckOk(policyBundle, "the manifest opens"))
    {
        return runner.Report();
    }

    const manifest::ManifestResult<manifest::EnvironmentView> policyEnvironment = policyBundle->OpenEnvironment(0u, 0u);
    if (!runner.CheckOk(policyEnvironment, "the environment opens"))
    {
        return runner.Report();
    }

    const ManifestIndex policyIndex{ policyEnvironment.value() };
    CheckIntegralDefault(runner, policyIndex, "IFFT_SIZE", 512u, "the manifest still records the declared default");
    const QueryResult<std::vector<VariantKey>> defaultKeys = policyIndex.Query().Where("IFFT_SIZE", 512u).Keys();
    runner.Check(defaultKeys.has_value() && defaultKeys->empty(), "a query for the default finds no variant");

    return runner.Report();
}
