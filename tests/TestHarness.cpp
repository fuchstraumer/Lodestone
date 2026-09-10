#include "TestHarness.hpp"
#include <print>
#include <string_view>

namespace lodestone::tests
{

TestRunner::TestRunner(std::string_view suite_name) noexcept
    : suiteName{ suite_name },
      sectionHeadingPrinted{ false },
      checksRun{ 0u },
      failures{ 0u }
{
}

void TestRunner::BeginSection(std::string_view name) noexcept
{
    currentSection = name;
    sectionHeadingPrinted = false;
}

void TestRunner::Check(bool condition, std::string_view description, std::source_location location) noexcept
{
    ++checksRun;
    if (condition)
    {
        return;
    }
    ++failures;
    reportFailure(description, location);
}

int TestRunner::Report() const noexcept
{
    if (failures == 0u)
    {
        std::println("{} : {} checks passed", suiteName, checksRun);
        return 0;
    }

    std::println("{} : {} of {} checks FAILED", suiteName, failures, checksRun);
    return 1;
}

size_t TestRunner::Failures() const noexcept
{
    return failures;
}

void TestRunner::reportFailure(std::string_view description, const std::source_location& location) noexcept
{
    if (!sectionHeadingPrinted && !currentSection.empty())
    {
        std::println("  [{}]", currentSection);
        sectionHeadingPrinted = true;
    }
    std::println("    FAIL: {}", description);
    // print source location first, to help find where the error occurred
    std::println("    at {}:{} in {}", location.file_name(), location.line(), location.function_name());
}

} // namespace lodestone::tests
