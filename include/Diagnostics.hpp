#pragma once
#ifndef LODESTONE_DIAGNOSTICS_HPP
#define LODESTONE_DIAGNOSTICS_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

/** Diagnostics are records shaped like the LSP `Diagnostic`, and a sink decides what becomes of them. Stages never
 * format a diagnostic string. */
namespace lodestone
{

/** @brief Slang severity mapped to LSP. `Fatal` has no LSP equivalent. */
enum class DiagnosticSeverity : uint8_t
{
    Invalid = 0,
    Note = 1,
    Warning = 2,
    Error = 3,
    Fatal = 4,
};

std::string_view ToString(DiagnosticSeverity severity) noexcept;

bool IsFailure(DiagnosticSeverity severity) noexcept;

/** @brief A half-open source range, one-based. All zeros means no location, such as "compilation ceased". */
struct SourceRange
{
    int32_t StartLine{ 0 };
    int32_t StartColumn{ 0 };
    int32_t EndLine{ 0 };
    int32_t EndColumn{ 0 };

    friend bool operator==(const SourceRange&, const SourceRange&) = default;
};

bool HasLocation(const SourceRange& range) noexcept;

struct ShaderLocation
{
    std::string_view File;
    uint32_t Line{ 0u };
    uint32_t Column{ 0u }; // column of 0 is not printed
};

[[nodiscard]] SourceRange ToSourceRange(const ShaderLocation& location) noexcept;

/** LSP `relatedInformation`. Not a diagnostic of its own, so one error is not counted several times. */
struct DiagnosticNote
{
    std::string File;
    SourceRange Range;
    std::string Message;
};

struct Diagnostic
{
    DiagnosticSeverity Severity{ DiagnosticSeverity::Invalid };
    /** Such as `E30015`. */
    std::string Code;
    std::string File;
    SourceRange Range;
    std::string Message;
    /** The part of our code that produced this report. */
    std::string Context;
    std::vector<DiagnosticNote> Related;
};

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

/** @brief `--quiet` does not silence this: a message from Slang is not ours to withhold. */
class StderrDiagnosticSink final : public DiagnosticSink
{
public:
    void Report(const Diagnostic& diagnostic) override;
    void Report(Diagnostic&& diagnostic) override;

    [[nodiscard]] int32_t FailureCount() const noexcept;

private:
    int32_t failureCount{ 0 };
};

/** @brief For tests. */
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
/** @brief The sink prints `where` in front of the message, so the message must not repeat it. */
[[nodiscard]] CookError ReportError(DiagnosticSink& sink,
                                    CookError error,
                                    const ShaderLocation& where,
                                    std::string message,
                                    std::source_location location = std::source_location::current());
[[nodiscard]] CookError ReportError(DiagnosticSink& sink,
                                    CookError error,
                                    const ShaderLocation& where,
                                    std::string message,
                                    std::vector<DiagnosticNote> related,
                                    std::source_location location = std::source_location::current());
void ReportWarning(DiagnosticSink& sink, std::string message, std::source_location location = std::source_location::current());
void ReportInfo(DiagnosticSink& sink, std::string message);

} // namespace lodestone

#endif // !LODESTONE_DIAGNOSTICS_HPP
