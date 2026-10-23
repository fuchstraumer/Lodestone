#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace
{

using namespace lodestone;

/** What a renderer needs to create and bind one resource, without its name: the names differ by design. */
using ResourceKey = std::tuple<uint32_t, uint32_t, BindingKind, ResourceShape, ResourceAccess, uint32_t, uint32_t, uint64_t>;

struct CookedBundle
{
    std::vector<std::byte> Bytes;
    bool Cooked{ false };
};

CookedBundle Cook(const std::filesystem::path& module_path)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back("wgsl");
    options.TargetNames.emplace_back("spirv");
    options.OutputPath = "ShimCook";
    // two cooks of the module must agree byte for byte
    options.VerifyDeterministic = true;

    MemoryOutputSink sink;
    CookedBundle result;
    result.Cooked = RunCook(options, sink).has_value();
    const auto manifest = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
    if (manifest != sink.GetArtifacts().end())
    {
        result.Bytes.resize(manifest->second.size());
        std::memcpy(result.Bytes.data(), manifest->second.data(), result.Bytes.size());
    }
    return result;
}

std::vector<ResourceKey> KeysOf(const manifest::EnvironmentView& environment, uint32_t entry_point)
{
    std::vector<ResourceKey> keys;
    for (const manifest::ResolvedResource resource : environment.VariantAt(0u).EntryPoint(entry_point).Layout())
    {
        const manifest::PlacementPayload placement = resource.PlacementValue();
        keys.emplace_back(placement.Word0,
                          placement.Word1,
                          resource.Kind(),
                          resource.Shape(),
                          resource.Access(),
                          resource.ElementStride(),
                          resource.ArrayCount(),
                          resource.ByteSize());
    }
    std::ranges::sort(keys);
    return keys;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "ShimCook" };

    runner.BeginSection("a material written with shims cooks for both targets, with both cross-checks and two identical cooks");
    const CookedBundle shim = Cook(assetDirectory / "ShimMaterial.slang");
    const CookedBundle control = Cook(assetDirectory / "ShimControl.slang");
    runner.Check(shim.Cooked, "the shim material cooks");
    runner.Check(control.Cooked, "the control cooks");

    const manifest::ManifestResult<manifest::BundleView> shimBundle = manifest::BundleView::Open(shim.Bytes);
    const manifest::ManifestResult<manifest::BundleView> controlBundle = manifest::BundleView::Open(control.Bytes);
    if (shimBundle.has_value() && controlBundle.has_value())
    {
        for (const uint32_t profile : { 0u, 1u })
        {
            runner.BeginSection(std::format("profile {}: a shim resource sits where the resource type itself sits", profile));
            const manifest::ManifestResult<manifest::EnvironmentView> shimEnvironment = shimBundle->OpenEnvironment(profile, 0u);
            const manifest::ManifestResult<manifest::EnvironmentView> controlEnvironment = controlBundle->OpenEnvironment(profile, 0u);
            runner.Check(shimEnvironment.has_value() && controlEnvironment.has_value(), "both environments open");
            if (!shimEnvironment.has_value() || !controlEnvironment.has_value())
            {
                continue;
            }

            for (uint32_t entryPoint = 0u; entryPoint < 2u; ++entryPoint)
            {
                // group, binding, kind, shape, access, stride, array length, and byte size: a global that moved down a set,
                // or a buffer that lost its element type, shows here
                const std::vector<ResourceKey> shimKeys = KeysOf(*shimEnvironment, entryPoint);
                runner.Check(!shimKeys.empty() && shimKeys == KeysOf(*controlEnvironment, entryPoint),
                             "the same resources at the same places, with the same shape, stride, and size");
            }
        }
    }

    runner.BeginSection("a shim extern used as a local variable does not cook");
    const CookedBundle local = Cook(assetDirectory / "ShimLocal.slang");
    runner.Check(!local.Cooked, "a resource cannot be a local, and the cook fails (phase F step F2.0)");

    return runner.Report();
}
