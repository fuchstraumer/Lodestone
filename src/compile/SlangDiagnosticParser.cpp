#include "compile/SlangDiagnosticParser.hpp"
#include "compile/Diagnostics.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    /**@brief Slang emits this in front of the message that ended the compile. The record after it repeats
     * the fatal message already reported, so the reader sees the problem twice. That seems on purpose,
     * to avoid having a recognizer be able to miss an abort message? */
    constexpr static std::string_view k_AbortPrefix = "abort compilation: ";

    constexpr static int32_t k_FieldCount = 8;
    constexpr static int32_t k_CodeField = 0;
    constexpr static int32_t k_SeverityField = 1;
    constexpr static int32_t k_FileField = 2;
    constexpr static int32_t k_StartLineField = 3;
    constexpr static int32_t k_MessageField = 7;
    using FieldStrArray = std::array<std::string_view, k_FieldCount>;

    /** Splits into exactly `k_FieldCount` fields, so a tab inside the message cannot shift the
     * columns. Gives nothing when the line has too few. */
    std::optional<FieldStrArray> SplitFields(std::string_view line)
    {
        FieldStrArray fields;

        for (int32_t i = 0; i < k_FieldCount - 1; ++i)
        {
            const size_t tab = line.find('\t');
            if (tab == std::string_view::npos)
            {
                return std::nullopt;
            }

            fields[i] = line.substr(0u, tab);
            line.remove_prefix(tab + 1u);
        }

        fields[k_FieldCount - 1] = line;
        return fields;
    }

    int32_t ParseInt(std::string_view text) noexcept
    {
        int32_t value = 0;
        // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage): the check looks for a `data()`
        // that reaches a callee with no length. `from_chars` takes the end pointer, so the length is
        // right there. Requiring the whole field to be consumed is what rejects a partial number.
        const char* const end = text.data() + text.size();
        const std::from_chars_result result = std::from_chars(text.data(), end, value);
        // C++26 will finally let us boolean convert from_chars_result directly.... please....
        return result.ec == std::errc{} && result.ptr == end ? value : 0;
    }

    SourceRange ParseRange(const FieldStrArray& fields) noexcept
    {
        return SourceRange{ .StartLine = ParseInt(fields[k_StartLineField]),
                            .StartColumn = ParseInt(fields[k_StartLineField + 1]),
                            .EndLine = ParseInt(fields[k_StartLineField + 2]),
                            .EndColumn = ParseInt(fields[k_StartLineField + 3]) };
    }

    /** `getSeverityName` in `slang-diagnostic-sink.h` writes these. `ignored` never reaches the
     * output, because a disabled diagnostic returns before it renders. */
    DiagnosticSeverity ToSeverity(std::string_view name) noexcept
    {
        if (name == "note")
        {
            return DiagnosticSeverity::Note;
        }
        else if (name == "warning")
        {
            return DiagnosticSeverity::Warning;
        }
        else if (name == "error")
        {
            return DiagnosticSeverity::Error;
        }
        else if (name == "fatal error" || name == "internal error")
        {
            return DiagnosticSeverity::Fatal;
        }
        else [[unlikely]]
        {
            // if slang adds any new severity names, we'll return this to raise it clearly
            std::println(stderr, "Unknown Slang Error name {} in SlangDiagnosticParser::ToSeverity", name);
            return DiagnosticSeverity::Error;
        }
    }

    bool IsAttachment(std::string_view severity_name) noexcept
    {
        return severity_name == "span" || severity_name == "note-span";
    }

    std::string_view StripCarriageReturn(std::string_view line) noexcept
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1u);
        }

        return line;
    }

    /** Slang writes `E` followed by the padded number, and a bare `E` when the diagnostic has no
     * code. A bare `E` becomes an empty code, because `E` alone identifies nothing. */
    std::string ParseCode(std::string_view field)
    {
        return field.size() > 1u && field.front() == 'E' ? std::string{ field } : std::string{};
    }

    /** The one thing this parser must never do is lose a line. */
    Diagnostic MakeUnreadableRecord(std::string_view line, std::string_view context)
    {
        return Diagnostic{ .Severity = DiagnosticSeverity::Error,
                           .Code = {},
                           .File = {},
                           .Range = {},
                           .Message = std::string{ line },
                           .Context = std::string{ context },
                           .Related = {} };
    }

    Diagnostic MakeRecord(const std::array<std::string_view, k_FieldCount>& fields, std::string_view context)
    {
        return Diagnostic{ .Severity = ToSeverity(fields[k_SeverityField]),
                           .Code = ParseCode(fields[k_CodeField]),
                           .File = std::string{ fields[k_FileField] },
                           .Range = ParseRange(fields),
                           .Message = std::string{ fields[k_MessageField] },
                           .Context = std::string{ context },
                           .Related = {} };
    }

    DiagnosticNote MakeNote(const std::array<std::string_view, k_FieldCount>& fields)
    {
        return DiagnosticNote{ .File = std::string{ fields[k_FileField] },
                               .Range = ParseRange(fields),
                               .Message = std::string{ fields[k_MessageField] } };
    }

    /** Collects the records of one text, so the caller reports each one once and in order. */
    class RecordBuilder final
    {
    public:
        explicit RecordBuilder(std::string_view context) noexcept
            : context{ context }
        {
        }

        void AddLine(std::string_view line)
        {
            const std::optional<FieldStrArray> fields = SplitFields(line);
            if (!fields)
            {
                records.emplace_back(MakeUnreadableRecord(line, context));
                return;
            }

            // An attachment with nothing above it has nothing to attach to, so it becomes a record of
            // its own rather than disappearing.
            if (IsAttachment((*fields)[k_SeverityField]) && !records.empty())
            {
                Diagnostic& prevDiag = records.back();
                prevDiag.Related.emplace_back(MakeNote(*fields));
                return;
            }

            records.emplace_back(MakeRecord(*fields, context));
        }

        std::vector<Diagnostic> Take() noexcept
        {
            return std::move(records);
        }

    private:
        std::string_view context;
        std::vector<Diagnostic> records;
    };

} // namespace

void ParseSlangDiagnostics(std::string_view text, std::string_view context, DiagnosticSink& sink)
{
    RecordBuilder builder{ context };

    while (!text.empty())
    {
        const size_t newline = text.find('\n');
        const std::string_view line = StripCarriageReturn(text.substr(0u, std::min(newline, text.size())));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1u);

        if (line.empty())
        {
            continue;
        }

        builder.AddLine(line.starts_with(k_AbortPrefix) ? line.substr(k_AbortPrefix.size()) : line);
    }

    for (const Diagnostic& record : builder.Take())
    {
        sink.Report(record);
    }
}

} // namespace lodestone
