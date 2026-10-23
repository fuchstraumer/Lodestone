#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ResourceFlags.hpp"
#include "ShaderManifest.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "target/StorageFormatTable.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

/** The cases of `StorageFormat`, as the enum axis of the shim module reads them through reflection. */
std::vector<std::string> ReadEnumCases(const std::filesystem::path& module_path, const std::filesystem::path& cache_directory)
{
    RecordingDiagnosticSink sink;
    SlangCompiler compiler;
    SlangCompilerCreateInfo createInfo;
    createInfo.ModulePath = module_path;
    createInfo.ModuleCacheDirectory = cache_directory;
    createInfo.MultithreadVariantBuild = false;
    createInfo.ExpectedBatchSize = 1u;
    createInfo.Language = TargetLanguage::Spirv;
    createInfo.SlangProfileName = "spirv_1_5";
    createInfo.AccessModel = PlacementKind::Bound;
    if (compiler.Initialize(createInfo, sink) != CookError::Success)
    {
        return {};
    }

    const CookResult<std::vector<RawAxisDeclaration>> axes = compiler.BuildAxisDeclarations();
    std::vector<std::string> cases;
    if (axes.has_value() && axes->size() == 1u)
    {
        for (const RawEnumCase& enumCase : (*axes)[0].EnumCases)
        {
            cases.push_back(enumCase.Name);
        }
    }
    return cases;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    const std::filesystem::path cacheDirectory = std::filesystem::temp_directory_path() / "LodestoneStorageFormatTest";
    tests::TestRunner runner{ "StorageFormat" };

    runner.BeginSection("the table lists six formats, each once, with a texture format and a WGSL name");
    const std::span<const StorageFormatInfo> table = GetStorageFormats();
    runner.Check(table.size() == 6u, "six float formats");
    std::set<std::string_view> cases;
    std::set<TextureFormat> formats;
    bool complete = true;
    for (const StorageFormatInfo& info : table)
    {
        cases.insert(info.EnumCase);
        formats.insert(info.Format);
        complete = complete && info.Format != TextureFormat::Invalid && !info.SlangType.empty() && !info.WgslName.empty();
    }
    runner.Check(cases.size() == table.size() && formats.size() == table.size() && complete,
                 "no case or texture format repeats, and no field is empty");
    const StorageFormatInfo* snorm = FindStorageFormat("Rgba8Snorm");
    runner.Check(snorm != nullptr && snorm->Format == TextureFormat::RGBA8Snorm && FindStorageFormat("Rgba8Uint") == nullptr,
                 "a case is found by name, and an integer format is not in the table");

    runner.BeginSection("the enum in the shim module has the cases of the table, in the same order");
    const std::vector<std::string> enumCases = ReadEnumCases(assetDirectory / "StorageFormatAxis.slang", cacheDirectory);
    bool sameCases = enumCases.size() == table.size();
    for (size_t i = 0u; sameCases && i < table.size(); ++i)
    {
        sameCases = enumCases[i] == table[i].EnumCase;
    }
    runner.Check(!enumCases.empty() && sameCases, "StorageFormat and StorageFormatTable.cpp agree");

    runner.BeginSection("all three readers of a texture format give the format that the table states");
    CookerOptions options;
    options.ModulePaths.push_back(assetDirectory / "StorageFormatProbe.slang");
    options.TargetNames.emplace_back("wgsl");
    options.TargetNames.emplace_back("spirv");
    options.OutputPath = "StorageFormat";
    MemoryOutputSink sink;
    // the cook compares reflection with the emitted code of each target, so a cook that succeeds means the readers agree
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    runner.Check(cooked.has_value(), "a texture of each format cooks for both targets, with both cross-checks");

    const auto manifestArtifact = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
    if (cooked.has_value() && manifestArtifact != sink.GetArtifacts().end())
    {
        std::vector<std::byte> bytes(manifestArtifact->second.size());
        std::memcpy(bytes.data(), manifestArtifact->second.data(), bytes.size());
        const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
        for (const uint32_t profile : { 0u, 1u })
        {
            if (!bundle.has_value())
            {
                break;
            }
            const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(profile, 0u);
            if (!environment.has_value())
            {
                runner.Check(false, "the environment opens");
                continue;
            }

            // agreement alone is not enough: two readers that both give Invalid agree too
            std::map<std::string, TextureFormat> found;
            for (const manifest::ResolvedResource resource : environment->VariantAt(0u).EntryPoint(0u).Layout())
            {
                found[std::string{ resource.Name() }] = resource.StorageFormat();
            }
            for (const StorageFormatInfo& info : table)
            {
                const auto entry = found.find(std::string{ info.EnumCase });
                runner.Check(entry != found.end() && entry->second == info.Format,
                             std::format("profile {}: {} reads back as the format of the table", profile, info.EnumCase));
            }
        }
    }

    runner.BeginSection("the write-only storage shim stores to the format of each type argument, with write access");
    CookerOptions shimOptions;
    shimOptions.ModulePaths.push_back(assetDirectory / "ShimStorage.slang");
    shimOptions.TargetNames.emplace_back("wgsl");
    shimOptions.TargetNames.emplace_back("spirv");
    shimOptions.OutputPath = "StorageFormatShim";
    MemoryOutputSink shimSink;
    const CookResult<CookStatistics> shimCooked = RunCook(shimOptions, shimSink);
    runner.Check(shimCooked.has_value(), "a shim texture of each format cooks for both targets, with both cross-checks");

    const auto shimArtifact = shimSink.GetArtifacts().find(std::string{ k_ManifestFileName });
    if (shimCooked.has_value() && shimArtifact != shimSink.GetArtifacts().end())
    {
        std::vector<std::byte> bytes(shimArtifact->second.size());
        std::memcpy(bytes.data(), shimArtifact->second.data(), bytes.size());
        const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
        for (const uint32_t profile : { 0u, 1u })
        {
            const manifest::ManifestResult<manifest::EnvironmentView> environment =
                bundle.has_value() ? bundle->OpenEnvironment(profile, 0u) : std::unexpected(bundle.error());
            if (!environment.has_value())
            {
                runner.Check(false, "the environment opens");
                continue;
            }

            std::map<std::string, manifest::ResolvedResource> byScope;
            for (const manifest::ResolvedResource resource : environment->VariantAt(0u).EntryPoint(0u).Layout())
            {
                byScope.emplace(std::string{ resource.ScopeName() }, resource);
            }
            for (const StorageFormatInfo& info : table)
            {
                const auto entry = byScope.find(std::format("levels_{}", info.EnumCase));
                runner.Check(entry != byScope.end() && entry->second.Kind() == BindingKind::StorageTexture &&
                                 entry->second.Access() == ResourceAccess::WriteOnly &&
                                 entry->second.StorageFormat() == info.Format,
                             std::format("profile {}: {} is a write-only storage texture of its format", profile, info.EnumCase));
            }
        }
    }

    std::filesystem::remove_all(cacheDirectory);
    return runner.Report();
}
