#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/ArtifactNames.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace lodestone;

struct ExpectedName
{
    std::string_view Scope;
    std::string_view Name;
};

// the innermost struct that is not a shim ends the scope, and the field of the shim is the name
constexpr std::array<ExpectedName, 7u> k_Expected{ {
    { "params", "First" },
    { "params", "Second" },
    { "params", "Surface" },
    { "params", "Filter" },
    { "params_Group", "Nested" },
    { "", "GlobalBuffer" },
    { "entryPointParams", "scratch" },
} };

// the names that the shim implementations give to their members
constexpr std::array<std::string_view, 3u> k_InnerNames{ "data", "texture", "sampler" };

constexpr std::array<std::string_view, 4u> k_Targets{ "wgsl", "spirv", "spirv-modern-mutable", "spirv-modern-typed" };

std::vector<std::byte> CookManifest(const std::filesystem::path& module_path, std::string_view target)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target);
    options.OutputPath = "ShimNames";

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

bool HasResource(const manifest::LayoutRange& layout, std::string_view scope, std::string_view name)
{
    for (const manifest::ResolvedResource resource : layout)
    {
        if (resource.ScopeName() == scope && resource.Name() == name)
        {
            return true;
        }
    }

    return false;
}

std::set<std::pair<std::string, std::string>> NamesOf(const manifest::LayoutRange& layout)
{
    std::set<std::pair<std::string, std::string>> names;
    for (const manifest::ResolvedResource resource : layout)
    {
        names.emplace(std::string{ resource.ScopeName() }, std::string{ resource.Name() });
    }

    return names;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "ShimName" };

    std::vector<std::set<std::pair<std::string, std::string>>> namesOfProfile;
    for (const std::string_view target : k_Targets)
    {
        runner.BeginSection(std::format("{}: each shim resource has the name of the field of the author", target));
        const std::vector<std::byte> bytes = CookManifest(assetDirectory / "ShimNames.slang", target);
        if (!runner.Check(!bytes.empty(), "the module cooks, so the reflection cross-check accepts the names"))
        {
            continue;
        }

        const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
        if (!runner.CheckOk(bundle, "the manifest opens"))
        {
            continue;
        }

        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
        if (!runner.CheckOk(environment, "the environment opens"))
        {
            continue;
        }

        const manifest::LayoutRange layout = environment->VariantAt(0u).EntryPoint(0u).Layout();
        for (const ExpectedName& expected : k_Expected)
        {
            runner.Check(HasResource(layout, expected.Scope, expected.Name),
                         std::format("{} has the resource '{}' in the scope '{}'", target, expected.Name, expected.Scope));
        }

        for (const manifest::ResolvedResource resource : layout)
        {
            for (const std::string_view innerName : k_InnerNames)
            {
                runner.Check(resource.Name() != innerName,
                             std::format("{}: the member name '{}' is not the name of a resource", target, innerName));
            }
        }

        namesOfProfile.push_back(NamesOf(layout));
    }

    runner.BeginSection("the four profiles agree on the scope and the name of every resource");
    for (const auto& names : namesOfProfile)
    {
        runner.Check(names == namesOfProfile.front(), "a profile has the same scopes and names as the first profile");
    }

    return runner.Report();
}
