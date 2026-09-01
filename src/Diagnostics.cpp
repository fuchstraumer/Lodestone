#include "compile/Diagnostics.hpp"
#include "CookerErrors.hpp"

#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    /** `file(line,column): ` when the record names a place, and nothing when it does not. The shape
     * follows the compiler convention that an editor already knows how to click on. */
    std::string FormatLocation(const Diagnostic& diagnostic)
    {
        if (diagnostic.File.empty() || !HasLocation(diagnostic.Range))
        {
            return {};
        }

        return std::format(
            "{}({},{}): ", diagnostic.File, diagnostic.Range.StartLine, diagnostic.Range.StartColumn);
    }

    std::string FormatCode(const Diagnostic& diagnostic)
    {
        if (diagnostic.Code.empty())
        {
            return {};
        }

        return std::format(" {}", diagnostic.Code);
    }

} // namespace

std::string_view ToString(DiagnosticSeverity severity) noexcept
{
    switch (severity)
    {
    case DiagnosticSeverity::Note:
        return "note";
    case DiagnosticSeverity::Warning:
        return "warning";
    case DiagnosticSeverity::Error:
        return "error";
    case DiagnosticSeverity::Fatal:
        return "fatal error";
    case DiagnosticSeverity::Invalid:
        return "invalid";
    }

    return "invalid";
}

bool IsFailure(DiagnosticSeverity severity) noexcept
{
    return severity > DiagnosticSeverity::Warning;
}

bool HasLocation(const SourceRange& range) noexcept
{
    return range.StartLine != 0 || range.StartColumn != 0;
}

void StderrDiagnosticSink::Report(const Diagnostic& diagnostic)
{
    if (IsFailure(diagnostic.Severity))
    {
        ++failureCount;
    }

    std::println(stderr,
                 "[shader_cooker] {}{}{}: {} [{}]",
                 FormatLocation(diagnostic),
                 ToString(diagnostic.Severity),
                 FormatCode(diagnostic),
                 diagnostic.Message,
                 diagnostic.Context);

    for (const DiagnosticNote& note : diagnostic.Related)
    {
        if (note.File.empty() || !HasLocation(note.Range))
        {
            std::println(stderr, "[shader_cooker]   note: {}", note.Message);
            continue;
        }

        std::println(stderr,
                     "[shader_cooker]   {}({},{}): note: {}",
                     note.File,
                     note.Range.StartLine,
                     note.Range.StartColumn,
                     note.Message);
    }
}

// pointless lint warning: we're just trying to match interfaces. moving helps the recording sink
//NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
void StderrDiagnosticSink::Report(Diagnostic&& diagnostic)
{
    Report(diagnostic);
}

int32_t StderrDiagnosticSink::FailureCount() const noexcept
{
    return failureCount;
}

RecordingDiagnosticSink::RecordingDiagnosticSink(RecordingDiagnosticSink&& other) noexcept
    : records(std::move(other.records))
{
}

RecordingDiagnosticSink& RecordingDiagnosticSink::operator=(RecordingDiagnosticSink&& other) noexcept
{
    records = std::move(other.records);
    return *this;
}

void RecordingDiagnosticSink::Report(const Diagnostic& diagnostic)
{
    records.push_back(diagnostic);
}

void RecordingDiagnosticSink::Report(Diagnostic&& diagnostic)
{
    records.emplace_back(std::move(diagnostic));
}

const std::vector<Diagnostic>& RecordingDiagnosticSink::Records() const noexcept
{
    return records;
}

std::vector<Diagnostic>&& RecordingDiagnosticSink::Records() noexcept
{
    return std::move(records);
}

void RecordingDiagnosticSink::Reset() noexcept
{
    records.clear();
}

void ReportToSink(DiagnosticSink& sink,
                  DiagnosticSeverity severity,
                  std::string message,
                  std::source_location location = std::source_location::current())
{
    Diagnostic diag;
    diag.Severity = severity;
    diag.Message = std::move(message);
    diag.Context = std::format(
        "{} {}:L{}", location.file_name(), location.function_name(), location.line());
    sink.Report(std::move(diag));
}

CookError ReportError(DiagnosticSink& sink,
                      CookError error,
                      std::string message,
                      std::source_location location)
{
    ReportToSink(sink, DiagnosticSeverity::Error, std::move(message), location);
    return error;
}

void ReportWarning(DiagnosticSink& sink, std::string message, std::source_location location)
{
    ReportToSink(sink, DiagnosticSeverity::Warning, std::move(message), location);
}

void ReportInfo(DiagnosticSink& sink, std::string message, std::source_location location)
{
    ReportToSink(sink, DiagnosticSeverity::Note, std::move(message), location);
}

} // namespace lodestone
