#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

struct ExpectedArray
{
    std::string_view Name;
    BindingKind Kind;
    uint32_t ArrayCount;
};

constexpr std::array<ExpectedArray, 6u> k_Expected{ {
    { "Levels", BindingKind::StorageTexture, 4u },
    { "Views", BindingKind::Texture, 3u },
    { "Samp", BindingKind::Sampler, 1u },
    { "Weights", BindingKind::StorageBuffer, 2u },
    { "Tints", BindingKind::UniformBuffer, 2u },
    { "Output", BindingKind::StorageBuffer, 1u },
} };

CookResult<CookStatistics> CookIntoMemory(const std::filesystem::path& module_path,
                                          std::string_view target,
                                          MemoryOutputSink& sink)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target);
    options.OutputPath = "BindingArrays";
    return RunCook(options, sink);
}

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

std::optional<manifest::ResolvedResource> FindResource(const manifest::LayoutRange& layout, std::string_view name)
{
    for (const manifest::ResolvedResource resource : layout)
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
    tests::TestRunner runner{ "BindingArrays" };

    runner.BeginSection("a fixed-size binding array cooks for SPIR-V, and its length reaches the manifest");
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = CookIntoMemory(assetDirectory / "BindingArrays.slang", "spirv", sink);
    runner.Check(cooked.has_value(), "the module with one array of each resource kind cooks");
    const std::vector<std::byte> bytes = ManifestBytes(sink);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    runner.Check(bundle.has_value(), "the manifest opens");
    if (bundle.has_value())
    {
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
        runner.Check(environment.has_value(), "the environment opens");
        if (environment.has_value())
        {
            const manifest::EntryPointInstanceView instance = environment->VariantAt(0u).EntryPoint(0u);
            const manifest::LayoutRange layout = instance.Layout();
            for (const ExpectedArray& expected : k_Expected)
            {
                const std::optional<manifest::ResolvedResource> resource = FindResource(layout, expected.Name);
                runner.Check(resource.has_value() && resource->Kind() == expected.Kind &&
                                 resource->ArrayCount() == expected.ArrayCount,
                             "each resource reads back with its kind and the length the shader declares");
            }

            // Slang states none of these, and the shader indexes every array with the entry point constant
            constexpr std::array<std::string_view, 4u> k_Features{ "SampledImageArrayDynamicIndexing",
                                                                   "StorageImageArrayDynamicIndexing",
                                                                   "StorageBufferArrayDynamicIndexing",
                                                                   "UniformBufferArrayDynamicIndexing" };
            for (const std::string_view feature : k_Features)
            {
                bool needed = false;
                for (uint32_t i = 0u; i < environment->CapabilityNames().size(); ++i)
                {
                    needed = needed || (environment->CapabilityName(i) == feature && instance.RequiresCapability(i));
                }
                runner.Check(needed, "a dynamic index into an array states the device feature it needs");
            }

            const std::span<const manifest::SamplerTexturePair> pairs = instance.SamplerTexturePairs();
            runner.Check(pairs.size() == 1u && layout[pairs[0].Texture].Name() == "Views" &&
                             layout[pairs[0].Sampler].Name() == "Samp",
                         "the sampler pairs with the texture array through a dynamic index");
        }
    }

    runner.BeginSection("the forms that are not read yet fail with their own errors");
    MemoryOutputSink wgslSink;
    const CookResult<CookStatistics> wgsl = CookIntoMemory(assetDirectory / "BindingArrays.slang", "wgsl", wgslSink);
    runner.Check(!wgsl.has_value() && wgsl.error() == CookError::CodeGenerationFailed,
                 "WGSL has no array of resources, and Slang rejects it");

    MemoryOutputSink unboundedSink;
    const CookResult<CookStatistics> unbounded =
        CookIntoMemory(assetDirectory / "UnboundedArray.slang", "spirv", unboundedSink);
    runner.Check(!unbounded.has_value() && unbounded.error() == CookError::ReflectionMismatch,
                 "an unbounded array fails the SPIR-V cross-check until the Indexed access model reads it");

    return runner.Report();
}
