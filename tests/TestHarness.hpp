#pragma once
#ifndef LODESTONE_TESTS_TEST_HARNESS_HPP
#define LODESTONE_TESTS_TEST_HARNESS_HPP
#include <cstddef>
#include <string_view>
#include <source_location>

// Not GTest or Catch2: a counter, a comparison helper, and a nonzero exit code. Each test executable
// runs by hand or under ctest, and prints only failures plus a one-line summary.
namespace lodestone::tests
{

class TestRunner final
{
public:
    explicit TestRunner(std::string_view suite_name) noexcept;

    /** @brief Heading for subsequent checks, printed only if one of them fails */
    void BeginSection(std::string_view name) noexcept;

    void Check(bool condition, std::string_view description, std::source_location location = std::source_location::current()) noexcept;

    /** @brief Prints the summary; returns what main() should hand back */
    [[nodiscard]] int Report() const noexcept;

    [[nodiscard]] size_t Failures() const noexcept;

private:
    void reportFailure(std::string_view description, const std::source_location& location) noexcept;

    std::string_view suiteName;
    std::string_view currentSection;
    bool sectionHeadingPrinted;
    size_t checksRun;
    size_t failures;
};

} // namespace lodestone::tests

#endif // !LODESTONE_TESTS_TEST_HARNESS_HPP
