#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/ArtifactNames.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <cstddef>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// `ls_axis_interface_active_when` gates an interface axis on an earlier axis. The module has the boolean axis
// USE_TINT and the interface axis TINT with two types, gated on USE_TINT: 1 + 2 = 3 variants, not 2 x 2 = 4.

namespace
{

using namespace lodestone;

std::vector<std::byte> ManifestBytes(const MemoryOutputSink& sink)
{
    const std::map<std::string, std::string>& artifacts = sink.GetArtifacts();
    const auto manifest = artifacts.find(MakeArtifactName(k_DefaultOutputName, k_ManifestSuffix));
    if (manifest == artifacts.end())
    {
        return {};
    }

    std::vector<std::byte> bytes(manifest->second.size());
    std::memcpy(bytes.data(), manifest->second.data(), bytes.size());
    return bytes;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "InterfaceAxisActiveWhen" };

    runner.BeginSection("an interface axis gated on a boolean axis has no values where the gate is off");
    CookerOptions options;
    options.ModulePaths.push_back(assetDirectory / "InterfaceAxisActiveWhen.slang");
    options.TargetNames.emplace_back("wgsl");
    options.OutputPath = "InterfaceAxisActiveWhen";
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    if (!runner.CheckOk(cooked, "the module cooks"))
    {
        return runner.Report();
    }

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
    const QueryResult<std::vector<VariantKey>> allKeys = index.Query().Keys();
    if (runner.CheckOk(allKeys, "the query for every variant succeeds"))
    {
        runner.CheckEqual(allKeys->size(), size_t{ 3u }, "the gate leaves 1 + 2 variants, not 2 x 2");
    }

    return runner.Report();
}
