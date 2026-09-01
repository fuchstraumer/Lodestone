#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

/** Makes sure that the target-specific infrastructure for reflection, and the scanners for the completed
  * binding data can correctly reject or flag data that isn't valid for that targets binding model 
  * 
  * The `RejectionCase` struct is used to better define and outline what each case is expected to prove
  * or test. 
 *
*/
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

constexpr std::array<RejectionCase, 2u> k_Cases{
    RejectionCase{ .ModuleFile = "PointerMember.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::PointerTypeNotSupported,
                   .Claim = "a pointer member under a bound access model fails the cook" },
    // The control arm. Without it, a check that rejects every module would pass this suite.
    RejectionCase{ .ModuleFile = "EntryPointParams.slang",
                   .TargetName = "wgsl",
                   .Expected = CookError::Success,
                   .Claim = "a module with no pointer still cooks" },
};

/** Cooks one module into memory and returns what the driver said.
 *
 * The sink is a `MemoryOutputSink`, so a row writes no file and two rows cannot collide. A cook that
 * must fail has no artifact worth keeping.
 */
CookError CookOneModule(const std::filesystem::path& module_path, std::string_view target_name)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetName = std::string{ target_name };
    options.OutputPath = "AccessModelReject";

    MemoryOutputSink sink;
    const CookResult<CookStatistics> result = RunCook(options, sink);
    return result ? CookError::Success : result.error();
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

    return runner.Report();
}
