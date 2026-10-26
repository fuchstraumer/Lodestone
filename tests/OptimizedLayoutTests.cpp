#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestFormat.hpp"
#include "TestHarness.hpp"

#include "CookComparison.hpp"
#include "CookerErrors.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestIndex.hpp"
#include "VariantKey.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

/** One cook of DeadBranch for wgsl and spirv at an optimization and debug level, twice, so that the two cooks must agree. */
tests::CookedBundle CookAtLevel(const std::filesystem::path& module_path, uint32_t level, uint32_t debug_level)
{
    CookerOptions options;
    options.ModulePaths = { module_path };
    options.TargetNames = { "wgsl", "spirv" };
    options.OutputPath = "OptimizedLayout";
    options.OptimizationLevel = level;
    options.DebugLevel = debug_level;
    options.VerifyDeterministic = true;

    MemoryOutputSink sink;
    tests::CookedBundle result;
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    result.Result = cooked.has_value() ? CookError::Success : cooked.error();
    const auto manifest = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
    if (manifest != sink.GetArtifacts().end())
    {
        result.Bytes.resize(manifest->second.size());
        std::memcpy(result.Bytes.data(), manifest->second.data(), result.Bytes.size());
    }
    return result;
}

/** What one entry point instance needs: its binding names, pair count, and workgroup memory. */
struct Instance
{
    std::vector<std::string> Names;
    size_t PairCount{ 0u };
    uint32_t WorkgroupBytes{ 0u };
    // the debug information of SPIR-V needs this extension
    bool NeedsNonSemanticInfo{ false };
    bool Found{ false };

    [[nodiscard]] bool Has(std::string_view name) const
    {
        return std::ranges::find(Names, name) != Names.end();
    }
};

