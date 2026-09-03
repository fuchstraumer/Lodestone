#include "Diagnostics.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "TestHarness.hpp"
#include <string>
#include <string_view>
#include <vector>

// The parser is the boundary between what Slang says and what the cooker knows. Everything after it
// reads a record, so a field this file gets wrong is a field no later stage can question.
//
// It takes a string, so it needs no compiler and no asset. The text in `k_RealCapture` is a real
// capture created by writing a purposefully broken test function. By comparing against that,
// we know that

using lodestone::Diagnostic;
using lodestone::DiagnosticSeverity;
using lodestone::ParseSlangDiagnostics;
using lodestone::RecordingDiagnosticSink;
using lodestone::tests::TestRunner;

namespace
{

constexpr std::string_view k_ModulePath = R"(D:\ShaderTools\tests\assets\compute\Ocean\OceanFft.slang)";

// Tabs are the field separator, so they are written as escapes rather than typed.
const std::string k_RealCapture =
    std::string{ "E30015\terror\t" } + std::string{ k_ModulePath } +
    "\t10\t25\t10\t51\tundefined identifier\n" + "E30015\tspan\t" + std::string{ k_ModulePath } +
    "\t10\t25\t10\t51\tundefined identifier 'this_symbol_does_not_exist'.\n" +
    "E39999\terror\t\t0\t0\t0\t0\timport failed due to compilation error\n" +
    "E39999\tspan\t\t0\t0\t0\t0\timport of module 'OceanFft' failed because of a compilation error\n" +
    "E40003\tfatal error\t\t0\t0\t0\t0\tcompilation ceased\n" +
    "abort compilation: E40003\tfatal error\t\t0\t0\t0\t0\tcompilation ceased\n";

/** Parses `text` and gives back everything the sink kept. */
std::vector<Diagnostic> Parse(std::string_view text, std::string_view context = "test")
{
    RecordingDiagnosticSink sink;
    ParseSlangDiagnostics(text, context, sink);
    return sink.Records();
}

void TestRealCapture(TestRunner& runner)
{
    runner.BeginSection("a real capture");

    const std::vector<Diagnostic> records = Parse(k_RealCapture, "loadModule");

    // Six lines in, four records out. Two of the six are spans, and a span is detail about the
    // message above it rather than a problem of its own.
    runner.Check(records.size() == 4u, "a span does not become a record of its own");

    if (records.size() != 4u)
    {
        return;
    }

    runner.Check(records[0].Severity == DiagnosticSeverity::Error, "the primary message is an error");
    runner.Check(records[0].Code == "E30015", "the code keeps the E prefix the compiler wrote");
    runner.Check(records[0].File == k_ModulePath, "the file reaches the record");
    runner.Check(records[0].Range.StartLine == 10 && records[0].Range.StartColumn == 25,
                 "the range starts where the compiler said");
    runner.Check(records[0].Range.EndLine == 10 && records[0].Range.EndColumn == 51,
                 "the range ends where the compiler said");
    runner.Check(records[0].Message == "undefined identifier", "the message reaches the record");
    runner.Check(records[0].Context == "loadModule", "the calling context reaches every record");

    runner.Check(records[0].Related.size() == 1u, "the span attaches to the message above it");
    runner.Check(records[0].Related.front().Message == "undefined identifier 'this_symbol_does_not_exist'.",
                 "the span carries the detailed wording, so it must not be dropped");

    runner.Check(records[1].File.empty() && records[1].Range == lodestone::SourceRange{},
                 "a message about no place in particular keeps no place in particular");
    runner.Check(records[2].Severity == DiagnosticSeverity::Fatal, "a fatal error is not an error");
    runner.Check(records[3].Message == "compilation ceased",
                 "the aborting line is parsed rather than kept as raw text");
}

void TestSeverities(TestRunner& runner)
{
    runner.BeginSection("severities");

    const std::vector<Diagnostic> records = Parse("E1\twarning\tf\t1\t1\t1\t2\tw\n"
                                                  "E2\tnote\tf\t1\t1\t1\t2\tn\n"
                                                  "E3\tinternal error\tf\t1\t1\t1\t2\ti\n"
                                                  "E4\tsomething slang added later\tf\t1\t1\t1\t2\tu\n");

    runner.Check(records.size() == 4u, "each line gives one record");

    if (records.size() != 4u)
    {
        return;
    }

    runner.Check(records[0].Severity == DiagnosticSeverity::Warning, "warning");
    runner.Check(records[1].Severity == DiagnosticSeverity::Note, "note");
    runner.Check(records[2].Severity == DiagnosticSeverity::Fatal,
                 "an internal error stopped the compiler, so it is fatal");
    // The safe direction is the only direction. A name nobody predicted must not quietly become a
    // note that the cook then ignores.
    runner.Check(records[3].Severity == DiagnosticSeverity::Error,
                 "a severity name this parser does not know becomes an error, never a note");
}

void TestNothingIsLost(TestRunner& runner)
{
    runner.BeginSection("nothing is lost");

    // Losing a line would turn a failed compile into a silent success, so a line that does not parse
    // is still reported, with its own text as the message.
    const std::vector<Diagnostic> unreadable = Parse("this line is not tab separated at all");
    runner.Check(unreadable.size() == 1u, "an unreadable line still produces a record");
    runner.Check(!unreadable.empty() && unreadable.front().Severity == DiagnosticSeverity::Error,
                 "an unreadable line is an error, because nobody can say it was not");
    runner.Check(!unreadable.empty() && unreadable.front().Message == "this line is not tab separated at all",
                 "an unreadable line keeps its own text");

    const std::vector<Diagnostic> tooFewFields = Parse("E1\terror\tfile\t1\t2\n");
    runner.Check(tooFewFields.size() == 1u && tooFewFields.front().Message == "E1\terror\tfile\t1\t2",
                 "a truncated record is kept whole rather than half read");

    // A span with nothing above it has nothing to attach to.
    const std::vector<Diagnostic> orphan = Parse("E1\tspan\tf\t1\t1\t1\t2\tdetail\n");
    runner.Check(orphan.size() == 1u, "a span with no message above it becomes a record");
}

void TestLineHandling(TestRunner& runner)
{
    runner.BeginSection("line handling");

    const std::vector<Diagnostic> crlf =
        Parse("E1\terror\tf\t1\t1\t1\t2\tm\r\nE2\terror\tf\t2\t1\t2\t2\tn\r\n");
    runner.Check(crlf.size() == 2u && crlf.front().Message == "m",
                 "a carriage return does not reach the message");

    const std::vector<Diagnostic> blanks = Parse("\n\nE1\terror\tf\t1\t1\t1\t2\tm\n\n");
    runner.Check(blanks.size() == 1u, "a blank line produces nothing");

    const std::vector<Diagnostic> noTrailingNewline = Parse("E1\terror\tf\t1\t1\t1\t2\tm");
    runner.Check(noTrailingNewline.size() == 1u, "a last line with no newline is still read");

    // The message is the last field, so it takes the rest of the line. A tab inside it must not shift
    // every column to the left.
    const std::vector<Diagnostic> tabbed = Parse("E1\terror\tf\t1\t1\t1\t2\tone\ttwo\n");
    runner.Check(tabbed.size() == 1u && tabbed.front().Message == "one\ttwo",
                 "a tab inside the message stays in the message");
    runner.Check(!tabbed.empty() && tabbed.front().Range.StartLine == 1,
                 "a tab inside the message does not move the range");
}

void TestCodes(TestRunner& runner)
{
    runner.BeginSection("codes");

    // Slang writes a bare `E` when the diagnostic has no number. `E` on its own identifies nothing,
    // so it becomes no code rather than a code of "E".
    const std::vector<Diagnostic> bare = Parse("E\terror\tf\t1\t1\t1\t2\tm\n");
    runner.Check(bare.size() == 1u && bare.front().Code.empty(), "a bare E is no code");

    const std::vector<Diagnostic> padded = Parse("E00042\terror\tf\t1\t1\t1\t2\tm\n");
    runner.Check(padded.size() == 1u && padded.front().Code == "E00042",
                 "the code keeps the padding the compiler wrote");
}

void TestEmptyInput(TestRunner& runner)
{
    runner.BeginSection("empty input");

    runner.Check(Parse("").empty(), "no text gives no records");
    runner.Check(Parse("\n\n\n").empty(), "newlines alone give no records");
}

} // namespace

int main()
{
    TestRunner runner{ "DiagnosticParserTests" };

    TestRealCapture(runner);
    TestSeverities(runner);
    TestNothingIsLost(runner);
    TestLineHandling(runner);
    TestCodes(runner);
    TestEmptyInput(runner);

    return runner.Report();
}
