#include "permute/AttributeExpression.hpp"
#include "CookerErrors.hpp"
#include "TestHarness.hpp"

#include <array>
#include <cstdint>
#include <string_view>

// The cooker evaluates a lodestone attribute expression itself, because Slang folds an attribute's
// integer argument at compile time and the permutation constants only fold at link time. A size
// expression such as `[vx_element_count("IFFT_SIZE * 4")]` and an axis constraint such as
// `[vx_axis_active_when("IFFT_USE_WAVE_OPS != 0")]` both pass through this one evaluator. It is the
// one place where a shader's declared size, or the guard on a variant, can drift from what the graph
// creates, so it is worth more test surface than its size suggests.

using lodestone::CookError;
using lodestone::EvaluateExpression;
using lodestone::SizeSymbol;

namespace
{

constexpr std::array<SizeSymbol, 4> k_Symbols{ SizeSymbol{ .Name="IFFT_SIZE", .Value=512 },
                                               SizeSymbol{ .Name="IFFT_NUM_WAVE_CASCADES", .Value=4 },
                                               SizeSymbol{ .Name="IFFT_WAVE_SIZE", .Value=32 },
                                               SizeSymbol{ .Name="IFFT_USE_WAVE_OPS", .Value=1 } };

void CheckValue(lodestone::tests::TestRunner& runner,
                std::string_view expression,
                int64_t expected,
                std::string_view description)
{
    const auto result = EvaluateExpression(expression, k_Symbols);
    runner.Check(result.has_value() && result.value() == expected, description);
}

void CheckError(lodestone::tests::TestRunner& runner,
                std::string_view expression,
                CookError expected,
                std::string_view description)
{
    const auto result = EvaluateExpression(expression, k_Symbols);
    runner.Check(!result.has_value() && result.error() == expected, description);
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "AttributeExpressionTests" };

    runner.BeginSection("literals");
    CheckValue(runner, "1", 1, "single digit");
    CheckValue(runner, "4096", 4096, "multi digit");
    CheckValue(runner, "512u", 512, "unsigned suffix, as Slang source writes it");
    CheckValue(runner, "0x100", 256, "hexadecimal");
    CheckValue(runner, "  64  ", 64, "surrounding whitespace");

    runner.BeginSection("symbols");
    CheckValue(runner, "IFFT_SIZE", 512, "bare symbol");
    CheckValue(runner, "IFFT_SIZE * IFFT_SIZE", 262144, "the square case the ocean demo needs");
    CheckValue(
        runner, "IFFT_SIZE * IFFT_SIZE * IFFT_NUM_WAVE_CASCADES", 1048576, "cascaded texture element count");

    runner.BeginSection("precedence");
    CheckValue(runner, "2 + 3 * 4", 14, "multiply binds tighter than add");
    CheckValue(runner, "(2 + 3) * 4", 20, "parentheses override");
    CheckValue(runner, "16 / 4 / 2", 2, "divide is left associative");
    CheckValue(runner, "10 - 3 - 2", 5, "subtract is left associative");
    CheckValue(runner, "1 << 4", 16, "shift left");
    CheckValue(runner, "1024 >> 2", 256, "shift right");
    CheckValue(runner, "1 << 2 + 1", 8, "shift is lower precedence than add");
    CheckValue(runner, "17 % 5", 2, "modulo");
    CheckValue(runner, "-4 + 10", 6, "leading negation");
    CheckValue(runner, "IFFT_SIZE / IFFT_WAVE_SIZE", 16, "workgroup count style expression");

