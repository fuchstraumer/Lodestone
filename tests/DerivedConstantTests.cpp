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

// Derived constants in size expressions, end to end. `DerivedConstants.slang` sizes `gLightIndices` with
// `LIGHT_SLOTS`, a module-scope `static const` that uses another derived constant, which uses the two axes and a
// constant from an `__include`d fragment. Nothing else checks this value: reflection cannot fold the expression,
// and the element count is not in the emitted bindings. So this test computes each count by hand and compares it
// with the count read back from the manifest.

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
};

// LIGHT_SLOTS = CLUSTER_COUNT * LIGHTS_PER_CLUSTER = GRID_X * GRID_Y * 16
constexpr std::array<ExpectedCount, 4u> k_ExpectedCounts
{
    ExpectedCount{ .GridX = 4u, .GridY = 2u, .LightSlots = 128u },
    ExpectedCount{ .GridX = 4u, .GridY = 4u, .LightSlots = 256u },
    ExpectedCount{ .GridX = 8u, .GridY = 2u, .LightSlots = 256u },
    ExpectedCount{ .GridX = 8u, .GridY = 4u, .LightSlots = 512u },
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

/** Returns the element count of `gLightIndices` in the variant with the given axis values, or nothing when the
 * variant or the resource is not found. */
std::optional<uint64_t> FindLightSlots(const manifest::EnvironmentView& environment,
                                       const ManifestIndex& index,
                                       const ExpectedCount& expected)
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
        if (resource.Name() == "gLightIndices")
        {
            return resource.ElementCount();
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
        const std::optional<uint64_t> count = FindLightSlots(environment.value(), index, expected);
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
    }

    return runner.Report();
}
