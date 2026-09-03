#include "permute/AttributeExpression.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The cooker evaluates a lodestone attribute expression itself, because Slang folds an attribute's
// integer argument at compile time and the permutation constants only fold at link time. A size
// expression such as `[vx_element_count("IFFT_SIZE * 4")]` and an axis constraint such as
// `[vx_axis_active_when("IFFT_USE_WAVE_OPS != 0")]` both pass through this one evaluator. It is the
// one place where a shader's declared size, or the guard on a variant, can drift from what the graph
// creates, so it is worth more test surface than its size suggests.

using lodestone::CookError;
using lodestone::DiagnosticSink;
using lodestone::EvaluateExpression;
using lodestone::CollectExpressionIdentifiers;
using lodestone::SizeSymbol;
using lodestone::StderrDiagnosticSink;

namespace
{

constexpr std::array<SizeSymbol, 4> k_Symbols{ SizeSymbol{ .Name="IFFT_SIZE", .Value=512 },
                                               SizeSymbol{ .Name="IFFT_NUM_WAVE_CASCADES", .Value=4 },
                                               SizeSymbol{ .Name="IFFT_WAVE_SIZE", .Value=32 },
                                               SizeSymbol{ .Name="IFFT_USE_WAVE_OPS", .Value=1 } };

void CheckValue(lodestone::tests::TestRunner& runner,
                std::string_view expression,
                int64_t expected,
                std::string_view description,
                DiagnosticSink& sink)
{
    const auto result = EvaluateExpression(expression, k_Symbols, sink);
    runner.Check(result.has_value() && result.value() == expected, description);
}

void CheckError(lodestone::tests::TestRunner& runner,
                std::string_view expression,
                CookError expected,
                std::string_view description,
                StderrDiagnosticSink& sink)
{
    static int64_t diagCount = 0;
    const auto result = EvaluateExpression(expression, k_Symbols, sink);
    runner.Check(!result.has_value() && result.error() == expected, description);
    runner.Check(sink.FailureCount() > diagCount, "expected diagnostic emitted");
    ++diagCount;
}

void CheckIdentifierCollection(lodestone::tests::TestRunner& runner, std::string_view expression, std::initializer_list<std::string_view> expected_identifiers, StderrDiagnosticSink& sink)
{
    const auto result = CollectExpressionIdentifiers(expression, sink);
    runner.Check(result.has_value(), "collection succeeded");
    runner.Check(result.has_value() && std::ranges::equal(*result, expected_identifiers), "collected identifiers match expected");
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "AttributeExpressionTests" };
    lodestone::StderrDiagnosticSink sink;
    runner.BeginSection("literals");
    CheckValue(runner, "1", 1, "single digit", sink);
    CheckValue(runner, "4096", 4096, "multi digit", sink);
    CheckValue(runner, "512u", 512, "unsigned suffix, as Slang source writes it", sink);
    CheckValue(runner, "0x100", 256, "hexadecimal", sink);
    CheckValue(runner, "  64  ", 64, "surrounding whitespace", sink);

    runner.BeginSection("symbols");
    CheckValue(runner, "IFFT_SIZE", 512, "bare symbol", sink);
    CheckValue(runner, "IFFT_SIZE * IFFT_SIZE", 262144, "the square case the ocean demo needs", sink);
    CheckValue(
        runner, "IFFT_SIZE * IFFT_SIZE * IFFT_NUM_WAVE_CASCADES", 1048576, "cascaded texture element count", sink);

    runner.BeginSection("precedence");
    CheckValue(runner, "2 + 3 * 4", 14, "multiply binds tighter than add", sink);
    CheckValue(runner, "(2 + 3) * 4", 20, "parentheses override", sink);
    CheckValue(runner, "16 / 4 / 2", 2, "divide is left associative", sink);
    CheckValue(runner, "10 - 3 - 2", 5, "subtract is left associative", sink);
    CheckValue(runner, "1 << 4", 16, "shift left", sink);
    CheckValue(runner, "1024 >> 2", 256, "shift right", sink);
    CheckValue(runner, "1 << 2 + 1", 8, "shift is lower precedence than add", sink);
    CheckValue(runner, "17 % 5", 2, "modulo", sink);
    CheckValue(runner, "-4 + 10", 6, "leading negation", sink);
    CheckValue(runner, "IFFT_SIZE / IFFT_WAVE_SIZE", 16, "workgroup count style expression", sink);

    // A comparison yields 0 or 1, and it binds looser than every arithmetic and shift operator, so
    // the two sides evaluate whole before they are compared.
    runner.BeginSection("comparison");
    CheckValue(runner, "1 == 1", 1, "equal, true", sink);
    CheckValue(runner, "1 == 2", 0, "equal, false", sink);
    CheckValue(runner, "1 != 2", 1, "not equal, true", sink);
    CheckValue(runner, "2 != 2", 0, "not equal, false", sink);
    CheckValue(runner, "1 < 2", 1, "less than", sink);
    CheckValue(runner, "2 <= 2", 1, "less or equal, on the boundary", sink);
    CheckValue(runner, "2 <= 1", 0, "less or equal, false", sink);
    CheckValue(runner, "3 > 2", 1, "greater than", sink);
    CheckValue(runner, "2 >= 3", 0, "greater or equal, false", sink);
    CheckValue(runner, "IFFT_USE_WAVE_OPS != 0", 1, "the canonical active-when guard", sink);
    CheckValue(runner, "IFFT_WAVE_SIZE > 16", 1, "a symbol against a bound", sink);
    CheckValue(runner, "1 + 1 == 2", 1, "comparison is looser than add", sink);
    CheckValue(runner, "1 << 2 == 4", 1, "comparison is looser than shift", sink);
    CheckValue(runner, "IFFT_SIZE / IFFT_WAVE_SIZE >= 16", 1, "a computed left side", sink);
    CheckValue(runner, "(3 > 2) * 4", 4, "a true comparison is 1, so it scales", sink);
    CheckValue(runner, "(2 > 3) * 4", 0, "a false comparison is 0", sink);

