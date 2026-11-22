#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestFormat.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/ArtifactNames.hpp"
#include "emit/OutputSink.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// `--fp-mode` must reach Slang for the base form and the delivered form, and the manifest must record it. Slang decorates
// float arithmetic with NoContraction in precise mode, so the SPIR-V shows whether the mode arrived.

namespace
{

using namespace lodestone;

constexpr uint32_t k_OpDecorate = 71u;
constexpr uint32_t k_DecorationNoContraction = 42u;
constexpr size_t k_SpirvHeaderWords = 5u;

struct FloatModeCook
{
    CookError Result{ CookError::Invalid };
    std::vector<std::byte> Manifest;
    uint32_t NoContractionCount{ 0u };
    bool FoundSpirv{ false };
};

uint32_t CountNoContraction(std::span<const uint32_t> words)
{
    uint32_t count = 0u;
    size_t index = k_SpirvHeaderWords;
    while (index < words.size())
    {
        const uint32_t instruction = words[index];
        const uint32_t wordCount = instruction >> 16u;
        const uint32_t opCode = instruction & 0xFFFFu;
        const bool isNoContraction =
            opCode == k_OpDecorate && wordCount >= 3u && words[index + 2u] == k_DecorationNoContraction;
        if (isNoContraction)
        {
            ++count;
        }

        index += wordCount;
    }

    return count;
}

FloatModeCook Cook(const std::filesystem::path& module_path,
                   std::string target_name,
                   manifest::FloatingPointMode mode,
                   uint32_t optimization_level)
{
    CookerOptions options;
    options.ModulePaths = { module_path };
    options.TargetNames = { std::move(target_name) };
    options.OutputPath = "FloatingPointMode";
    options.FloatingPointMode = mode;
    options.OptimizationLevel = optimization_level;
    options.DumpSources = true;

    MemoryOutputSink sink;
    const CookResult<CookStatistics> cooked = RunCook(options, sink);

    FloatModeCook result;
    result.Result = cooked.has_value() ? CookError::Success : cooked.error();
    for (const auto& [name, content] : sink.GetArtifacts())
    {
        if (name == MakeArtifactName(k_DefaultOutputName, k_ManifestSuffix))
        {
            result.Manifest.resize(content.size());
            std::memcpy(result.Manifest.data(), content.data(), content.size());
        }
        else if (name.ends_with(".spv"))
        {
            std::vector<uint32_t> words(content.size() / sizeof(uint32_t));
            std::memcpy(words.data(), content.data(), words.size() * sizeof(uint32_t));
            result.NoContractionCount += CountNoContraction(words);
            result.FoundSpirv = true;
        }
    }

    return result;
}

} // namespace

int main()
{
    tests::TestRunner runner{ "FloatingPointMode" };
    const std::filesystem::path module = std::filesystem::path{ LODESTONE_TEST_ASSET_DIR } / "MulAdd.slang";

    for (const uint32_t level : { 0u, 2u })
    {
        const tests::ScopedContext context = runner.Context(std::format("--O{}", level));
        const FloatModeCook fast = Cook(module, "spirv", manifest::FloatingPointMode::Fast, level);
        const FloatModeCook precise = Cook(module, "spirv", manifest::FloatingPointMode::Precise, level);
        const FloatModeCook standard = Cook(module, "spirv", manifest::FloatingPointMode::Default, level);

        runner.CheckEqual(fast.Result, CookError::Success, "the fast cook succeeds");
        runner.CheckEqual(precise.Result, CookError::Success, "the precise cook succeeds");
        runner.CheckEqual(standard.Result, CookError::Success, "the default cook succeeds");
        if (!runner.Check(fast.FoundSpirv && precise.FoundSpirv && standard.FoundSpirv, "each cook dumps its SPIR-V"))
        {
            continue;
        }

        runner.CheckEqual(fast.NoContractionCount, 0u, "fast mode decorates no arithmetic NoContraction");
        runner.Check(precise.NoContractionCount > 0u, "precise mode decorates the multiply and the add NoContraction");

        const tests::ScopedSection section = runner.Section("the manifest records the mode");
        const FloatModeCook* const cooks[]{ &fast, &standard, &precise };
        const manifest::FloatingPointMode modes[]{ manifest::FloatingPointMode::Fast,
                                                   manifest::FloatingPointMode::Default,
                                                   manifest::FloatingPointMode::Precise };
        for (size_t index = 0u; index < 3u; ++index)
        {
            const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(cooks[index]->Manifest);
            if (runner.CheckOk(bundle, "the bundle opens"))
            {
                runner.CheckEqual(bundle->Profiles()[0].FloatingPointMode, modes[index], "the profile records the mode");
            }
        }
    }

    return runner.Report();
}
