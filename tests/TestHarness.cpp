#include "TestHarness.hpp"
#include "ShaderManifestFormat.hpp"
#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>

namespace lodestone::tests
{

Claim::Claim(const char* text, std::source_location location) noexcept :
    Text{ text },
    Location{ location }
{
}

Claim::Claim(std::string_view text, std::source_location location) noexcept :
    Text{ text },
    Location{ location }
{
}

Claim::Claim(const std::string& text, std::source_location location) noexcept :
    Text{ text },
    Location{ location }
{
}

ScopedSection::ScopedSection(TestRunner& _runner) noexcept :
    runner{ _runner }
{
}

ScopedSection::~ScopedSection()
{
    runner.endScopedSection();
}

ScopedContext::ScopedContext(TestRunner& _runner) noexcept :
    runner{ _runner }
{
}

ScopedContext::~ScopedContext()
{
    runner.endContext();
}

QueryErrorCode ErrorCodeOf(const QueryError& error) noexcept
{
    return error.Code;
}

std::string FormatForTest(const QueryError& error)
{
    return std::format("code {}, axis name \"{}\", detail {}, suggestion \"{}\"", FormatForTest(error.Code),
                       error.AxisName, error.Detail, error.Suggestion);
}

manifest::ErrorCode ErrorCodeOf(const manifest::ErrorState& error) noexcept
{
    return error.Code;
}

std::string FormatForTest(const manifest::ErrorState& error)
{
    return manifest::DescribeShaderManifestError(error);
}

TestRunner::TestRunner(std::string_view suite_name) noexcept :
    suiteName{ suite_name },
    checksRun{ 0 },
    failures{ 0 },
    emptySections{ 0 }
{
}

void TestRunner::BeginSection(std::string_view name)
{
    if (sections.empty())
    {
        sections.push_back(SectionState{ .Name = std::string{ name } });
        return;
    }

    finishSection(sections.back());
    sections.back() = SectionState{ .Name = std::string{ name } };
}

ScopedSection TestRunner::Section(std::string_view name)
{
    sections.push_back(SectionState{ .Name = std::string{ name } });
    return ScopedSection{ *this };
}

ScopedContext TestRunner::Context(std::string text)
{
    contexts.push_back(std::move(text));
    return ScopedContext{ *this };
}

bool TestRunner::Check(bool condition, Claim claim)
{
    if (condition)
    {
        pass();
        return true;
    }

    fail(claim, {});
    return false;
}

int TestRunner::Report()
{
    while (!sections.empty())
    {
        finishSection(sections.back());
        sections.pop_back();
    }

    if (checksRun == 0)
    {
        ++emptySections;
        std::println(stderr, "    FAIL: the suite ran no checks");
    }

    if (Failures() == 0)
    {
        std::println("{} : {} checks passed", suiteName, checksRun);
        return 0;
    }

    std::println("{} : {} of {} checks FAILED, {} sections ran no checks", suiteName, failures, checksRun, emptySections);
    return 1;
}

int32_t TestRunner::Failures() const noexcept
{
    return failures + emptySections;
}

void TestRunner::pass() noexcept
{
    ++checksRun;
    for (SectionState& section : sections)
    {
        ++section.ChecksRun;
    }
}

void TestRunner::fail(const Claim& claim, std::string_view detail)
{
    // a failed check still counts as run, in the suite and in every enclosing section
    pass();
    ++failures;
    printHeading();
    // stderr, so that a failure stays next to the diagnostics the code under test printed
    std::println(stderr, "    FAIL: {}", claim.Text);
    if (!detail.empty())
    {
        std::println(stderr, "      {}", detail);
    }

    for (const std::string& context : contexts)
    {
        std::println(stderr, "      while: {}", context);
    }
    
    std::println(stderr,
                 "      at {}:{} in {}",
                 claim.Location.file_name(),
                 claim.Location.line(),
                 claim.Location.function_name());
}

void TestRunner::printHeading()
{
    std::string path;
    bool anyUnprinted = false;
    for (const SectionState& section : sections)
    {
        anyUnprinted = anyUnprinted || !section.HeadingPrinted;
        path += path.empty() ? section.Name : std::format(" > {}", section.Name);
    }

    if (anyUnprinted && !path.empty())
    {
        std::println(stderr, "  [{}]", path);
        for (SectionState& section : sections)
        {
            section.HeadingPrinted = true;
        }
    }
}

void TestRunner::finishSection(SectionState& section)
{
    if (section.ChecksRun > 0)
    {
        return;
    }

    // a section whose checks all sat behind a guard that failed proves nothing, so it fails
    ++emptySections;
    std::println(stderr, "  [{}]", section.Name);
    std::println(stderr, "    FAIL: this section ran no checks");
}

void TestRunner::endScopedSection()
{
    finishSection(sections.back());
    sections.pop_back();
}

void TestRunner::endContext() noexcept
{
    contexts.pop_back();
}

} // namespace lodestone::tests
