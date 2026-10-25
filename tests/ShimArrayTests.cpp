#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestFormat.hpp"
#include "TestHarness.hpp"

#include "CookComparison.hpp"
#include "CookerErrors.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "VariantKey.hpp"
#include "target/StorageFormatTable.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

constexpr std::string_view k_DynamicIndexing = "StorageImageArrayDynamicIndexing";
constexpr uint32_t k_LevelCount = 16u;

/** The subject with the literal type of one format. The store narrows the float4 to the texel of the format by hand. */
std::string MakeControlSource(const StorageFormatInfo& format)
{
    const std::string_view narrow = format.SlangType == "R32F" ? ".x" : format.SlangType == "RG32F" ? ".xy" : "";
    return std::format(R"(module PyramidControl_{0};

struct Levels
{{
    WTexture2D<{1}> levels[16];
}};

ParameterBlock<Levels> pyramid;

[shader("compute")]
[numthreads(8, 8, 1)]
void WriteLevelCS(uint3 id : SV_DispatchThreadID, uniform uint level)
{{
    pyramid.levels[level].Store(id.xy, float4(0.25f){2});
}}

[shader("compute")]
[numthreads(8, 8, 1)]
void WriteLevel3CS(uint3 id : SV_DispatchThreadID)
{{
    pyramid.levels[3].Store(id.xy, float4(0.25f){2});
}}
)",
                       format.EnumCase,
                       format.SlangType,
                       narrow);
}

std::optional<uint32_t> FindVariantOf(const manifest::EnvironmentView& environment, std::string_view enum_case)
{
    const ManifestIndex index{ environment };
    const std::expected<VariantKey, QueryError> key = index.Query().Where("LEVEL_FORMAT", AxisValueDomain::Enum, enum_case).Single();
    return key.has_value() ? environment.FindVariant(*key) : std::nullopt;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    const std::filesystem::path controlDirectory = std::filesystem::temp_directory_path() / "LodestoneShimArrayTest";
    tests::TestRunner runner{ "ShimArray" };
    const std::span<const StorageFormatInfo> table = GetStorageFormats();

    std::filesystem::create_directories(controlDirectory);
    std::vector<std::filesystem::path> controlPaths;
    for (const StorageFormatInfo& format : table)
    {
        controlPaths.push_back(controlDirectory / std::format("PyramidControl_{}.slang", format.EnumCase));
        std::ofstream{ controlPaths.back() } << MakeControlSource(format);
    }

    {
        const tests::ScopedSection section = runner.Section("an array of storage shims cooks for spirv to what its controls bind");
        const tests::CookedBundle subject = tests::CookForTargets({ assetDirectory / "Pyramid.slang" }, std::nullopt, { "spirv" });
        const tests::CookedBundle controls = tests::CookForTargets(controlPaths, std::nullopt, { "spirv" });
        runner.CheckEqual(subject.Result, CookError::Success, "the subject cooks, with the cross-check and two identical cooks");
        runner.CheckEqual(controls.Result, CookError::Success, "the six controls cook");

        const manifest::ManifestResult<manifest::BundleView> subjectBundle = manifest::BundleView::Open(subject.Bytes);
        const manifest::ManifestResult<manifest::BundleView> controlBundle = manifest::BundleView::Open(controls.Bytes);
        const manifest::ManifestResult<manifest::EnvironmentView> environment =
            subjectBundle.has_value() ? subjectBundle->OpenEnvironment(0u, 0u) : std::unexpected(subjectBundle.error());
        if (runner.CheckOk(controlBundle, "the control bundle opens") && runner.CheckOk(environment, "the subject environment opens") &&
            runner.Check(environment->HasCodeFacts(), "the capability names were read"))
        {
            for (const StorageFormatInfo& format : table)
            {
                const tests::ScopedContext context = runner.Context(std::format("format {}", format.EnumCase));
                const int32_t controlModule = controlBundle->FindModule(std::format("PyramidControl_{}", format.EnumCase));
                if (!runner.Check(controlModule >= 0, "the control module is in the bundle"))
                {
                    continue;
                }
                const manifest::ManifestResult<manifest::EnvironmentView> control =
                    controlBundle->OpenEnvironment(0u, static_cast<uint32_t>(controlModule));
                const std::optional<uint32_t> variant = FindVariantOf(*environment, format.EnumCase);
                if (!runner.CheckOk(control, "the control environment opens") ||
                    !runner.Check(variant.has_value(), "the variant of the format is found"))
                {
                    continue;
                }

                for (const std::string_view entryPointName : { "WriteLevelCS", "WriteLevel3CS" })
                {
                    const tests::ScopedContext entryContext = runner.Context(std::format("entry point {}", entryPointName));
                    const manifest::ManifestResult<uint32_t> subjectEntry =
                        subjectBundle->Module(0u).FindEntryPointIndex(entryPointName);
                    const manifest::ManifestResult<uint32_t> controlEntry =
                        controlBundle->Module(static_cast<uint32_t>(controlModule)).FindEntryPointIndex(entryPointName);
                    if (!runner.CheckOk(subjectEntry, "the subject has the entry point") ||
                        !runner.CheckOk(controlEntry, "the control has the entry point"))
                    {
                        continue;
                    }

                    const std::vector<tests::ResourceKey> keys = tests::ResourceKeysOf(*environment, *variant, *subjectEntry);
                    runner.CheckEqual(keys.size(), 1u, "the array is one binding");
                    if (!keys.empty())
                    {
                        runner.CheckEqual(std::get<BindingKind>(keys.front()), BindingKind::StorageTexture, "a storage texture");
                        runner.CheckEqual(std::get<6>(keys.front()), k_LevelCount, "an array of 16");
                        runner.CheckEqual(std::get<TextureFormat>(keys.front()), format.Format, "the format of the variant");
                    }
                    runner.Check(keys == tests::ResourceKeysOf(*control, 0u, *controlEntry),
                                 "the same resources at the same places as the control");

                    const std::vector<std::string_view> names = tests::CapabilityNamesOf(*environment, *variant, *subjectEntry);
                    runner.Check(names == tests::CapabilityNamesOf(*control, 0u, *controlEntry),
                                 "the same capability names as the control");
                    const bool dynamic = std::ranges::contains(names, k_DynamicIndexing);
                    runner.CheckEqual(dynamic,
                                      entryPointName == "WriteLevelCS",
                                      "only the dynamic index needs StorageImageArrayDynamicIndexing");
                }
            }
        }
    }

    {
        const tests::ScopedSection section = runner.Section("an array of storage textures does not cook for wgsl");
        const tests::CookedBundle wgsl = tests::CookForTargets({ assetDirectory / "Pyramid.slang" }, std::nullopt, { "wgsl" });
        runner.CheckEqual(wgsl.Result, CookError::TargetValidationEntryPointParseFailed, "Tint rejects the array");
    }

    std::filesystem::remove_all(controlDirectory);
    return runner.Report();
}
