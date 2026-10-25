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
#include <expected>
#include <filesystem>
#include <format>
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
    runner.CheckOk(cooked, "the module with one array of each resource kind cooks");
    const std::vector<std::byte> bytes = ManifestBytes(sink);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (runner.CheckOk(bundle, "the manifest opens"))
    {
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
        if (runner.CheckOk(environment, "the environment opens"))
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
            const char* pairClaim = "the sampler pairs with the texture array through a dynamic index";
            if (runner.CheckEqual(pairs.size(), 1u, pairClaim))
            {
                runner.CheckEqual(layout[pairs[0].Texture].Name(), "Views", pairClaim);
                runner.CheckEqual(layout[pairs[0].Sampler].Name(), "Samp", pairClaim);
            }
        }
    }

    runner.BeginSection("an unbounded binding array cooks for SPIR-V, and the manifest marks it unbounded");
    MemoryOutputSink unboundedSink;
    const CookResult<CookStatistics> unbounded =
        CookIntoMemory(assetDirectory / "UnboundedArray.slang", "spirv", unboundedSink);
    runner.CheckOk(unbounded, "the module with an unbounded array of textures and of storage buffers cooks");
    const std::vector<std::byte> unboundedBytes = ManifestBytes(unboundedSink);
    const manifest::ManifestResult<manifest::BundleView> unboundedBundle = manifest::BundleView::Open(unboundedBytes);
    const manifest::ManifestResult<manifest::EnvironmentView> unboundedEnvironment =
        unboundedBundle.has_value() ? unboundedBundle->OpenEnvironment(0u, 0u) : std::unexpected(unboundedBundle.error());
    if (runner.CheckOk(unboundedEnvironment, "the environment opens"))
    {
        const auto needs = [&unboundedEnvironment](const manifest::EntryPointInstanceView& instance, std::string_view name)
        {
            for (uint32_t i = 0u; i < unboundedEnvironment->CapabilityNames().size(); ++i)
            {
                if (unboundedEnvironment->CapabilityName(i) == name)
                {
                    return instance.RequiresCapability(i);
                }
            }
            return false;
        };

        const manifest::ModuleView module = unboundedBundle->Module(0u);
        const manifest::ManifestResult<uint32_t> uniformIndex = module.FindEntryPointIndex("UniformCS");
        const manifest::ManifestResult<uint32_t> nonUniformIndex = module.FindEntryPointIndex("NonUniformCS");
        if (runner.CheckOk(uniformIndex, "the entry point with a uniform index exists") &&
            runner.CheckOk(nonUniformIndex, "the entry point with a non-uniform index exists"))
        {
            const manifest::EntryPointInstanceView uniform = unboundedEnvironment->VariantAt(0u).EntryPoint(*uniformIndex);
            const manifest::LayoutRange layout = uniform.Layout();
            for (const std::string_view name : { "Views", "Buffers" })
            {
                const std::optional<manifest::ResolvedResource> resource = FindResource(layout, name);
                if (runner.Check(resource.has_value(), std::format("{} is in the layout", name)))
                {
                    runner.Check(resource->IsUnbounded(), std::format("{} is unbounded", name));
                    runner.CheckEqual(resource->ArrayCount(), 0u, std::format("{} has an array count of 0", name));
                    runner.CheckEqual(manifest::DescribeCreation(*resource).BindingCount, manifest::CreationSource::Cpu,
                                      std::format("the renderer chooses the descriptor count of {}", name));
                }
            }

            const std::optional<manifest::ResolvedResource> buffers = FindResource(layout, "Buffers");
            if (buffers.has_value())
            {
                const manifest::CreationFields fields = manifest::DescribeCreation(*buffers);
                runner.Check(fields.Size == manifest::CreationSource::Shader && !fields.IsComplete(),
                             "a buffer array with a stated size is not complete, because the count is the renderer's");
            }

            const std::optional<manifest::ResolvedResource> output = FindResource(layout, "Output");
            if (runner.Check(output.has_value(), "Output is in the layout"))
            {
                runner.Check(!output->IsUnbounded(), "a single resource is not unbounded");
                runner.CheckEqual(manifest::DescribeCreation(*output).BindingCount, manifest::CreationSource::NotApplicable,
                                  "a single resource has no descriptor count to choose");
            }

            runner.Check(needs(uniform, "RuntimeDescriptorArray"), "Slang states RuntimeDescriptorArray");
            runner.Check(needs(uniform, "SampledImageArrayDynamicIndexing") &&
                             needs(uniform, "StorageBufferArrayDynamicIndexing"),
                         "a dynamic index into an unbounded array states the device feature it needs");
            runner.Check(!needs(uniform, "SampledImageArrayNonUniformIndexing"),
                         "a dynamically uniform index needs no non-uniform feature");

            const manifest::EntryPointInstanceView nonUniform =
                unboundedEnvironment->VariantAt(0u).EntryPoint(*nonUniformIndex);
            // ShaderNonUniform comes with Vulkan 1.2, the floor of the profile, so the cook does not store it
            runner.Check(needs(nonUniform, "SampledImageArrayNonUniformIndexing"),
                         "NonUniformResourceIndex states SampledImageArrayNonUniformIndexing");
        }
    }

    runner.BeginSection("a fixed-size array has a descriptor count that the shader fixes");
    if (bundle.has_value())
    {
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
        const std::optional<manifest::ResolvedResource> levels =
            environment.has_value() ? FindResource(environment->VariantAt(0u).EntryPoint(0u).Layout(), "Levels") : std::nullopt;
        if (runner.Check(levels.has_value(), "Levels is in the layout"))
        {
            runner.Check(!levels->IsUnbounded(), "an array of four is not unbounded");
            runner.CheckEqual(manifest::DescribeCreation(*levels).BindingCount, manifest::CreationSource::Shader,
                              "the shader fixes the descriptor count of an array of four");
        }
    }

    runner.BeginSection("WGSL has no array of resources");
    MemoryOutputSink wgslSink;
    const CookResult<CookStatistics> wgsl = CookIntoMemory(assetDirectory / "BindingArrays.slang", "wgsl", wgslSink);
    runner.CheckError(wgsl, CookError::CodeGenerationFailed, "Slang rejects a fixed-size array of resources for WGSL");
    MemoryOutputSink unboundedWgslSink;
    const CookResult<CookStatistics> unboundedWgsl =
        CookIntoMemory(assetDirectory / "UnboundedArray.slang", "wgsl", unboundedWgslSink);
    runner.Check(!unboundedWgsl.has_value(), "an unbounded array does not cook for WGSL");

    return runner.Report();
}
