#include "Diagnostics.hpp"
#include "CookerErrors.hpp"

#include <cstdint>
#include <cstdio>
#include <format>
#include <print>
#include <filesystem>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    /** `file(line,column): ` or `file(line): ` when the record names a place, and nothing when it does not. The shape
     * follows the compiler convention that an editor already knows how to click on. */
    std::string FormatLocation(std::string_view file, const SourceRange& range)
    {
        if (file.empty() || !HasLocation(range))
        {
            return {};
        }

        if (range.StartColumn == 0)
        {
            return std::format("{}({}): ", file, range.StartLine);
        }
        return std::format("{}({},{}): ", file, range.StartLine, range.StartColumn);
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

SourceRange ToSourceRange(const ShaderLocation& location) noexcept
{
    const auto line = static_cast<int32_t>(location.Line);
    const auto column = static_cast<int32_t>(location.Column);
    return SourceRange{ .StartLine = line, .StartColumn = column, .EndLine = line, .EndColumn = column };
}

void StderrDiagnosticSink::Report(const Diagnostic& diagnostic)
{
    if (IsFailure(diagnostic.Severity))
    {
        ++failureCount;
    }

    if (diagnostic.Severity != DiagnosticSeverity::Note && !diagnostic.Context.empty())
    {
        std::println(stderr,
                    "[lodestone] {}{}{}: {} [{}]",
                    FormatLocation(diagnostic.File, diagnostic.Range),
                    ToString(diagnostic.Severity),
                    FormatCode(diagnostic),
                    diagnostic.Message,
                    diagnostic.Context);
    }
    else
    {
        std::println(stderr,
                    "[lodestone] {}{}{}: {}",
                    FormatLocation(diagnostic.File, diagnostic.Range),
                    ToString(diagnostic.Severity),
                    FormatCode(diagnostic),
                    diagnostic.Message);
    }

    for (const DiagnosticNote& note : diagnostic.Related)
    {
        std::println(stderr, "[lodestone]   {}note: {}", FormatLocation(note.File, note.Range), note.Message);
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

Diagnostic MakeDiagnostic(DiagnosticSeverity severity, std::string message, std::source_location location)
{
    Diagnostic diag;
    diag.Severity = severity;
    diag.Message = std::move(message);
    // Notes and warnings carry no location
    if (severity == DiagnosticSeverity::Error || severity == DiagnosticSeverity::Fatal)
    {
        diag.Context = std::format("{}:{}", std::filesystem::path{ location.file_name() }.filename().string(), location.line());
    }
    return diag;
}

void ReportToSink(DiagnosticSink& sink,
                  DiagnosticSeverity severity,
                  std::string message,
                  std::source_location location)
{
    sink.Report(MakeDiagnostic(severity, std::move(message), location));
}

CookError ReportError(DiagnosticSink& sink,
                      CookError error,
                      std::string message,
                      std::source_location location)
{
    ReportToSink(sink, DiagnosticSeverity::Error, std::move(message), location);
    return error;
}

CookError ReportError(DiagnosticSink& sink,
                      CookError error,
                      const ShaderLocation& where,
                      std::string message,
                      std::source_location location)
{
    return ReportError(sink, error, where, std::move(message), {}, location);
}

CookError ReportError(DiagnosticSink& sink,
                      CookError error,
                      const ShaderLocation& where,
                      std::string message,
                      std::vector<DiagnosticNote> related,
                      std::source_location location)
{
    Diagnostic diag = MakeDiagnostic(DiagnosticSeverity::Error, std::move(message), location);
    diag.File = std::string{ where.File };
    diag.Range = ToSourceRange(where);
    diag.Related = std::move(related);
    sink.Report(std::move(diag));
    return error;
}

void ReportWarning(DiagnosticSink& sink, std::string message, std::source_location location)
{
    ReportToSink(sink, DiagnosticSeverity::Warning, std::move(message), location);
}

void ReportInfo(DiagnosticSink& sink, std::string message)
{
    ReportToSink(sink, DiagnosticSeverity::Note, std::move(message), {});
}

} // namespace lodestone
