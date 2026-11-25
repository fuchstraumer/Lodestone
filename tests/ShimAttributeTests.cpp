#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/ArtifactNames.hpp"
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

struct ExpectedAnnotations
{
    std::string_view Scope;
    std::string_view Name;
    ResourceRoleFlags Roles;
    std::string_view Tags;
    uint64_t ElementCount;
    uint32_t ExtentX;
    uint32_t ExtentY;
};

// the Holder field has a tag, and its inner resource gets none: a plain user struct passes nothing down
constexpr std::array<ExpectedAnnotations, 7u> k_Expected{ {
    { "params", "Items", ResourceRoleFlags::IndexData, "Aliased:Indices", 16u, 0u, 0u },
    { "params", "Tally", ResourceRoleFlags::None, "Aliased:Counters", 4u, 0u, 0u },
    { "params", "Surface", ResourceRoleFlags::None, "Surface", 0u, 64u, 32u },
    { "params", "Filter", ResourceRoleFlags::None, "Filter", 0u, 0u, 0u },
    { "params_Plain", "Inner", ResourceRoleFlags::None, "", 0u, 0u, 0u },
    { "params", "Untagged", ResourceRoleFlags::None, "", 0u, 0u, 0u },
    { "", "GlobalTotals", ResourceRoleFlags::None, "Global:Totals", 2u, 0u, 0u },
} };

constexpr std::array<std::string_view, 4u> k_Targets{ "wgsl", "spirv", "spirv-modern-mutable", "spirv-modern-typed" };

std::vector<std::byte> CookManifest(const std::filesystem::path& module_path, std::string_view target)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target);
    options.OutputPath = "ShimAttributes";

    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    if (!cooked)
    {
        return {};
    }

    const auto manifest = sink.GetArtifacts().find(MakeArtifactName(k_DefaultOutputName, k_ManifestSuffix));
    std::vector<std::byte> bytes(manifest->second.size());
    std::memcpy(bytes.data(), manifest->second.data(), bytes.size());

    return bytes;
}

std::optional<manifest::ResolvedResource> FindResource(const manifest::LayoutRange& layout,
                                                       std::string_view scope,
                                                       std::string_view name)
{
    for (const manifest::ResolvedResource resource : layout)
    {
        if (resource.ScopeName() == scope && resource.Name() == name)
        {
            return resource;
        }
    }

    return std::nullopt;
}

std::string JoinedTags(const manifest::ResolvedResource& resource)
{
    std::string joined;
    for (uint32_t position = 0u; position < resource.TagCount(); ++position)
    {
        joined += joined.empty() ? "" : " ";
        joined += resource.Tag(position);
    }

    return joined;
}

std::string Summarize(const manifest::ResolvedResource& resource)
{
    return std::format("roles={} tags=[{}] count={} extent={}x{}",
                       static_cast<uint32_t>(resource.Roles()),
                       JoinedTags(resource),
                       resource.ElementCount(),
                       resource.ExtentX(),
                       resource.ExtentY());
}

void CheckProfile(tests::TestRunner& runner,
                  const std::filesystem::path& asset_directory,
                  std::string_view target,
                  std::map<std::string, std::string>& out_summaries)
{
    runner.BeginSection(std::format("{}: the attributes of each shim field reach its resource", target));
    const std::vector<std::byte> bytes = CookManifest(asset_directory / "ShimAttributes.slang", target);
    if (!runner.Check(!bytes.empty(), "the module cooks"))
    {
        return;
    }

    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (!runner.CheckOk(bundle, "the manifest opens"))
    {
        return;
    }

    const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
    if (!runner.CheckOk(environment, "the environment opens"))
    {
        return;
    }

    const manifest::LayoutRange layout = environment->VariantAt(0u).EntryPoint(0u).Layout();
    for (const ExpectedAnnotations& expected : k_Expected)
    {
        const std::optional<manifest::ResolvedResource> resource = FindResource(layout, expected.Scope, expected.Name);
        if (!runner.Check(resource.has_value(), std::format("{}.{} is in the layout", expected.Scope, expected.Name)))
        {
            continue;
        }

        const std::string claim = std::format("{}.{}", expected.Scope, expected.Name);
        runner.CheckEqual(resource->Roles(), expected.Roles, std::format("{}: roles", claim));
        runner.CheckEqual(JoinedTags(*resource), std::string{ expected.Tags }, std::format("{}: tags", claim));
        runner.CheckEqual(resource->ElementCount(), expected.ElementCount, std::format("{}: element count", claim));
        runner.CheckEqual(resource->ExtentX(), expected.ExtentX, std::format("{}: extent x", claim));
        runner.CheckEqual(resource->ExtentY(), expected.ExtentY, std::format("{}: extent y", claim));

        out_summaries.emplace(std::format("{}|{}", target, claim), Summarize(*resource));
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "ShimAttribute" };

    std::map<std::string, std::string> summaries;
    for (const std::string_view target : k_Targets)
    {
        CheckProfile(runner, assetDirectory, target, summaries);
    }

    runner.BeginSection("the four profiles agree on every resource");
    for (const ExpectedAnnotations& expected : k_Expected)
    {
        const std::string claim = std::format("{}.{}", expected.Scope, expected.Name);
        const auto first = summaries.find(std::format("{}|{}", k_Targets[0], claim));
        if (!runner.Check(first != summaries.end(), std::format("{}: the first profile read it", claim)))
        {
            continue;
        }

        for (const std::string_view target : k_Targets)
        {
            const auto other = summaries.find(std::format("{}|{}", target, claim));
            runner.Check(other != summaries.end() && other->second == first->second,
                         std::format("{}: {} equals {}", claim, target, k_Targets[0]));
        }
    }

    return runner.Report();
}
