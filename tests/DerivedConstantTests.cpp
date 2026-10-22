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
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// reflection cannot fold these expressions, so each count is computed by hand

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
    RejectionCase{ .ModuleFile = "DerivedConstantShadow.slang",
                   .Expected = CookError::DerivedConstantShadowsSymbol,
                   .Claim = "a function-local constant with the name of an axis fails the cook" },
    RejectionCase{ .ModuleFile = "DerivedConstantCycle.slang",
                   .Expected = CookError::ModuleLoadFailed,
                   .Claim = "Slang rejects two constants that use each other, before stage 4 can (E39999)" },
};

struct ExpectedCount
{
    uint32_t GridX;
    uint32_t GridY;
    uint64_t LightSlots;
    uint32_t ClusterCount;
};

// LIGHT_SLOTS = CLUSTER_COUNT * LIGHTS_PER_CLUSTER = GRID_X * GRID_Y * 16, and CLUSTER_COUNT = GRID_X * GRID_Y
constexpr std::array<ExpectedCount, 4u> k_ExpectedCounts
{
    ExpectedCount{ .GridX = 4u, .GridY = 2u, .LightSlots = 128u, .ClusterCount = 8u },
    ExpectedCount{ .GridX = 4u, .GridY = 4u, .LightSlots = 256u, .ClusterCount = 16u },
    ExpectedCount{ .GridX = 8u, .GridY = 2u, .LightSlots = 256u, .ClusterCount = 16u },
    ExpectedCount{ .GridX = 8u, .GridY = 4u, .LightSlots = 512u, .ClusterCount = 32u },
};

CookResult<CookStatistics> CookIntoMemory(const std::filesystem::path& module_path, MemoryOutputSink& sink)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back("wgsl");
    options.OutputPath = "DerivedConstants";
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

/** Returns the resource named `name` in the variant with the given axis values, or nothing when the variant or
 * the resource is not found. */
std::optional<manifest::ResolvedResource> FindResource(const manifest::EnvironmentView& environment,
                                                       const ManifestIndex& index,
                                                       const ExpectedCount& expected,
                                                       std::string_view name)
{
    const std::expected<VariantKey, QueryError> key =
        index.Query().Where("GRID_X", expected.GridX).Where("GRID_Y", expected.GridY).Single();
    if (!key)
    {
        return std::nullopt;
    }

    const std::optional<uint32_t> variantIndex = environment.FindVariant(key.value());
    if (!variantIndex)
    {
        return std::nullopt;
    }

    const manifest::VariantView variant = environment.VariantAt(*variantIndex);
    const manifest::EntryPointInstanceView entryPoint = variant.EntryPoint(0u);
    for (const manifest::ResolvedResource resource : entryPoint.Layout())
    {
        if (resource.Name() == name)
        {
            return resource;
        }
    }
    return std::nullopt;
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat gives no arguments, so CMake sets LODESTONE_TEST_ASSET_DIR as the fallback
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "DerivedConstant" };

    for (const RejectionCase& testCase : k_RejectionCases)
    {
        runner.BeginSection(testCase.Claim);
        MemoryOutputSink sink;
        const CookResult<CookStatistics> result = CookIntoMemory(assetDirectory / testCase.ModuleFile, sink);
        const CookError actual = result ? CookError::Success : result.error();
        if (actual != testCase.Expected)
        {
            std::println(stderr,
                         "[DerivedConstant] {}: expected {}, and the cook returned {}",
                         testCase.ModuleFile,
                         ToString(testCase.Expected),
                         ToString(actual));
        }
        runner.Check(actual == testCase.Expected, testCase.Claim);
    }

    runner.BeginSection("each variant sizes gLightIndices with its own value of LIGHT_SLOTS");
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = CookIntoMemory(assetDirectory / "DerivedConstants.slang", sink);
    runner.Check(cooked.has_value(), "the module with derived constants cooks");
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
    for (const ExpectedCount& expected : k_ExpectedCounts)
    {
        const std::optional<manifest::ResolvedResource> lightIndices =
            FindResource(environment.value(), index, expected, "gLightIndices");
        const std::optional<uint64_t> count =
            lightIndices.has_value() ? std::optional<uint64_t>{ lightIndices->ElementCount() } : std::nullopt;
        if (count != expected.LightSlots)
        {
            std::println(stderr,
                         "[DerivedConstant] GRID_X {} GRID_Y {}: expected {}, and the manifest has {}",
                         expected.GridX,
                         expected.GridY,
                         expected.LightSlots,
                         count.has_value() ? std::to_string(*count) : std::string{ "nothing" });
        }
        runner.Check(count == expected.LightSlots, "the element count equals GRID_X * GRID_Y * 16");

        // ls_extent_1d: the width follows the axes, and the two axes it does not set are 1
        const std::optional<manifest::ResolvedResource> ramp =
            FindResource(environment.value(), index, expected, "gClusterRamp");
        runner.Check(ramp.has_value() && ramp->ExtentX() == expected.ClusterCount && ramp->ExtentY() == 1u &&
                         ramp->ExtentZ() == 1u,
                     "the 1d extent equals GRID_X * GRID_Y, and its other axes are 1");
    }

    // task L16: the footprint axes are measured from the cooked variants, and the cook compares them with the axes
    // that each attribute names through LIGHT_SLOTS and CLUSTER_COUNT. Both resources reach GRID_X and GRID_Y
    runner.BeginSection("the footprint axes follow the derived constants back to the axes");
    const auto footprintAxisNames = [&](std::string_view resource_name)
    {
        std::vector<std::string_view> names;
        const std::optional<manifest::ResolvedResource> resource =
            FindResource(environment.value(), index, k_ExpectedCounts.front(), resource_name);
        if (!resource.has_value())
        {
            return names;
        }
        const manifest::ModuleView& module = environment->Module();
        for (uint32_t axis = 0u; axis < module.AxisCount(); ++axis)
        {
            if (resource->IsFootprintAxis(axis))
            {
                names.push_back(environment->String(module.AxisData(axis).NameString));
            }
        }
        return names;
    };
    const std::vector<std::string_view> k_GridAxes{ "GRID_X", "GRID_Y" };
    runner.Check(footprintAxisNames("gLightIndices") == k_GridAxes,
                 "the element count changes with GRID_X and GRID_Y, through two derived constants");
    runner.Check(footprintAxisNames("gClusterRamp") == k_GridAxes,
                 "the 1d extent changes with GRID_X and GRID_Y, through one derived constant");

    return runner.Report();
}