    // A logical operator yields 0 or 1, and it binds looser than a comparison. `&&` and `||` do not
    // short circuit, so both sides are always read.
    runner.BeginSection("logical");
    CheckValue(runner, "1 && 1", 1, "and, both true", sink);
    CheckValue(runner, "1 && 0", 0, "and, one false", sink);
    CheckValue(runner, "0 || 0", 0, "or, both false", sink);
    CheckValue(runner, "1 || 0", 1, "or, one true", sink);
    CheckValue(runner, "2 && 3", 1, "and normalizes a nonzero to 1", sink);
    CheckValue(runner, "1 == 1 && 2 == 2", 1, "logical is looser than comparison", sink);
    CheckValue(runner, "2 > 1 && 3 > 4", 0, "one comparison fails the and", sink);
    CheckValue(runner, "2 > 3 || 4 > 1", 1, "the or takes the true side", sink);
    CheckValue(runner,
               "IFFT_USE_WAVE_OPS != 0 && IFFT_WAVE_SIZE == 32",
               1,
               "an active-when over two axes", sink);
    CheckValue(runner, "(1 || 0) && 0", 0, "parentheses group the or first", sink);

    // `!` is a unary operator, so it binds tighter than a comparison.
    runner.BeginSection("logical-not");
    CheckValue(runner, "!0", 1, "not of false", sink);
    CheckValue(runner, "!1", 0, "not of true", sink);
    CheckValue(runner, "!5", 0, "not of any nonzero", sink);
    CheckValue(runner, "!!7", 1, "double not normalizes to 1", sink);
    CheckValue(runner, "!(1 == 2)", 1, "not of a parenthesized comparison", sink);
    CheckValue(runner, "!0 == 1", 1, "not binds tighter than the comparison", sink);
    CheckValue(runner, "!(IFFT_USE_WAVE_OPS != 0)", 0, "not of the active-when guard", sink);

    runner.BeginSection("rejections");
    lodestone::StderrDiagnosticSink rejectionSink;
    CheckError(runner, "", CookError::AttributeExpressionParseFailed, "empty expression", rejectionSink);
    CheckError(runner, "IFFT_SIZ", CookError::AttributeExpressionUnknownSymbol, "typo in a symbol name", rejectionSink);
    CheckError(runner, "FFT_SIZE", CookError::AttributeExpressionUnknownSymbol, "the historical typo", rejectionSink);
    CheckError(runner, "4 / 0", CookError::AttributeExpressionDivideByZero, "divide by zero", rejectionSink);
    CheckError(runner, "4 % 0", CookError::AttributeExpressionDivideByZero, "modulo by zero", rejectionSink);
    CheckError(runner, "1 << 99", CookError::AttributeExpressionOutOfRange, "shift past the word", rejectionSink);
    CheckError(runner, "(2 + 3", CookError::AttributeExpressionParseFailed, "unclosed parenthesis", rejectionSink);
    CheckError(runner, "2 +", CookError::AttributeExpressionParseFailed, "dangling operator", rejectionSink);
    CheckError(runner, "2 3", CookError::AttributeExpressionParseFailed, "trailing text", rejectionSink);
    CheckError(runner, "$", CookError::AttributeExpressionParseFailed, "unexpected character", rejectionSink);
    CheckError(runner, "1 <", CookError::AttributeExpressionParseFailed, "dangling comparison", rejectionSink);
    CheckError(runner, "1 &&", CookError::AttributeExpressionParseFailed, "dangling logical operator", rejectionSink);
    CheckError(runner, "&& 1", CookError::AttributeExpressionParseFailed, "leading logical operator", rejectionSink);
    CheckError(runner, "1 = 2", CookError::AttributeExpressionParseFailed, "a single equals is not an operator", rejectionSink);
    CheckError(runner, "!", CookError::AttributeExpressionParseFailed, "not with no operand", rejectionSink);

    runner.BeginSection("CollectOnly mode checks");
    CheckIdentifierCollection(runner, "A + B * 2", {"A", "B"}, sink);
    CheckIdentifierCollection(runner, "C * (D + E)", {"C", "D", "E"}, sink);
    CheckIdentifierCollection(runner, "!(X == 1) && Y", {"X", "Y"}, sink);
    CheckIdentifierCollection(runner, "P || Q && R", {"P", "Q", "R"}, sink);
    // empty case
    CheckIdentifierCollection(runner, "1 << 2", {}, sink);

    return runner.Report();
}
