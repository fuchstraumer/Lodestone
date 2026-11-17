#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

// Worker sessions load the root module from its serialized IR. The control arm (`--root-from-source`) parses it from
// source. Both arms must write the same bytes for every artifact, so each case cooks twice and compares them all.

namespace
{

using namespace lodestone;

struct RootCase
{
    std::string Name;
    std::vector<std::filesystem::path> Modules;
    std::vector<std::string> Targets;
    std::vector<std::filesystem::path> PolicyFiles;
    uint32_t OptimizationLevel;
    uint32_t DebugLevel;
};

struct CookOutput
{
    CookError Result{ CookError::Invalid };
    std::map<std::string, std::string> Artifacts;
    int32_t ErrorCount{ 0 };
};

CookOutput Cook(const RootCase& root_case, bool root_from_source)
{
    CookerOptions options;
    options.ModulePaths = root_case.Modules;
    options.TargetNames = root_case.Targets;
    options.PolicyFiles = root_case.PolicyFiles;
    options.OutputPath = "RootModuleIr";
    options.OptimizationLevel = root_case.OptimizationLevel;
    options.DebugLevel = root_case.DebugLevel;
    options.DumpStageMask = AllStageDumpBits();
    options.DumpSources = true;
    options.RootFromSource = root_from_source;

    MemoryOutputSink sink;
    RecordingDiagnosticSink diagnostics;
    const CookResult<CookStatistics> cooked = RunCook(options, sink, diagnostics);

    CookOutput output;
    output.Result = cooked.has_value() ? CookError::Success : cooked.error();
    output.Artifacts = sink.GetArtifacts();
    output.ErrorCount = static_cast<int32_t>(std::ranges::count_if(
        diagnostics.Records(), [](const Diagnostic& record) { return record.Severity == DiagnosticSeverity::Error; }));
    return output;
}

void CheckCase(lodestone::tests::TestRunner& runner, const RootCase& root_case)
{
    const auto context = runner.Context(root_case.Name);
    const CookOutput fromIr = Cook(root_case, false);
    const CookOutput fromSource = Cook(root_case, true);

    runner.CheckEqual(fromSource.Result, CookError::Success, "the cook from source succeeds");
    if (!runner.CheckEqual(fromIr.Result, CookError::Success, "the cook from IR succeeds") ||
        fromSource.Result != CookError::Success)
    {
        return;
    }

    runner.CheckEqual(fromIr.ErrorCount, 0, "the cook from IR reports no error");
    runner.Check(fromIr.Artifacts.size() > 3u, "the cook writes the manifest, the reports, and the stage dumps");
    runner.CheckEqual(fromIr.Artifacts.size(), fromSource.Artifacts.size(), "both arms write the same number of artifacts");

    for (const auto& [name, content] : fromIr.Artifacts)
    {
        const auto other = fromSource.Artifacts.find(name);
        if (runner.Check(other != fromSource.Artifacts.end(), std::format("the cook from source writes '{}'", name)))
        {
            runner.Check(content == other->second, std::format("'{}' has the same bytes in both arms", name));
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    lodestone::tests::TestRunner runner{ "RootModuleIr" };

    const std::filesystem::path kitchenSink = assetDirectory / "KitchenSink";
    const std::vector<std::filesystem::path> kitchenModules
    {
        kitchenSink / "KsGeometry.slang",
        kitchenSink / "KsMaterial.slang",
    };
    const std::vector<std::filesystem::path> kitchenPolicy{ kitchenSink / "KitchenSink.toml" };
    const std::vector<std::string> bothTargets{ "wgsl", "spirv" };

    const std::vector<RootCase> cases
    {
        RootCase{ "KitchenSink O0", kitchenModules, bothTargets, kitchenPolicy, 0u, 0u },
        RootCase{ "KitchenSink O2", kitchenModules, bothTargets, kitchenPolicy, 2u, 0u },
        RootCase{ "KitchenSink O2 g2", kitchenModules, bothTargets, kitchenPolicy, 2u, 2u },
        // `__include` of a fragment, in the root module
        RootCase{ "InterfaceAxis with __include", { assetDirectory / "InterfaceAxis" / "InterfaceAxisTest.slang" }, bothTargets, {}, 0u, 0u },
        RootCase{ "DerivedConstants with __include",
                  { assetDirectory / "DerivedConstants" / "DerivedConstants.slang" },
                  bothTargets,
                  {},
                  0u,
                  0u },
        // shim externs: a link module for each profile, and the heap lowering module of the typed profile
        RootCase{ "Shim externs",
                  { assetDirectory / "Shims" / "ShimMaterial.slang" },
                  { "wgsl", "spirv", "spirv-modern-mutable", "spirv-modern-typed" },
                  {},
                  0u,
                  0u },
        // a storage format axis: one link module for each value of the axis
        RootCase{ "Storage format axis",
                  { assetDirectory / "StorageFormatAxis" / "MipDownsample.slang" },
                  bothTargets,
                  { assetDirectory / "StorageFormatAxis" / "MipDownsample.toml" },
                  0u,
                  0u },
    };

    for (const RootCase& rootCase : cases)
    {
        const auto section = runner.Section(rootCase.Name);
        CheckCase(runner, rootCase);
    }

    return runner.Report();
}