Instance ReadInstance(const manifest::BundleView& bundle, std::string_view target, std::string_view reduction)
{
    Instance instance;
    const int32_t profile = bundle.FindProfile(target);
    if (profile < 0)
    {
        return instance;
    }
    const manifest::ManifestResult<manifest::EnvironmentView> environment =
        bundle.OpenEnvironment(static_cast<uint32_t>(profile), 0u);
    if (!environment.has_value())
    {
        return instance;
    }
    const ManifestIndex index{ *environment };
    const std::expected<VariantKey, QueryError> key =
        index.Query().Where("REDUCTION", AxisValueDomain::Type, reduction).Single();
    const std::optional<uint32_t> variant = key.has_value() ? environment->FindVariant(*key) : std::nullopt;
    const manifest::ManifestResult<uint32_t> entry = bundle.Module(0u).FindEntryPointIndex("ReduceCS");
    if (!variant.has_value() || !entry.has_value())
    {
        return instance;
    }
    const manifest::EntryPointInstanceView view = environment->VariantAt(*variant).EntryPoint(*entry);
    for (const manifest::ResolvedResource resource : view.Layout())
    {
        instance.Names.emplace_back(resource.Name());
    }
    instance.PairCount = view.SamplerTexturePairs().size();
    instance.WorkgroupBytes = view.WorkgroupMemoryBytes();
    for (uint32_t i = 0u; i < environment->CapabilityNames().size(); ++i)
    {
        if (environment->CapabilityName(i) == "SPV_KHR_non_semantic_info" && view.RequiresCapability(i))
        {
            instance.NeedsNonSemanticInfo = true;
        }
    }
    instance.Found = true;
    return instance;
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "OptimizedLayout" };
    const std::filesystem::path module = assetDirectory / "DeadBranch.slang";

    const tests::CookedBundle unoptimized = CookAtLevel(module, 0u, 0u);
    const tests::CookedBundle optimized = CookAtLevel(module, 2u, 0u);
    const tests::CookedBundle debug = CookAtLevel(module, 2u, 3u);

    {
        const tests::ScopedSection section = runner.Section("both cooks pass the cross-profile checks");
        runner.CheckEqual(unoptimized.Result, CookError::Success, "the O0 cook of both targets succeeds");
        runner.CheckEqual(optimized.Result, CookError::Success, "the O2 cook of both targets succeeds");
        runner.CheckEqual(debug.Result, CookError::Success, "the O2 cook with debug information succeeds");
    }

    const manifest::ManifestResult<manifest::BundleView> bundle0 = manifest::BundleView::Open(unoptimized.Bytes);
    const manifest::ManifestResult<manifest::BundleView> bundle2 = manifest::BundleView::Open(optimized.Bytes);
    const manifest::ManifestResult<manifest::BundleView> bundleDebug = manifest::BundleView::Open(debug.Bytes);
    if (!runner.CheckOk(bundle0, "the O0 bundle opens") || !runner.CheckOk(bundle2, "the O2 bundle opens") ||
        !runner.CheckOk(bundleDebug, "the debug bundle opens"))
    {
        return runner.Report();
    }

    {
        // a repeated Slang option keeps its first value, so a fixed option row once hid the debug level
        const tests::ScopedSection section = runner.Section("the debug level reaches the SPIR-V through the optimizer");
        const Instance plain = ReadInstance(*bundle2, "spirv", "Exact");
        const Instance withDebug = ReadInstance(*bundleDebug, "spirv", "Exact");
        if (runner.Check(plain.Found && withDebug.Found, "both instances are found"))
        {
            runner.Check(!plain.NeedsNonSemanticInfo, "without debug information the code needs no SPV_KHR_non_semantic_info");
            runner.Check(withDebug.NeedsNonSemanticInfo, "with --g3 the code needs SPV_KHR_non_semantic_info");
        }
    }

    {
        const tests::ScopedSection section = runner.Section("the profiles record the cook's levels");
        for (const manifest::Profile& profile : bundleDebug->Profiles())
        {
            runner.CheckEqual(profile.OptimizationLevel, uint8_t{ 2u }, "the debug cook records --O2");
            runner.CheckEqual(profile.DebugLevel, uint8_t{ 3u }, "the debug cook records --g3");
        }
    }

    {
        // the relevance describes the base form, so neither the optimizer nor debug information changes it
        const tests::ScopedSection section = runner.Section("the axis relevance of the debug cook equals the O0 cook's");
        for (uint32_t profile = 0u; profile < bundle0->Profiles().size(); ++profile)
        {
            const tests::ScopedContext context = runner.Context(std::string{ bundle0->String(bundle0->Profiles()[profile].TargetNameString) });
            const manifest::ManifestResult<manifest::EnvironmentView> plain = bundle0->OpenEnvironment(profile, 0u);
            const manifest::ManifestResult<manifest::EnvironmentView> withDebug = bundleDebug->OpenEnvironment(profile, 0u);
            if (runner.CheckOk(plain, "the O0 environment opens") && runner.CheckOk(withDebug, "the debug environment opens"))
            {
                runner.Check(std::ranges::equal(plain->AxisRelevance(0u), withDebug->AxisRelevance(0u)),
                             "the relevance masks are equal");
            }
        }
    }

    {
        const tests::ScopedSection section = runner.Section("at O0 every profile and variant keeps all three bindings");
        for (const std::string_view target : { "wgsl", "spirv" })
        {
            for (const std::string_view reduction : { "Exact", "Max" })
            {
                const tests::ScopedContext context = runner.Context(std::string{ target } + " " + std::string{ reduction });
                const Instance instance = ReadInstance(*bundle0, target, reduction);
                if (runner.Check(instance.Found, "the instance is found"))
                {
                    runner.Check(instance.Has("Input") && instance.Has("Linear") && instance.Has("Output"),
                                 "Input, Linear, and Output are in the layout");
                }
            }
        }
    }

    {
        const tests::ScopedSection section = runner.Section("at O0 Exact has one pair");
        for (const std::string_view target : { "wgsl", "spirv" })
        {
            const tests::ScopedContext context = runner.Context(std::string{ target });
            const Instance instance = ReadInstance(*bundle0, target, "Exact");
            runner.CheckEqual(instance.PairCount, 1u, "one pair of Input and Linear");
        }
    }

    {
        const tests::ScopedSection section = runner.Section("at O0 Max needs as much workgroup memory as Exact");
        for (const std::string_view target : { "wgsl", "spirv" })
        {
            const tests::ScopedContext context = runner.Context(std::string{ target });
            const Instance exact = ReadInstance(*bundle0, target, "Exact");
            const Instance max = ReadInstance(*bundle0, target, "Max");
            runner.Check(exact.WorkgroupBytes != 0u, "Exact uses workgroup memory");
            runner.CheckEqual(max.WorkgroupBytes, exact.WorkgroupBytes, "Max reads the same bytes as Exact");
        }
    }

    {
        const tests::ScopedSection section = runner.Section("at O2 spirv Max loses the sampler and the workgroup memory");
        const Instance instance = ReadInstance(*bundle2, "spirv", "Max");
        if (runner.Check(instance.Found, "the instance is found"))
        {
            runner.Check(instance.Has("Input") && instance.Has("Output"), "Input and Output are in the layout");
            runner.Check(!instance.Has("Linear"), "Linear is not in the layout");
            runner.CheckEqual(instance.WorkgroupBytes, 0u, "no workgroup memory");
            runner.CheckEqual(instance.PairCount, 0u, "no pairs");
        }
    }

    {
        const tests::ScopedSection section = runner.Section("at O2 spirv Exact keeps its bindings");
        const Instance instance = ReadInstance(*bundle2, "spirv", "Exact");
        if (runner.Check(instance.Found, "the instance is found"))
        {
            runner.Check(instance.Has("Input") && instance.Has("Linear") && instance.Has("Output"),
                         "Input, Linear, and Output are in the layout");
            runner.CheckEqual(instance.PairCount, 1u, "one pair");
            runner.Check(instance.WorkgroupBytes != 0u, "workgroup memory is not zero");
        }
    }

    {
        const tests::ScopedSection section = runner.Section("at O2 wgsl Max is not optimized");
        const Instance instance = ReadInstance(*bundle2, "wgsl", "Max");
        if (runner.Check(instance.Found, "the instance is found"))
        {
            runner.Check(instance.Has("Linear"), "Linear is still in the layout");
            runner.Check(instance.WorkgroupBytes != 0u, "workgroup memory is not zero");
        }
    }

    return runner.Report();
}
