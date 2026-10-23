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
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

struct ExpectedBinding
{
    std::string_view ScopeName;
    std::string_view Name;
    BindingKind Kind;
    uint32_t Group;
    uint32_t Binding;
};

// the shim implementations name their resource by the member they hold, and the scope is the path to the shim
constexpr std::array<ExpectedBinding, 4u> k_BlockBindings{ {
    { "", "Output", BindingKind::StorageBuffer, 0u, 0u },
    { "material_Points", "data", BindingKind::StorageBuffer, 1u, 1u },
    { "material_Albedo", "texture", BindingKind::Texture, 1u, 2u },
    { "material_Linear", "sampler", BindingKind::Sampler, 1u, 3u },
} };

std::optional<manifest::ResolvedResource> FindResource(const manifest::LayoutRange& layout,
                                                       std::string_view scope_name,
                                                       std::string_view name)
{
    for (const manifest::ResolvedResource resource : layout)
    {
        if (resource.ScopeName() == scope_name && resource.Name() == name)
        {
            return resource;
        }
    }
    return std::nullopt;
}

bool IsAt(const manifest::ResolvedResource& resource, const ExpectedBinding& expected)
{
    const manifest::PlacementPayload placement = resource.PlacementValue();
    return resource.Kind() == expected.Kind && placement.Word0 == expected.Group && placement.Word1 == expected.Binding;
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat gives no arguments, so CMake sets LODESTONE_TEST_ASSET_DIR as the fallback
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "ShimModule" };

    runner.BeginSection("the shim module cooks for both targets, and both cross-checks agree");
    CookerOptions options;
    options.ModulePaths.push_back(assetDirectory / "ShimDirect.slang");
    options.TargetNames.emplace_back("wgsl");
    options.TargetNames.emplace_back("spirv");
    options.OutputPath = "ShimModule";
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    runner.Check(cooked.has_value(), "buffer, texture, and sampler shims in a block, and a shim in an entry point uniform");

    const std::map<std::string, std::string>& artifacts = sink.GetArtifacts();
    const auto manifestArtifact = artifacts.find(std::string{ k_ManifestFileName });
    if (!cooked.has_value() || manifestArtifact == artifacts.end())
    {
        return runner.Report();
    }

    std::vector<std::byte> bytes(manifestArtifact->second.size());
    std::memcpy(bytes.data(), manifestArtifact->second.data(), bytes.size());
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    runner.Check(bundle.has_value(), "the manifest opens");
    if (!bundle.has_value())
    {
        return runner.Report();
    }

    for (const uint32_t profile : { 0u, 1u })
    {
        runner.BeginSection(std::format("the bindings of profile {} read back as the shim implementations declare them", profile));
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(profile, 0u);
        runner.Check(environment.has_value(), "the environment opens");
        if (!environment.has_value())
        {
            continue;
        }

        const manifest::LayoutRange block = environment->VariantAt(0u).EntryPoint(0u).Layout();
        for (const ExpectedBinding& expected : k_BlockBindings)
        {
            const std::optional<manifest::ResolvedResource> resource = FindResource(block, expected.ScopeName, expected.Name);
            runner.Check(resource.has_value() && IsAt(*resource, expected),
                         "a shim resource has the kind and the placement of the resource it holds");
        }

        const std::optional<manifest::ResolvedResource> count = FindResource(block, "", "material");
        runner.Check(count.has_value() && count->Kind() == BindingKind::UniformBuffer,
                     "the ordinary data beside the shims is the uniform buffer of the block");

        const manifest::LayoutRange entry = environment->VariantAt(0u).EntryPoint(1u).Layout();
        const std::optional<manifest::ResolvedResource> scratch = FindResource(entry, "entryPointParams_Scratch", "data");
        runner.Check(scratch.has_value() && scratch->Kind() == BindingKind::StorageBuffer &&
                         scratch->Access() == ResourceAccess::ReadWrite,
                     "a shim in an entry point uniform is a read and write storage buffer");
    }

    return runner.Report();
}
