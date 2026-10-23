#include "TestHarness.hpp"

#include "CookComparison.hpp"
#include "ShaderManifest.hpp"

#include <cstdint>
#include <filesystem>
#include <format>
#include <vector>

namespace
{

using namespace lodestone;
using tests::CookBothTargets;
using tests::CookedBundle;
using tests::ResourceKey;

CookedBundle Cook(const std::filesystem::path& module_path)
{
    return CookBothTargets({ module_path }, std::nullopt);
}

std::vector<ResourceKey> KeysOf(const manifest::EnvironmentView& environment, uint32_t entry_point)
{
    return tests::ResourceKeysOf(environment, 0u, entry_point);
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
