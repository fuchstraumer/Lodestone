#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestFormat.hpp"
#include "TestHarness.hpp"

#include "CookComparison.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "VariantKey.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "target/StorageFormatTable.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
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

constexpr std::string_view k_CapabilityWithoutFormat = "StorageImageWriteWithoutFormat";

/** The subject with the literal type of one format. The store narrows the float4 to the texel of the format by hand. */
std::string MakeControlSource(const StorageFormatInfo& format)
{
    const std::string_view narrow = format.SlangType == "R32F" ? ".x" : format.SlangType == "RG32F" ? ".xy" : "";
    return std::format(R"(module MipControl_{0};

struct MipLevel
{{
    Texture2D<float4> previous;
    WTexture2D<{1}> next;
}};

ParameterBlock<MipLevel> level;

[shader("compute")]
[numthreads(8, 8, 1)]
void DownsampleCS(uint3 id : SV_DispatchThreadID)
{{
    const int2 texel = int2(id.xy) * 2;
    const float4 sum = level.previous.Load(int3(texel, 0)) + level.previous.Load(int3(texel + int2(1, 0), 0)) +
                       level.previous.Load(int3(texel + int2(0, 1), 0)) + level.previous.Load(int3(texel + int2(1, 1), 0));
    level.next.Store(id.xy, (sum * 0.25f){2});
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

bool NamesCapability(const manifest::EnvironmentView& environment, std::string_view capability)
{
    for (uint32_t i = 0u; i < environment.CapabilityNames().size(); ++i)
    {
        if (environment.CapabilityName(i) == capability)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    const std::filesystem::path controlDirectory = std::filesystem::temp_directory_path() / "LodestoneStorageFormatAxisTest";
    tests::TestRunner runner{ "StorageFormatAxis" };
    const std::span<const StorageFormatInfo> table = GetStorageFormats();

    runner.BeginSection("the mip kernel with a format axis and one control for each format cook for both targets");
    std::filesystem::create_directories(controlDirectory);
    std::vector<std::filesystem::path> controlPaths;
    for (const StorageFormatInfo& format : table)
    {
        controlPaths.push_back(controlDirectory / std::format("MipControl_{}.slang", format.EnumCase));
        std::ofstream{ controlPaths.back() } << MakeControlSource(format);
    }

    const tests::CookedBundle subject = tests::CookBothTargets({ assetDirectory / "MipDownsample.slang" }, std::nullopt);
    const tests::CookedBundle controls = tests::CookBothTargets(controlPaths, std::nullopt);
    runner.Check(subject.Cooked, "the subject cooks, with both cross-checks and two identical cooks");
    runner.Check(controls.Cooked, "the six controls cook");

    const manifest::ManifestResult<manifest::BundleView> subjectBundle = manifest::BundleView::Open(subject.Bytes);
    const manifest::ManifestResult<manifest::BundleView> controlBundle = manifest::BundleView::Open(controls.Bytes);
    if (subjectBundle.has_value() && controlBundle.has_value())
    {
        for (const uint32_t profile : { 0u, 1u })
        {
            runner.BeginSection(std::format("profile {}: each variant binds what its control binds, with a known format", profile));
            const manifest::ManifestResult<manifest::EnvironmentView> environment = subjectBundle->OpenEnvironment(profile, 0u);
            if (!environment.has_value())
            {
                runner.Check(false, "the subject environment opens");
                continue;
            }

            for (const StorageFormatInfo& format : table)
            {
                const int32_t controlModule = controlBundle->FindModule(std::format("MipControl_{}", format.EnumCase));
                if (!runner.Check(controlModule >= 0, std::format("{}: the control module is in the bundle", format.EnumCase)))
                {
                    continue;
                }
                const manifest::ManifestResult<manifest::EnvironmentView> control =
                    controlBundle->OpenEnvironment(profile, static_cast<uint32_t>(controlModule));
                const std::optional<uint32_t> variant = FindVariantOf(*environment, format.EnumCase);
                if (!control.has_value() || !variant.has_value())
                {
                    runner.Check(false, std::format("{}: the variant and its control are found", format.EnumCase));
                    continue;
                }

                const std::vector<tests::ResourceKey> keys = tests::ResourceKeysOf(*environment, *variant, 0u);
                runner.Check(!keys.empty() && keys == tests::ResourceKeysOf(*control, 0u, 0u),
                             std::format("{}: the same resources at the same places, with the same format", format.EnumCase));
            }

            // the facts must have been read, or the next check proves nothing
            runner.Check(environment->HasCodeFacts() && !NamesCapability(*environment, k_CapabilityWithoutFormat),
                         "no variant writes a storage image without a format");
        }
    }

    runner.BeginSection("the capability check can see a storage image written with no format");
    {
        const std::filesystem::path unformattedPath = controlDirectory / "MipUnformatted.slang";
        // Slang infers rgba32f from the element type unless the format is stated as unknown
        std::ofstream{ unformattedPath } << R"(module MipUnformatted;

[format("unknown")]
RWTexture2D<float4> next;

[shader("compute")]
[numthreads(8, 8, 1)]
void DownsampleCS(uint3 id : SV_DispatchThreadID)
{
    next[id.xy] = float4(0.25f);
}
)";
        CookerOptions options;
        options.ModulePaths.push_back(unformattedPath);
        options.TargetNames.emplace_back("spirv");
        options.OutputPath = "StorageFormatAxisUnformatted";
        MemoryOutputSink sink;
        const bool cooked = RunCook(options, sink).has_value();
        const auto manifest = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
        std::vector<std::byte> bytes;
        if (manifest != sink.GetArtifacts().end())
        {
            bytes.resize(manifest->second.size());
            std::memcpy(bytes.data(), manifest->second.data(), bytes.size());
        }
        const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
        const manifest::ManifestResult<manifest::EnvironmentView> environment =
            bundle.has_value() ? bundle->OpenEnvironment(0u, 0u) : std::unexpected(bundle.error());
        runner.Check(cooked && environment.has_value() && NamesCapability(*environment, k_CapabilityWithoutFormat),
                     "a write to a storage image of unknown format needs StorageImageWriteWithoutFormat on spirv");
    }

    runner.BeginSection("a policy that lists two formats cooks those two");
    const tests::CookedBundle subset =
        tests::CookBothTargets({ assetDirectory / "MipDownsample.slang" }, assetDirectory / "MipDownsample.toml");
    runner.Check(subset.Cooked, "the subset cooks");
    const manifest::ManifestResult<manifest::BundleView> subsetBundle = manifest::BundleView::Open(subset.Bytes);
    for (const uint32_t profile : { 0u, 1u })
    {
        const manifest::ManifestResult<manifest::EnvironmentView> environment =
            subsetBundle.has_value() ? subsetBundle->OpenEnvironment(profile, 0u) : std::unexpected(subsetBundle.error());
        if (!environment.has_value())
        {
            runner.Check(false, "the subset environment opens");
            continue;
        }

        runner.Check(environment->VariantKeys().size() == 2u && FindVariantOf(*environment, "Rgba16Float").has_value() &&
                         FindVariantOf(*environment, "R32Float").has_value() &&
                         !FindVariantOf(*environment, "Rgba8Unorm").has_value(),
                     std::format("profile {}: Rgba16Float and R32Float, and no other format", profile));
    }

    std::filesystem::remove_all(controlDirectory);
    return runner.Report();
}
