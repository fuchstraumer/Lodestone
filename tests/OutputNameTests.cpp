#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/ArtifactNames.hpp"
#include "emit/OutputSink.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

// Two cooks with different output names can share one folder. A cook removes the artifacts of its own name only.

namespace
{

using namespace lodestone;

constexpr std::array<std::string_view, 4u> k_Suffixes
{
    k_ManifestSuffix,
    k_DedupeReportSuffix,
    k_ResourceReportSuffix,
    k_DepfileSuffix
};

CookError CookWithName(const std::filesystem::path& module_path,
                       const std::filesystem::path& output_directory,
                       std::string_view output_name)
{
    CookerOptions options;
    options.ModulePaths = { module_path };
    options.TargetNames = { "spirv" };
    options.OutputPath = output_directory;
    options.OutputName = std::string{ output_name };
    options.WriteDepfile = true;

    FileOutputSink sink{ output_directory };
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    return cooked.has_value() ? CookError::Success : cooked.error();
}

bool AllExist(const std::filesystem::path& directory, std::string_view output_name)
{
    for (const std::string_view suffix : k_Suffixes)
    {
        if (!std::filesystem::exists(directory / MakeArtifactName(output_name, suffix)))
        {
            return false;
        }
    }

    return true;
}

bool NoneExist(const std::filesystem::path& directory, std::string_view output_name)
{
    for (const std::string_view suffix : k_Suffixes)
    {
        if (std::filesystem::exists(directory / MakeArtifactName(output_name, suffix)))
        {
            return false;
        }
    }

    return true;
}

} // namespace

int main()
{
    tests::TestRunner runner{ "OutputName" };
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::format("LodestoneOutputNameTest_{}", stamp);
    const std::filesystem::path module = std::filesystem::path{ LODESTONE_TEST_ASSET_DIR } / "MulAdd.slang";

    runner.BeginSection("two cooks with different names share one folder");
    runner.CheckEqual(CookWithName(module, directory, "A"), CookError::Success, "the cook named A");
    runner.CheckEqual(CookWithName(module, directory, "B"), CookError::Success, "the cook named B");
    runner.Check(AllExist(directory, "A"), "every artifact of A exists after the cook of B");
    runner.Check(AllExist(directory, "B"), "every artifact of B exists");
    runner.Check(NoneExist(directory, k_DefaultOutputName), "no artifact has the default name");

    runner.BeginSection("a failed cook removes its own artifacts only");
    const std::filesystem::path missingModule = directory / "Missing.slang";
    runner.Check(!IsSuccess(CookWithName(missingModule, directory, "A")), "a cook of a missing module fails");
    runner.Check(NoneExist(directory, "A"), "the artifacts of A are removed");
    runner.Check(AllExist(directory, "B"), "the artifacts of B stay");

    std::error_code removeError;
    std::filesystem::remove_all(directory, removeError);
    return runner.Report();
}
