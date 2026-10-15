#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// The declared default of an axis, end to end: the shader initializer is read at the cook, written to the
// manifest, and read back by the client query layer. `AxisDefaults.slang` declares every default as a value
// other than the first one, so a cook that confused the declared default with the canonical (first) value
// fails here. It covers all four value domains, a literal initializer (which Slang reflection also reads, so
// the two readers are compared), and an expression initializer (which only the source text can read).

namespace
{

using namespace lodestone;

struct RejectionCase
{
    std::string_view ModuleFile;
    CookError Expected;
    std::string_view Claim;
};

constexpr std::array<RejectionCase, 2u> k_RejectionCases
{
    RejectionCase{ .ModuleFile = "AxisDefaultMissing.slang",
                   .Expected = CookError::AxisDefaultMissing,
                   .Claim = "an axis with no initializer fails the cook" },
    RejectionCase{ .ModuleFile = "AxisDefaultNotDeclared.slang",
                   .Expected = CookError::AxisDefaultNotDeclared,
                   .Claim = "an initializer that is not one of the axis values fails the cook" },
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
    runner.Check(value.has_value() && value->IntegralValue == expected, claim);
}

void CheckNamedDefault(tests::TestRunner& runner,
                       const ManifestIndex& index,
                       std::string_view axis_name,
                       std::string_view expected,
                       std::string_view claim)
{
    const QueryResult<QueryAxisValue> value = index.AxisDefault(axis_name);
    runner.Check(value.has_value() && value->Name == expected, claim);
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
        if (actual != testCase.Expected)
        {
            std::println(stderr,
                         "[AxisDefault] {}: expected {}, and the cook returned {}",
                         testCase.ModuleFile,
                         ToString(testCase.Expected),
                         ToString(actual));
        }
        runner.Check(actual == testCase.Expected, testCase.Claim);
    }

    runner.BeginSection("every declared default reaches the client, and none of them is the first value");
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = CookIntoMemory(assetDirectory / "AxisDefaults.slang", std::nullopt, sink);
    runner.Check(cooked.has_value(), "the module with non-first defaults cooks");
    const std::vector<std::byte> bytes = ManifestBytes(sink);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    runner.Check(bundle.has_value(), "the manifest opens");
    if (!bundle.has_value())
    {
        return runner.Report();
    }

    const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
    runner.Check(environment.has_value(), "the environment opens");
    if (!environment.has_value())
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
    runner.Check(policyCook.has_value(), "the cook succeeds when the policy drops IFFT_SIZE = 512");
    const std::vector<std::byte> policyBytes = ManifestBytes(policySink);
    const manifest::ManifestResult<manifest::BundleView> policyBundle = manifest::BundleView::Open(policyBytes);
    runner.Check(policyBundle.has_value(), "the manifest opens");
    if (!policyBundle.has_value())
    {
        return runner.Report();
    }

    const manifest::ManifestResult<manifest::EnvironmentView> policyEnvironment = policyBundle->OpenEnvironment(0u, 0u);
    runner.Check(policyEnvironment.has_value(), "the environment opens");
    if (!policyEnvironment.has_value())
    {
        return runner.Report();
    }

    const ManifestIndex policyIndex{ policyEnvironment.value() };
    CheckIntegralDefault(runner, policyIndex, "IFFT_SIZE", 512u, "the manifest still records the declared default");
    const QueryResult<std::vector<VariantKey>> defaultKeys = policyIndex.Query().Where("IFFT_SIZE", 512u).Keys();
    runner.Check(defaultKeys.has_value() && defaultKeys->empty(), "a query for the default finds no variant");

    return runner.Report();
}
