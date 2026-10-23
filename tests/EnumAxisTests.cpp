#include "TestHarness.hpp"

#include "CookComparison.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "VariantKey.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// `EnumAxisTest.slang` declares QualityLevel { Low = 5, High = 1, Medium = 10 }, and the shader takes 1, 8, or 4 samples
// by case. A cook that linked the digit (0, 1, 2) in place of the case value would give Low and Medium the same code,
// and every validator would still pass, so only a comparison of the code can find it.

namespace
{

using namespace lodestone;

constexpr std::array<std::string_view, 3u> k_Cases{ "Low", "High", "Medium" };

/** The code bytes of one variant's first entry point, read by content so that `--no-dedupe` gives the same answer. */
std::vector<std::byte> CodeOf(const manifest::EnvironmentView& environment, uint32_t profile, VariantKey key)
{
    const std::optional<uint32_t> variant = environment.FindVariant(key);
    const manifest::EntryPointInstanceView instance = environment.VariantAt(*variant).EntryPoint(0u);
    // profile 0 is wgsl and profile 1 is spirv, in the order CookBothTargets names them
    const std::span<const std::byte> bytes =
        profile == 0u ? std::as_bytes(std::span{ instance.Source() }) : std::as_bytes(instance.SpirvWords());
    return std::vector<std::byte>{ bytes.begin(), bytes.end() };
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "EnumAxis" };

    runner.BeginSection("the enum axis module cooks for both targets, with both cross-checks and two identical cooks");
    const tests::CookedBundle cooked = tests::CookBothTargets({ assetDirectory / "EnumAxisTest.slang" }, std::nullopt);
    runner.CheckEqual(cooked.Result, CookError::Success, "three cases crossed with a boolean cook");
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(cooked.Bytes);
    if (!runner.CheckOk(bundle, "the manifest opens"))
    {
        return runner.Report();
    }

    for (const uint32_t profile : { 0u, 1u })
    {
        runner.BeginSection(std::format("profile {}: each case links its own value", profile));
        const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(profile, 0u);
        if (!runner.CheckOk(environment, "the environment opens"))
        {
            continue;
        }

        runner.CheckEqual(environment->VariantKeys().size(), 6u, "three cases times two DITHER values");
        const ManifestIndex index{ *environment };
        const QueryResult<std::vector<QueryAxisValue>> values = index.AxisValues("QUALITY");
        if (runner.CheckOk(values, "QUALITY is an axis") && runner.CheckEqual(values->size(), k_Cases.size(), "QUALITY has three cases"))
        {
            for (size_t digit = 0u; digit < k_Cases.size(); ++digit)
            {
                runner.CheckEqual((*values)[digit].Name, k_Cases[digit], "the digit is the declaration order, not the value");
            }
        }

        std::vector<std::vector<std::byte>> codes;
        for (const std::string_view enumCase : k_Cases)
        {
            const std::expected<VariantKey, QueryError> key =
                index.Query().Where("QUALITY", AxisValueDomain::Enum, enumCase).Where("DITHER", false).Single();
            if (runner.CheckOk(key, std::format("QUALITY={} with DITHER=false is one variant", enumCase)))
            {
                codes.push_back(CodeOf(*environment, profile, *key));
            }
        }

        if (runner.CheckEqual(codes.size(), k_Cases.size(), "a variant for each case"))
        {
            runner.Check(codes[0] != codes[2], "Low (5) and Medium (10) take different sample counts, so their code differs");
            runner.Check(codes[0] != codes[1] && codes[1] != codes[2], "High (1) differs from both");
        }
    }

    return runner.Report();
}
