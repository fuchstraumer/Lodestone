#pragma once
#ifndef LODESTONE_DIAGNOSTICS_HPP
#define LODESTONE_DIAGNOSTICS_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

/** A diagnostic is a record, and a sink decides what becomes of it. 
 *
 * So the boundary parses, and everything after the boundary reports. **A stage must never format a
 * diagnostic string.** It fills a record and hands the record to a sink.
 *
 * The record takes its shape from the Language Server Protocol `Diagnostic` type: a range, a
 * severity, a code, a message, and related information. Editors already read that shape, so a future
 * language server needs a translation and not a redesign.
 *
 * `SlangDiagnosticParser.hpp` is where the compiler's own format is understood, and it understands it as
 * text. */
namespace lodestone
{

/**@brief The translated severity from Slang -> LSP.
 * @note `Fatal` has no direct LSP equivalent, but indicates a full compiler loss or crash
 */
enum class DiagnosticSeverity : uint8_t
{
    Invalid = 0,
    Note = 1,
    Warning = 2,
    Error = 3,
    Fatal = 4,
};

std::string_view ToString(DiagnosticSeverity severity) noexcept;

/**@brief True for a severity that must fail the cook. */
bool IsFailure(DiagnosticSeverity severity) noexcept;

/**@brief A half-open range of source, one based in both axes, as the compiler reports it.
 *
 * Zero in every field means the message names no location. That is a real answer and not a missing
 * one: "compilation ceased" belongs to no line. LSP has no way to say it, so a translation to LSP
 * has to invent a location, and this type keeps the truth until then. */
struct SourceRange
{
    int32_t StartLine{ 0 };
    int32_t StartColumn{ 0 };
    int32_t EndLine{ 0 };
    int32_t EndColumn{ 0 };

    friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

/** True when the range names a place in a file. */
bool HasLocation(const SourceRange& range) noexcept;

/** One extra place the compiler wants the reader to look. LSP calls this `relatedInformation`.
 *
 * Slang attaches these to a primary message: the span that carries the detailed wording, another
 * span that points at a conflicting declaration, and a note that explains a substitution. They are
 * not diagnostics of their own, and counting them as such would report one error several times. */
struct DiagnosticNote
{
    std::string File;
    SourceRange Range;
    std::string Message;
};

struct Diagnostic
{
    DiagnosticSeverity Severity{ DiagnosticSeverity::Invalid };
    /** The compiler's own identifier, such as `E30015` */
    std::string Code;
    std::string File;
    SourceRange Range;
    std::string Message;
    /** `Context` is which part of our code produced this report. */
    std::string Context;
    std::vector<DiagnosticNote> Related;
};

/**@brief Where a diagnostic output goes. Only one method on purpose,
 * as this is intended to be nothing but a data-forwarding class.
 */
class DiagnosticSink
{
public:
    DiagnosticSink() noexcept = default;
    virtual ~DiagnosticSink() = default;
    DiagnosticSink(const DiagnosticSink&) = delete;
    DiagnosticSink& operator=(const DiagnosticSink&) = delete;
    DiagnosticSink(DiagnosticSink&&) noexcept = default;
    DiagnosticSink& operator=(DiagnosticSink&&) noexcept = default;

    virtual void Report(const Diagnostic& diagnostic) = 0;
    virtual void Report(Diagnostic&& diagnostic) = 0;
};

/**@brief Writes each record to `stderr`.
 * @note Currently no suppression switch - `--quiet` turns off internal per-variant reflection reports,
 * not this. A message from Slang is not our message to withhold.
 */
class StderrDiagnosticSink final : public DiagnosticSink
{
public:
    void Report(const Diagnostic& diagnostic) override;
    void Report(Diagnostic&& diagnostic) override;

    /**@brief Cumulative failure count over the whole cook */
    [[nodiscard]] int32_t FailureCount() const noexcept;

private:
    int32_t failureCount{ 0 };
};

/**@brief Same as `stderr`: not filtering or withholding. Used by tests to capture and persist diagnostics. */
class RecordingDiagnosticSink final : public DiagnosticSink
{
public:
    RecordingDiagnosticSink() noexcept = default;
    RecordingDiagnosticSink(RecordingDiagnosticSink&& other) noexcept;
    RecordingDiagnosticSink& operator=(RecordingDiagnosticSink&& other) noexcept;
    void Report(const Diagnostic& diagnostic) override;
    void Report(Diagnostic&& diagnostic) override;

    [[nodiscard]] const std::vector<Diagnostic>& Records() const noexcept;
    [[nodiscard]] std::vector<Diagnostic>&& Records() noexcept;

    void Reset() noexcept;

private:
    std::vector<Diagnostic> records;
};

[[nodiscard]] CookError ReportError(DiagnosticSink& sink,
                                    CookError error,
                                    std::string message,
                                    std::source_location location = std::source_location::current());
void ReportWarning(DiagnosticSink& sink, std::string message, std::source_location location = std::source_location::current());
void ReportInfo(DiagnosticSink& sink, std::string message);

} // namespace lodestone

#endif // !LODESTONE_DIAGNOSTICS_HPP
