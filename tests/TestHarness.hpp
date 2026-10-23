#pragma once
#ifndef LODESTONE_TESTS_TEST_HARNESS_HPP
#define LODESTONE_TESTS_TEST_HARNESS_HPP
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestIndex.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Not GTest or Catch2: a counter, comparison helpers that print both values, and a nonzero exit code. Each test
// executable runs by hand or under ctest, and prints only failures plus a one-line summary.
namespace lodestone::tests
{

/** @brief text of a check and the place that wrote it */
struct Claim
{
    Claim(const char* text,
          std::source_location location = std::source_location::current()) noexcept;

    Claim(std::string_view text,
          std::source_location location = std::source_location::current()) noexcept;

    Claim(const std::string& text,
          std::source_location location = std::source_location::current()) noexcept;

    std::string_view Text;
    std::source_location Location;
};

class TestRunner;

/** @brief Restores the enclosing section when it leaves scope. Returned by `TestRunner::Section`. */
class ScopedSection final
{
public:
    explicit ScopedSection(TestRunner& _runner) noexcept;
    ~ScopedSection();
    ScopedSection(const ScopedSection&) = delete;
    ScopedSection& operator=(const ScopedSection&) = delete;
    ScopedSection(ScopedSection&&) = delete;
    ScopedSection& operator=(ScopedSection&&) = delete;

private:
    TestRunner& runner;
};

/** @brief Removes its line of context when it leaves scope. Returned by `TestRunner::Context`. */
class ScopedContext final
{
public:
    explicit ScopedContext(TestRunner& _runner) noexcept;
    ~ScopedContext();
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
    ScopedContext(ScopedContext&&) = delete;
    ScopedContext& operator=(ScopedContext&&) = delete;

private:
    TestRunner& runner;
};

/** @brief Returns the code of a query error, so that `CheckError` compares codes. */
QueryErrorCode ErrorCodeOf(const QueryError& error) noexcept;

/** @brief Returns the code, axis name, detail, and suggestion of a query error. */
std::string FormatForTest(const QueryError& error);

/** @brief Returns the error code of a manifest error, so that `CheckError` compares codes. */
manifest::ErrorCode ErrorCodeOf(const manifest::ErrorState& error) noexcept;

template<typename ErrorType> requires std::is_enum_v<ErrorType>
constexpr ErrorType ErrorCodeOf(ErrorType error) noexcept
{
    return error;
}

/** @brief Returns the code, table, record, and detail of a manifest error. */
std::string FormatForTest(const manifest::ErrorState& error);

/** @brief uses template traits to print the best representation we can of a test value */
template<typename ValueType>
std::string FormatForTest(const ValueType& value)
{
    if constexpr (std::same_as<ValueType, bool>)
    {
        return value ? "true" : "false";
    }
    else if constexpr (std::formattable<ValueType, char>)
    {
        return std::format("{}", value);
    }
    else if constexpr (requires { ToString(value); })
    {
        return std::format("{}", ToString(value));
    }
    else if constexpr (std::is_enum_v<ValueType>)
    {
        return std::format("{} (no ToString)", std::to_underlying(value));
    }
    else
    {
        return "<no formatter>";
    }
}

class TestRunner final
{
public:
    explicit TestRunner(std::string_view suite_name) noexcept;

    /** @brief Starts a new section at the current depth. A section that runs no check is a failure. */
    void BeginSection(std::string_view name);

    [[nodiscard]] ScopedSection Section(std::string_view name);
    /** @brief Adds a line that every failure prints until the returned object leaves scope. */
    [[nodiscard]] ScopedContext Context(std::string text);
    /** @brief Returns the condition, so that the check can guard the checks that depend on it. */
    bool Check(bool condition, Claim claim);
    /** @brief Compares with `==`, or with `std::cmp_equal` for two integers, and prints both values on a failure. */
    template<typename ActualType, typename ExpectedType>
    bool CheckEqual(const ActualType& actual, const ExpectedType& expected, Claim claim);
    /** @brief Passes when the result holds a value, and prints the error when it does not. */
    template<typename ValueType, typename ErrorType>
    bool CheckOk(const std::expected<ValueType, ErrorType>& result, Claim claim);
    /** @brief Passes when the result failed with `expected_error`, and prints what came back when it did not. */
    template<typename ValueType, typename ErrorType, typename CodeType>
    bool CheckError(const std::expected<ValueType, ErrorType>& result, CodeType expected_error, Claim claim);
    /** @brief Prints the summary, and returns what main() returns. */
    [[nodiscard]] int Report();
    /** @brief Returns the failed checks plus the sections that ran no check. */
    [[nodiscard]] int32_t Failures() const noexcept;

private:
    friend class ScopedSection;
    friend class ScopedContext;

    struct SectionState
    {
        std::string Name;
        int32_t ChecksRun{ 0 };
        bool HeadingPrinted{ false };
    };

    void pass() noexcept;
    void fail(const Claim& claim, std::string_view detail);
    void printHeading();
    void finishSection(SectionState& section);
    void endScopedSection();
    void endContext() noexcept;

    std::string_view suiteName;
    std::vector<SectionState> sections;
    std::vector<std::string> contexts;
    int32_t checksRun;
    int32_t failures;
    int32_t emptySections;
};

template<typename ActualType, typename ExpectedType>
bool TestRunner::CheckEqual(const ActualType& actual, const ExpectedType& expected, Claim claim)
{
    bool equal = false;
    if constexpr (std::integral<ActualType> &&
                  std::integral<ExpectedType> &&
                 !std::same_as<ActualType, bool> &&
                 !std::same_as<ExpectedType, bool>)
    {
        equal = std::cmp_equal(actual, expected);
    }
    else
    {
        equal = static_cast<bool>(actual == expected);
    }

    if (equal)
    {
        pass();
        return true;
    }

    fail(claim, std::format("expected: {}\n      actual:   {}", FormatForTest(expected), FormatForTest(actual)));
    return false;
}

template<typename ValueType, typename ErrorType>
bool TestRunner::CheckOk(const std::expected<ValueType, ErrorType>& result, Claim claim)
{
    if (result.has_value())
    {
        pass();
        return true;
    }

    fail(claim, std::format("expected: a value\n      actual:   error {}", FormatForTest(result.error())));
    return false;
}

template<typename ValueType, typename ErrorType, typename CodeType>
bool TestRunner::CheckError(const std::expected<ValueType, ErrorType>& result, CodeType expected_error, Claim claim)
{
    if (!result.has_value() && ErrorCodeOf(result.error()) == expected_error)
    {
        pass();
        return true;
    }

    const std::string actual = result.has_value() ? std::string{ "a value" }
                                                  : std::format("error {}", FormatForTest(result.error()));
    fail(claim, std::format("expected: error {}\n      actual:   {}", FormatForTest(expected_error), actual));
    return false;
}

/** @brief The bytes of a text, in the form the cooker stores target code. */
inline std::vector<std::byte> BytesOf(std::string_view text)
{
    const std::span<const std::byte> bytes = std::as_bytes(std::span{ text });
    return std::vector<std::byte>{ bytes.begin(), bytes.end() };
}

} // namespace lodestone::tests

#endif // !LODESTONE_TESTS_TEST_HARNESS_HPP
