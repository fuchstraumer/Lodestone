#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ManifestFixtures.hpp"
#include "ShaderLibraryTypes.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "model/AxisInfluence.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using namespace lodestone;

struct CookCase
{
    std::string_view ModuleFile;
    CookError Expected;
    std::string_view Claim;
};

constexpr std::array<CookCase, 4u> k_CookCases
{
    CookCase{ .ModuleFile = "WaveGuarded.slang",
              .Expected = CookError::Success,
              .Claim = "an axis whose on path alone uses a wave operation cooks" },
    CookCase{ .ModuleFile = "GatedWaveSize.slang",
              .Expected = CookError::Success,
              .Claim = "a gated capability axis that changes no capability is not checked" },
    CookCase{ .ModuleFile = "OffPathStillWaves.slang",
              .Expected = CookError::CapabilityAxisInert,
              .Claim = "an off path that still calls a wave operation fails the cook" },
    CookCase{ .ModuleFile = "OffPathNeedsMore.slang",
              .Expected = CookError::CapabilityAxisInverted,
              .Claim = "an off path that needs a capability the on path does not fails the cook" },
};

CookError CookOneModule(const std::filesystem::path& module_path, std::string_view target_name)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target_name);
    options.OutputPath = "CapabilityAxis";

    MemoryOutputSink sink;
    const CookResult<CookStatistics> result = RunCook(options, sink);
    return result ? CookError::Success : result.error();
}

/** One axis, and two variants: off, then on. Each variant reads its own source. */
PermutationAxis MakeAxis(AxisKind kind, std::string active_when)
{
    return PermutationAxis{ "USE_WAVE_OPS",
                            { PermutationValue{ false }, PermutationValue{ true } },
                            kind,
                            EarliestBindingTime::Cook,
                            AxisValueDomain::Boolean,
                            std::move(active_when) };
}

CookedLibrary MakeLibrary(const PermutationSpace& space,
                          std::vector<std::string> off_names,
                          std::vector<std::string> on_names,
                          bool code_facts_read)
{
    CookedModule module;
    module.Name = "Probe";
    module.Space = &space;
    module.SpaceSize = 2u;
    module.EntryPoints.push_back(LibraryEntryPoint{ .Name = "SumCS", .Stage = ShaderStageKind::Compute });
    tests::AddFixtureSource(module, tests::BytesOf("// off"), SourceCodeFacts{ .CapabilityNames = std::move(off_names) });
    tests::AddFixtureSource(module, tests::BytesOf("// on"), SourceCodeFacts{ .CapabilityNames = std::move(on_names) });
    tests::AddDefaultInstanceTables(module);

    for (uint32_t source = 0u; source < 2u; ++source)
    {
        const std::array instances{ tests::ComputeInstance(source, 0u) };
        LibraryVariant variant = tests::MakeFixtureVariant(
            space,
            source,
            PermutationAssignment{ PermutationBinding{ .Axis = &space.Axes()[0], .Value = PermutationValue{ source == 1u } } },
            instances);
        module.VariantKeys.push_back(space.ComputeVariantKey(variant.Canonical));
        module.Variants.push_back(std::move(variant));
    }

    CookedLibrary library;
    library.ModuleNames = { "Probe" };
    library.Profiles.push_back(CookedProfile{ .TargetName = "spirv",
                                              .CodeFormat = ShaderCodeFormat::Spirv,
                                              .CodeFactsRead = code_facts_read });
    library.Environments.emplace_back(std::move(module));
    return library;
}

PermutationSpace MakeSpace(AxisKind kind, std::string active_when)
{
    std::vector<PermutationAxis> axes;
    axes.push_back(MakeAxis(kind, std::move(active_when)));
    return PermutationSpace{ std::move(axes) };
}

/** The checks over a library that no cook produced, so that each rule has a row of its own. */
void TestHandBuilt(tests::TestRunner& runner)
{
    runner.BeginSection("a capability axis must change the capabilities, and off must not need more than on");
    const std::vector<std::string> none;
    const std::vector<std::string> subgroups{ "GroupNonUniform", "GroupNonUniformArithmetic" };
    const std::vector<std::string> float16{ "Float16" };
    const std::vector<std::string> both{ "Float16", "GroupNonUniform" };

    {
        const PermutationSpace space = MakeSpace(AxisKind::Capability, {});
        runner.Check(VerifyCapabilityAxes(MakeLibrary(space, none, subgroups, true)) == CookError::Success,
                     "an axis that turns names on passes");
        runner.Check(VerifyCapabilityAxes(MakeLibrary(space, float16, both, true)) == CookError::Success,
                     "an off path that needs a subset of the on path passes");
        runner.Check(VerifyCapabilityAxes(MakeLibrary(space, subgroups, subgroups, true)) == CookError::CapabilityAxisInert,
                     "an axis whose two values need the same names fails");
        runner.Check(VerifyCapabilityAxes(MakeLibrary(space, subgroups, none, true)) == CookError::CapabilityAxisInverted,
                     "an off path that needs more than the on path fails");
        runner.Check(VerifyCapabilityAxes(MakeLibrary(space, float16, subgroups, true)) == CookError::CapabilityAxisInverted,
                     "an off path that needs a name the on path lacks fails, though the sets differ");
        runner.Check(VerifyCapabilityAxes(MakeLibrary(space, subgroups, subgroups, false)) == CookError::Success,
                     "a profile with no reader is not checked");
    }
    {
        const PermutationSpace gated = MakeSpace(AxisKind::Capability, "OTHER == 1");
        runner.Check(VerifyCapabilityAxes(MakeLibrary(gated, subgroups, subgroups, true)) == CookError::Success,
                     "a gated capability axis is not checked");
    }
    {
        const PermutationSpace technique = MakeSpace(AxisKind::Technique, {});
        runner.Check(VerifyCapabilityAxes(MakeLibrary(technique, subgroups, subgroups, true)) == CookError::Success,
                     "an axis of another kind is not checked");
    }
}

} // namespace

int main(int argc, char** argv)
{
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "CapabilityAxis" };

    TestHandBuilt(runner);

    for (const std::string_view target : { "wgsl", "spirv" })
    {
        for (const CookCase& testCase : k_CookCases)
        {
            runner.BeginSection(std::format("{} ({})", testCase.Claim, target));
            const std::filesystem::path modulePath = assetDirectory / testCase.ModuleFile;
            if (!std::filesystem::exists(modulePath))
            {
                runner.Check(false, "the module named in the row is on disk");
                continue;
            }

            const CookError actual = CookOneModule(modulePath, target);
            if (actual != testCase.Expected)
            {
                std::println(stderr,
                             "[CapabilityAxis] {} ({}): expected {}, and the cook returned {}",
                             testCase.ModuleFile,
                             target,
                             ToString(testCase.Expected),
                             ToString(actual));
            }
            runner.Check(actual == testCase.Expected, testCase.Claim);
        }
    }

    return runner.Report();
}
