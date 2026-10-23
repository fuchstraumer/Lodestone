#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ResourceFlags.hpp"
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
#include <span>
#include <string>
#include <string_view>
#include <vector>


namespace
{

using namespace lodestone;

struct ExpectedRoles
{
    std::string_view Name;
    ResourceRoleFlags Roles;
    BufferUsageFlags Usage;
};

constexpr std::array<ExpectedRoles, 6u> k_Expected{ {
    { "DispatchArgsBuffer", ResourceRoleFlags::IndirectDispatch, BufferUsageFlags::Storage | BufferUsageFlags::Indirect },
    { "DrawWords", ResourceRoleFlags::IndirectDraw, BufferUsageFlags::Storage | BufferUsageFlags::Indirect },
    { "DrawIndexedRaw", ResourceRoleFlags::IndirectDrawIndexed, BufferUsageFlags::Storage | BufferUsageFlags::Indirect },
    { "SharedArgs",
      ResourceRoleFlags::IndirectDispatch | ResourceRoleFlags::IndirectDraw,
      BufferUsageFlags::Storage | BufferUsageFlags::Indirect },
    { "Indices", ResourceRoleFlags::IndexData, BufferUsageFlags::Storage | BufferUsageFlags::Index },
    { "Vertices", ResourceRoleFlags::VertexData, BufferUsageFlags::Storage | BufferUsageFlags::Vertex },
} };

CookResult<CookStatistics> CookIntoMemory(const std::filesystem::path& module_path,
                                          std::span<const std::string_view> targets,
                                          MemoryOutputSink& sink)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    for (const std::string_view target : targets)
    {
        options.TargetNames.emplace_back(target);
    }
    options.OutputPath = "ResourceRoles";
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

bool TagsAre(const manifest::ResolvedResource& resource, std::span<const std::string_view> expected)
{
    if (resource.TagCount() != expected.size())
    {
        return false;
    }
    for (uint32_t i = 0u; i < resource.TagCount(); ++i)
    {
        if (resource.Tag(i) != expected[i])
        {
            return false;
        }
    }
    return true;
}

void CheckEnvironment(tests::TestRunner& runner, const manifest::EnvironmentView& environment)
{
    const manifest::VariantView variant = environment.VariantAt(0u);
    const manifest::LayoutRange layout = variant.EntryPoint(0u).Layout();

    for (const ExpectedRoles& expected : k_Expected)
    {
        const std::optional<manifest::ResolvedResource> resource = FindResource(layout, expected.Name);
        if (runner.Check(resource.has_value(), std::format("{}: each buffer with a role is in the layout", expected.Name)))
        {
            runner.CheckEqual(resource->Roles(), expected.Roles,
                              std::format("{}: the roles read back as the shader declares them", expected.Name));
            runner.CheckEqual(resource->RequiredBufferUsage(), expected.Usage,
                              std::format("{}: the creation usage joins the binding's usage and the roles' usage", expected.Name));
        }
    }

    constexpr std::array<std::string_view, 2u> k_SharedTags{ "ShadowCascade", "debug.view" };
    const std::optional<manifest::ResolvedResource> taggedA = FindResource(layout, "TaggedA");
    const std::optional<manifest::ResolvedResource> taggedB = FindResource(layout, "TaggedB");
    runner.Check(taggedA.has_value() && TagsAre(*taggedA, k_SharedTags), "tags read back sorted");
    runner.Check(taggedB.has_value() && TagsAre(*taggedB, k_SharedTags),
                 "tags in another order, with a repeat, read back as the same sorted, unique tags");
    runner.Check(taggedA.has_value() && taggedA->Roles() == ResourceRoleFlags::None &&
                     taggedA->RequiredBufferUsage() == BufferUsageFlags::Storage,
                 "a buffer with no role needs only its binding's usage");

    const std::optional<manifest::ResolvedResource> vertices = FindResource(layout, "Vertices");
    runner.Check(vertices.has_value() && vertices->HasTag("Skinned") && !vertices->HasTag("ShadowCascade"),
                 "HasTag finds a tag of the resource and no other");
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat gives no arguments, so CMake sets LODESTONE_TEST_ASSET_DIR as the fallback
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "ResourceRoles" };

    runner.BeginSection("the roles and tags of each target read back through the client views");
    constexpr std::array<std::string_view, 2u> k_BothTargets{ "wgsl", "spirv" };
    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = CookIntoMemory(assetDirectory / "ResourceRoles.slang", k_BothTargets, sink);
    runner.CheckOk(cooked, "the module cooks for both targets");
    const std::vector<std::byte> bytes = ManifestBytes(sink);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (runner.CheckOk(bundle, "the manifest opens"))
    {
        for (uint32_t profile = 0u; profile < k_BothTargets.size(); ++profile)
        {
            const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(profile, 0u);
            if (runner.CheckOk(environment, std::format("profile {}: the environment opens", profile)))
            {
                CheckEnvironment(runner, environment.value());
            }
        }
    }

    runner.BeginSection("each broken rule fails the cook with its own error");
    struct Rejection
    {
        std::string_view File;
        CookError Error;
        std::string_view Description;
    };
    constexpr std::array<Rejection, 6u> k_Rejections{ {
        { "RoleOnTexture.slang", CookError::ResourceRoleKindMismatch, "a role on a texture fails" },
        { "RoleUnknown.slang", CookError::ResourceRoleUnknown, "a misspelled role fails" },
        { "RoleStrideMismatch.slang", CookError::ResourceRoleStrideMismatch, "a stride that is not the command's fails" },
        { "RoleCountMismatch.slang",
          CookError::ResourceRoleStrideMismatch,
          "a 4-byte stride with a count that is not whole commands fails" },
        { "RoleNeedsCount.slang", CookError::ResourceRoleNeedsElementCount, "a 4-byte stride with no count fails" },
        { "TagInvalid.slang", CookError::ResourceTagInvalid, "a tag with a character outside the tag set fails" },
    } };
    constexpr std::array<std::string_view, 1u> k_Wgsl{ "wgsl" };
    for (const Rejection& rejection : k_Rejections)
    {
        MemoryOutputSink rejectSink;
        const CookResult<CookStatistics> result = CookIntoMemory(assetDirectory / rejection.File, k_Wgsl, rejectSink);
        runner.CheckError(result, rejection.Error, rejection.Description);
    }

    return runner.Report();
}
