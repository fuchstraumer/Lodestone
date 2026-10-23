#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <string_view>

namespace
{

using namespace lodestone;

struct RejectionCase
{
    /** File name inside the asset directory the command line names. */
    std::string_view ModuleFile;
    /** Which `TargetProfile` the cook asks for. */
    std::string_view TargetName;
    /** What `RunCook` must return. `Success` means the module must cook. */
    CookError Expected;
    /** What this row proves, for the failure message. */
    std::string_view Claim;
};

constexpr std::array<RejectionCase, 5u> k_Cases
{
    RejectionCase{ .ModuleFile = "PointerMember.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::PointerTypeNotSupported,
                   .Claim = "a pointer member under a bound access model fails the cook" },
    // Slang's metadata reports the unused resource as used (shader-slang/slang#13789), and the workaround removes
    // it from the visibility. Without the workaround, this row fails with ReflectionMismatch.
    RejectionCase{ .ModuleFile = "EntryPointUnusedResource.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "an unread resource beside entry point constants cooks (shader-slang/slang#13789)" },
    RejectionCase{ .ModuleFile = "SpecConstantWorkgroupSize.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::CodeGenerationFailed,
                   .Claim = "a codegen error fails the cook, even when Slang returns a success code" },
    // The control arm. Without it, a check that rejects every module would pass this suite.
    RejectionCase{ .ModuleFile = "EntryPointParams.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "a module with no pointer still cooks" },
    // the rule that a shim extern (phase F step F2) is exempt from: an interface axis is a local, so it holds no resource
    RejectionCase{ .ModuleFile = "InterfaceAxisResourceMember.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::SlangInterfaceHasResourceMember,
                   .Claim = "an implementation of an interface axis that holds a resource fails the cook" },
};

/** Cooks one module into a `MemoryOutputSink`, so that rows write no files and cannot collide. */
CookError CookOneModule(const std::filesystem::path& module_path, std::string_view target_name)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target_name);
    options.OutputPath = "AccessModelReject";

    MemoryOutputSink sink;
    const CookResult<CookStatistics> result = RunCook(options, sink);
    return result ? CookError::Success : result.error();
}

/** Cooks one module into a folder on disk, through the sink the console uses. */
CookError CookOneModuleToDisk(const std::filesystem::path& module_path,
                              std::string_view target_name,
                              const std::filesystem::path& output_directory)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target_name);
    options.OutputPath = output_directory;

    FileOutputSink sink{ output_directory };
    const CookResult<CookStatistics> result = RunCook(options, sink);
    return result ? CookError::Success : result.error();
}

/** True when a temporary file of an atomic write is left in the folder. */
bool HasTemporaryFile(const std::filesystem::path& directory)
{
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator{ directory })
    {
        if (entry.path().extension() == ".tmp")
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat doesn't give any arguments to each .exe, but we do have CMake set 
    // `LODESTONE_TEST_ASSET_DIR` so that we can still find the directory for our test shaders
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    lodestone::tests::TestRunner runner{ "AccessModelReject" };

    for (const RejectionCase& testCase : k_Cases)
    {
        runner.BeginSection(testCase.Claim);

        const std::filesystem::path modulePath = assetDirectory / testCase.ModuleFile;
        if (!std::filesystem::exists(modulePath))
        {
            runner.Check(false, "the module named in the row is on disk");
            continue;
        }

        const CookError actual = CookOneModule(modulePath, testCase.TargetName);
        const bool matched = actual == testCase.Expected;
        if (!matched)
        {
            std::println(stderr,
                         "[AccessModelReject] {}: expected {}, and the cook returned {}",
                         testCase.ModuleFile,
                         ToString(testCase.Expected),
                         ToString(actual));
        }

        runner.Check(matched, testCase.Claim);
    }

    runner.BeginSection("a failed cook leaves no manifest of an earlier cook");
    // a reader that finds an old manifest takes it for the output of the cook that failed
    const std::filesystem::path outputDirectory = std::filesystem::temp_directory_path() / "LodestoneStaleManifestTest";
    std::filesystem::remove_all(outputDirectory);
    std::filesystem::create_directories(outputDirectory);
    const std::filesystem::path manifestPath = outputDirectory / k_ManifestFileName;
    {
        std::ofstream stale{ manifestPath, std::ios::binary };
        stale << "a manifest of an earlier cook";
    }
    const CookError failed = CookOneModuleToDisk(assetDirectory / k_Cases[0].ModuleFile, k_Cases[0].TargetName, outputDirectory);
    runner.Check(failed == k_Cases[0].Expected, "the cook fails with the error of its row");
    runner.Check(!std::filesystem::exists(manifestPath), "the failed cook removed the old manifest");

    const CookError cooked = CookOneModuleToDisk(assetDirectory / k_Cases[3].ModuleFile, k_Cases[3].TargetName, outputDirectory);
    runner.Check(cooked == CookError::Success && std::filesystem::exists(manifestPath),
                 "a cook that succeeds writes the manifest again");
    runner.Check(!HasTemporaryFile(outputDirectory), "each atomic write renamed its temporary file into place");
    std::filesystem::remove_all(outputDirectory);

    return runner.Report();
}
