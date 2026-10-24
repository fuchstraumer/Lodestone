#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "driver/CookerOptions.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The command line is an ingestion surface: everything after ParseCommandLine trusts what it returns. Each rule here
// is one the driver relies on, and no Slang is needed.

namespace
{

using namespace lodestone;

/** A folder of its own for each run, so that two runs of this test cannot collide. */
std::filesystem::path MakeScratchDirectory()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::format("LodestoneCookerOptionsTest_{}", stamp);
    std::filesystem::create_directories(directory);
    return directory;
}

CookResult<CookerOptions> Parse(std::initializer_list<std::string_view> arguments)
{
    const std::vector<std::string_view> list{ arguments };
    return ParseCommandLine(list);
}

} // namespace

int main()
{
    tests::TestRunner runner{ "CookerOptions" };
    const std::filesystem::path scratch = MakeScratchDirectory();
    const std::string output = (scratch / "out").string();
    const std::string includeA = (scratch / "includeA").string();
    const std::string includeB = (scratch / "includeB").string();
    std::filesystem::create_directories(includeA);
    std::filesystem::create_directories(includeB);
    const std::string policy = (std::filesystem::path{ LODESTONE_TEST_ASSET_DIR } / "KitchenSink/KitchenSink.toml").string();

    runner.BeginSection("a minimal command line parses, and its defaults are the ones the driver expects");
    const CookResult<CookerOptions> minimal = Parse({ "-o", output, "--target=wgsl", "a.slang" });
    if (runner.CheckOk(minimal, "-o, one target, and one module"))
    {
        runner.CheckEqual(minimal->OutputPath, std::filesystem::path{ output }, "the output path reads back");
        runner.Check(!std::filesystem::exists(output), "parsing does not create the output folder");
        runner.CheckEqual(minimal->TargetNames, std::vector<std::string>{ "wgsl" }, "the target reads back");
        runner.CheckEqual(minimal->ModulePaths.size(), 1u, "one module");
        runner.CheckEqual(minimal->OptimizationLevel, 0u, "the optimization level defaults to 0");
        runner.Check(minimal->DedupeEnabled && minimal->ValidateAgainstEmittedText && !minimal->VerifyDeterministic,
                     "dedup and validation are on, and the determinism check is off");
        runner.Check(!minimal->PolicyFile.has_value(), "no policy file unless one is named");
        runner.Check(!minimal->ModuleCacheDirectory.empty(), "the module cache folder has a default");
    }

    runner.BeginSection("a command line that lacks a required part fails with its own error");
    runner.CheckError(Parse({ "--target=wgsl", "a.slang" }), CookError::NoOutputSpecified, "no -o");
    runner.CheckError(Parse({ "-o", output, "--target=wgsl" }), CookError::NoModulesSpecified, "no module");
    runner.CheckError(Parse({ "-o", output, "a.slang" }), CookError::NoTargetSpecified, "no target");
    runner.CheckError(Parse({ "--target=wgsl", "a.slang", "-o" }), CookError::MalformedArgument, "-o with no value");
    runner.CheckError(Parse({ "-o", output, "--target=wgsl", "--frobnicate", "a.slang" }),
                      CookError::UnknownArgument,
                      "an unknown flag is an error, not a module path");

    runner.BeginSection("a target is checked at the command line");
    runner.CheckError(Parse({ "-o", output, "--target=hlsl", "a.slang" }),
                      CookError::UnknownTargetProfile,
                      "a target the build does not know");
    runner.CheckError(Parse({ "-o", output, "--target=", "a.slang" }), CookError::UnknownTargetProfile, "an empty target");
    const CookResult<CookerOptions> twoTargets = Parse({ "-o", output, "--target=spirv", "--target=wgsl", "a.slang" });
    if (runner.CheckOk(twoTargets, "two targets"))
    {
        runner.CheckEqual(twoTargets->TargetNames, std::vector<std::string>{ "spirv", "wgsl" }, "targets keep their order");
    }

    runner.BeginSection("the optimization level is 0 to 3, and nothing else");
    for (const uint32_t level : { 0u, 1u, 2u, 3u })
    {
        const std::string flag = std::format("--O{}", level);
        const CookResult<CookerOptions> parsed = Parse({ "-o", output, "--target=wgsl", flag, "a.slang" });
        if (runner.CheckOk(parsed, flag))
        {
            runner.CheckEqual(parsed->OptimizationLevel, level, flag);
        }
    }
    for (const std::string_view flag : { "--O4", "--O", "--O-1", "--O2x" })
    {
        runner.CheckError(Parse({ "-o", output, "--target=wgsl", flag, "a.slang" }),
                          CookError::MalformedArgument,
                          std::format("{} is rejected", flag));
    }

    runner.BeginSection("include folders keep their order, and must exist");
    const CookResult<CookerOptions> includes =
        Parse({ "-o", output, "--target=wgsl", "--include-dir", includeA, "-I", includeB, "a.slang" });
    if (runner.CheckOk(includes, "two include folders"))
    {
        runner.CheckEqual(includes->IncludeDirectories,
                          std::vector<std::filesystem::path>{ includeA, includeB },
                          "the search order is the order given");
    }
    runner.CheckError(Parse({ "-o", output, "--target=wgsl", "-I", (scratch / "missing").string(), "a.slang" }),
                      CookError::DirectoryDoesNotExist,
                      "an include folder that does not exist");

    runner.BeginSection("a policy file must exist");
    const CookResult<CookerOptions> withPolicy = Parse({ "-o", output, "--target=wgsl", "--policy-file", policy, "a.slang" });
    if (runner.CheckOk(withPolicy, "an existing policy file"))
    {
        runner.CheckEqual(withPolicy->PolicyFile, std::optional<std::filesystem::path>{ policy }, "the policy path reads back");
    }
    runner.CheckError(Parse({ "-o", output, "--target=wgsl", "--policy-file", (scratch / "missing.toml").string(), "a.slang" }),
                      CookError::FileNotFound,
                      "a mistyped policy path fails, and does not cook every variant with no budget");
    runner.CheckError(Parse({ "-o", output, "--target=wgsl", "a.slang", "--policy-file" }),
                      CookError::MalformedArgument,
                      "--policy-file with no value");

    runner.BeginSection("each switch sets its option");
    const CookResult<CookerOptions> switches = Parse({ "-o", output, "--target=wgsl", "--no-dedupe", "--verify-deterministic",
                                                       "--no-validate", "--quiet", "--single-threaded", "--dump-sources",
                                                       "--depfile", "a.slang" });
    if (runner.CheckOk(switches, "every switch at once"))
    {
        runner.CheckEqual(switches->DedupeEnabled, false, "--no-dedupe");
        runner.CheckEqual(switches->VerifyDeterministic, true, "--verify-deterministic");
        runner.CheckEqual(switches->ValidateAgainstEmittedText, false, "--no-validate");
        runner.CheckEqual(switches->ReportReflection, false, "--quiet");
        runner.CheckEqual(switches->MultithreadEntryPointCodegen, false, "--single-threaded");
        runner.CheckEqual(switches->DumpSources, true, "--dump-sources");
        runner.CheckEqual(switches->WriteDepfile, true, "--depfile");
    }

    runner.BeginSection("--dump-stage adds to the mask, and rejects an unknown stage");
    const CookResult<CookerOptions> cookedDump = Parse({ "-o", output, "--target=wgsl", "--dump-stage=cooked", "a.slang" });
    if (runner.CheckOk(cookedDump, "--dump-stage=cooked"))
    {
        runner.CheckEqual(cookedDump->DumpStageMask, StageDumpBit(StageDumpKind::Cooked), "only the cooked dump is requested");
    }
    const CookResult<CookerOptions> twoDumps =
        Parse({ "-o", output, "--target=wgsl", "--dump-stage=space", "--dump-stage=variants", "a.slang" });
    if (runner.CheckOk(twoDumps, "the flag repeats"))
    {
        runner.CheckEqual(twoDumps->DumpStageMask,
                          StageDumpBit(StageDumpKind::Space) | StageDumpBit(StageDumpKind::Variants),
                          "a repeated flag adds to the mask instead of replacing it");
    }
    const CookResult<CookerOptions> allDumps = Parse({ "-o", output, "--target=wgsl", "--dump-stage=all", "a.slang" });
    if (runner.CheckOk(allDumps, "--dump-stage=all"))
    {
        runner.CheckEqual(allDumps->DumpStageMask, AllStageDumpBits(), "--dump-stage=all sets every bit");
    }
    runner.CheckError(Parse({ "-o", output, "--target=wgsl", "--dump-stage=nonsense", "a.slang" }),
                      CookError::MalformedArgument,
                      "an unknown stage name");

    std::filesystem::remove_all(scratch);
    return runner.Report();
}