    // A comparison yields 0 or 1, and it binds looser than every arithmetic and shift operator, so
    // the two sides evaluate whole before they are compared.
    runner.BeginSection("comparison");
    CheckValue(runner, "1 == 1", 1, "equal, true");
    CheckValue(runner, "1 == 2", 0, "equal, false");
    CheckValue(runner, "1 != 2", 1, "not equal, true");
    CheckValue(runner, "2 != 2", 0, "not equal, false");
    CheckValue(runner, "1 < 2", 1, "less than");
    CheckValue(runner, "2 <= 2", 1, "less or equal, on the boundary");
    CheckValue(runner, "2 <= 1", 0, "less or equal, false");
    CheckValue(runner, "3 > 2", 1, "greater than");
    CheckValue(runner, "2 >= 3", 0, "greater or equal, false");
    CheckValue(runner, "IFFT_USE_WAVE_OPS != 0", 1, "the canonical active-when guard");
    CheckValue(runner, "IFFT_WAVE_SIZE > 16", 1, "a symbol against a bound");
    CheckValue(runner, "1 + 1 == 2", 1, "comparison is looser than add");
    CheckValue(runner, "1 << 2 == 4", 1, "comparison is looser than shift");
    CheckValue(runner, "IFFT_SIZE / IFFT_WAVE_SIZE >= 16", 1, "a computed left side");
    CheckValue(runner, "(3 > 2) * 4", 4, "a true comparison is 1, so it scales");
    CheckValue(runner, "(2 > 3) * 4", 0, "a false comparison is 0");

    // A logical operator yields 0 or 1, and it binds looser than a comparison. `&&` and `||` do not
    // short circuit, so both sides are always read.
    runner.BeginSection("logical");
    CheckValue(runner, "1 && 1", 1, "and, both true");
    CheckValue(runner, "1 && 0", 0, "and, one false");
    CheckValue(runner, "0 || 0", 0, "or, both false");
    CheckValue(runner, "1 || 0", 1, "or, one true");
    CheckValue(runner, "2 && 3", 1, "and normalizes a nonzero to 1");
    CheckValue(runner, "1 == 1 && 2 == 2", 1, "logical is looser than comparison");
    CheckValue(runner, "2 > 1 && 3 > 4", 0, "one comparison fails the and");
    CheckValue(runner, "2 > 3 || 4 > 1", 1, "the or takes the true side");
    CheckValue(runner,
               "IFFT_USE_WAVE_OPS != 0 && IFFT_WAVE_SIZE == 32",
               1,
               "an active-when over two axes");
    CheckValue(runner, "(1 || 0) && 0", 0, "parentheses group the or first");

    // `!` is a unary operator, so it binds tighter than a comparison.
    runner.BeginSection("logical-not");
    CheckValue(runner, "!0", 1, "not of false");
    CheckValue(runner, "!1", 0, "not of true");
    CheckValue(runner, "!5", 0, "not of any nonzero");
    CheckValue(runner, "!!7", 1, "double not normalizes to 1");
    CheckValue(runner, "!(1 == 2)", 1, "not of a parenthesized comparison");
    CheckValue(runner, "!0 == 1", 1, "not binds tighter than the comparison");
    CheckValue(runner, "!(IFFT_USE_WAVE_OPS != 0)", 0, "not of the active-when guard");

    runner.BeginSection("rejections");
    CheckError(runner, "", CookError::AttributeExpressionParseFailed, "empty expression");
    CheckError(runner, "IFFT_SIZ", CookError::AttributeExpressionUnknownSymbol, "typo in a symbol name");
    CheckError(runner, "FFT_SIZE", CookError::AttributeExpressionUnknownSymbol, "the historical typo");
    CheckError(runner, "4 / 0", CookError::AttributeExpressionDivideByZero, "divide by zero");
    CheckError(runner, "4 % 0", CookError::AttributeExpressionDivideByZero, "modulo by zero");
    CheckError(runner, "1 << 99", CookError::AttributeExpressionOutOfRange, "shift past the word");
    CheckError(runner, "(2 + 3", CookError::AttributeExpressionParseFailed, "unclosed parenthesis");
    CheckError(runner, "2 +", CookError::AttributeExpressionParseFailed, "dangling operator");
    CheckError(runner, "2 3", CookError::AttributeExpressionParseFailed, "trailing text");
    CheckError(runner, "$", CookError::AttributeExpressionParseFailed, "unexpected character");
    CheckError(runner, "1 <", CookError::AttributeExpressionParseFailed, "dangling comparison");
    CheckError(runner, "1 &&", CookError::AttributeExpressionParseFailed, "dangling logical operator");
    CheckError(runner, "&& 1", CookError::AttributeExpressionParseFailed, "leading logical operator");
    CheckError(runner, "1 = 2", CookError::AttributeExpressionParseFailed, "a single equals is not an operator");
    CheckError(runner, "!", CookError::AttributeExpressionParseFailed, "not with no operand");

    return runner.Report();
}
