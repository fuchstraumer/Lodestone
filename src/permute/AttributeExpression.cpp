#include "permute/AttributeExpression.hpp"
#include "CookerErrors.hpp"
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <print>
#include <span>
#include <string_view>
#include <system_error>

#ifdef __clang__
// The warning about RVO failures are for std::unexpected value returns, which are fine
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnrvo"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage" // offsets from string pointers
// without this we get warnings about recursion... in a recursive descent parser. silenced.
//NOLINTBEGIN(misc-no-recursion)
#endif

namespace lodestone
{

namespace
{

    bool IsIdentifierStart(char character) noexcept
    {
        return std::isalpha(static_cast<unsigned char>(character)) != 0 || character == '_';
    }

    bool IsIdentifierCharacter(char character) noexcept
    {
        return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_';
    }

    /** Recursive descent over a fixed grammar. Every failure returns an error rather than a default,
     * because a size that silently evaluates to zero allocates a zero-byte buffer and fails much
     * later, somewhere unrelated. A constraint that silently evaluates to zero is a false that the
     * author did not write, which prunes a variant that should have cooked.
     *
     * A comparison and a logical operator yield 0 or 1, and every other level treats a nonzero
     * value as true. This matches C and Slang, so `(IFFT_SIZE > 256) * 4` reads the way the author
     * expects. `&&` and `||` do NOT short circuit: this parser evaluates as it descends, so it has no
     * unevaluated right operand to skip. The only effect is that a divide by zero or an out-of-range
     * shift on the right of a false `&&` still fails, where C would not reach it. A constraint that
     * relies on that guard is rare, and the failure is loud rather than silent. */
    class ExpressionParser
    {
    public:
        ExpressionParser(std::string_view expression, std::span<const SizeSymbol> symbols) noexcept :
            text{ expression },
            symbolTable{ symbols }
        {
        }

        CookResult<int64_t> ParseComplete()
        {
            const CookResult<int64_t> value = parseLogical();
            if (!value)
            {
                return value;
            }

            skipWhitespace();
            if (cursor != text.size())
            {
                std::println(stderr,
                             "[shader_cooker] attribute expression '{}' has trailing text at offset {}",
                             text,
                             cursor);
                return std::unexpected(CookError::SizeExpressionParseFailed);
            }

            return value;
        }

    private:

        void skipWhitespace() noexcept
        {
            while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
            {
                ++cursor;
            }
        }

        bool consumeOperator(std::string_view oper) noexcept
        {
            skipWhitespace();
            if (text.compare(cursor, oper.size(), oper) != 0)
            {
                return false;
            }

            cursor += oper.size();
            return true;
        }

        CookResult<int64_t> parseLogical()
        {
            CookResult<int64_t> left = parseComparison();
            if (!left)
            {
                return left;
            }

            while (true)
            {
                skipWhitespace();
                const bool isAnd = consumeOperator("&&");
                const bool isOr = !isAnd && consumeOperator("||");
                if (!isAnd && !isOr)
                {
                    break;
                }

                const CookResult<int64_t> right = parseComparison();
                if (!right)
                {
                    return right;
                }

                const bool leftTrue = left.value() != 0;
                const bool rightTrue = right.value() != 0;
                left = static_cast<int64_t>(isAnd ? (leftTrue && rightTrue) : (leftTrue || rightTrue));
            }

            return left;
        }

        CookResult<int64_t> parseComparison()
        {
            CookResult<int64_t> left = parseShift();
            if (!left)
            {
                return left;
            }

            while (true)
            {
                // A two-character operator must be tested before its one-character prefix, or `<=`
                // reads as `<` and leaves a stray `=`. A `<<` or `>>` cannot reach this point,
                // because `parseShift` consumes every shift before it returns, so a `<` or `>` here
                // is always a comparison.
                Comparison comparison = Comparison::None;
                if (consumeOperator("=="))
                {
                    comparison = Comparison::Equal;
                }
                else if (consumeOperator("!="))
                {
                    comparison = Comparison::NotEqual;
                }
                else if (consumeOperator("<="))
                {
                    comparison = Comparison::LessOrEqual;
                }
                else if (consumeOperator(">="))
                {
                    comparison = Comparison::GreaterOrEqual;
                }
                else if (consumeOperator("<"))
                {
                    comparison = Comparison::Less;
                }
                else if (consumeOperator(">"))
                {
                    comparison = Comparison::Greater;
                }
                else
                {
                    break;
                }

                const CookResult<int64_t> right = parseShift();
                if (!right)
                {
                    return right;
                }

                left = static_cast<int64_t>(Compare(comparison, left.value(), right.value()));
            }

            return left;
        }

        CookResult<int64_t> parseShift()
        {
            CookResult<int64_t> left = parseSum();
            if (!left)
            {
                return left;
            }

            while (peekIsShift())
            {
                const bool shiftLeft = consumeOperator("<<");
                if (!shiftLeft && !consumeOperator(">>"))
                {
                    break;
                }

                const CookResult<int64_t> right = parseSum();
                if (!right)
                {
                    return right;
                }

                if (right.value() < 0 || right.value() >= 64)
                {
                    std::println(stderr,
                                 "[shader_cooker] attribute expression '{}' shifts by {}, which is out "
                                 "of range",
                                 text,
                                 right.value());
                    return std::unexpected(CookError::SizeExpressionOutOfRange);
                }

                left = shiftLeft ? (left.value() << right.value()) : (left.value() >> right.value());
            }

            return left;
        }

        CookResult<int64_t> parseSum()
        {
            CookResult<int64_t> left = parseProduct();
            if (!left)
            {
                return left;
            }

            while (true)
            {
                skipWhitespace();
                const bool isAdd = consumeOperator("+");
                const bool isSubtract = !isAdd && consumeOperator("-");
                if (!isAdd && !isSubtract)
                {
                    break;
                }

                const CookResult<int64_t> right = parseProduct();
                if (!right)
                {
                    return right;
                }

                left = isAdd ? (left.value() + right.value()) : (left.value() - right.value());
            }

            return left;
        }

        CookResult<int64_t> parseProduct()
        {
            CookResult<int64_t> left = parseUnary();
            if (!left)
            {
                return left;
            }

            while (true)
            {
                skipWhitespace();
                const bool isMultiply = consumeOperator("*");
                const bool isDivide = !isMultiply && consumeOperator("/");
                const bool isModulo = !isMultiply && !isDivide && consumeOperator("%");
                if (!isMultiply && !isDivide && !isModulo)
                {
                    break;
                }

                const CookResult<int64_t> right = parseUnary();
                if (!right)
                {
                    return right;
                }

                if ((isDivide || isModulo) && right.value() == 0)
                {
                    std::println(stderr, "[shader_cooker] attribute expression '{}' divides by zero", text);
                    return std::unexpected(CookError::SizeExpressionDivideByZero);
                }

                if (isMultiply)
                {
                    left = left.value() * right.value();
                }
                else if (isDivide)
                {
                    left = left.value() / right.value();
                }
                else
                {
                    left = left.value() % right.value();
                }
            }

            return left;
        }

        CookResult<int64_t> parseUnary()
        {
            skipWhitespace();
            // `!` is tested before the operand. It never reads the `!` of a `!=`, because `!=` is a
            // comparison operator and `parseComparison` consumes it before an operand begins.
            if (consumeOperator("!"))
            {
                const CookResult<int64_t> operand = parseUnary();
                if (!operand)
                {
                    return operand;
                }

                return static_cast<int64_t>(operand.value() == 0);
            }

            if (consumeOperator("-"))
            {
                const CookResult<int64_t> operand = parseUnary();
                if (!operand)
                {
                    return operand;
                }

                return -operand.value();
            }

            return parsePrimary();
        }

        CookResult<int64_t> parsePrimary()
        {
            skipWhitespace();
            if (cursor >= text.size())
            {
                std::println(stderr, "[shader_cooker] attribute expression '{}' ends early", text);
                return std::unexpected(CookError::SizeExpressionParseFailed);
            }

            if (consumeOperator("("))
            {
                const CookResult<int64_t> inner = parseLogical();
                if (!inner)
                {
                    return inner;
                }

                if (!consumeOperator(")"))
                {
                    std::println(stderr,
                                 "[shader_cooker] attribute expression '{}' is missing a closing "
                                 "parenthesis",
                                 text);
                    return std::unexpected(CookError::SizeExpressionParseFailed);
                }

                return inner;
            }

            if (std::isdigit(static_cast<unsigned char>(text[cursor])) != 0)
            {
                return parseInteger();
            }

            if (IsIdentifierStart(text[cursor]))
            {
                return parseIdentifier();
            }

            std::println(stderr,
                         "[shader_cooker] attribute expression '{}' has an unexpected character '{}' at "
                         "offset {}",
                         text,
                         text[cursor],
                         cursor);
            return std::unexpected(CookError::SizeExpressionParseFailed);
        }

        CookResult<int64_t> parseInteger()
        {
            int base = 10;
            size_t digitsBegin = cursor;

            if (text.compare(cursor, 2u, "0x") == 0 || text.compare(cursor, 2u, "0X") == 0)
            {
                base = 16;
                digitsBegin = cursor + 2u;
            }

            size_t digitsEnd = digitsBegin;
            while (digitsEnd < text.size() &&
                   std::isalnum(static_cast<unsigned char>(text[digitsEnd])) != 0)
            {
                ++digitsEnd;
            }

            // A trailing `u` or `U` lets an expression be pasted out of Slang source unchanged.
            size_t valueEnd = digitsEnd;
            if (valueEnd > digitsBegin && base == 10 &&
                (text[valueEnd - 1u] == 'u' || text[valueEnd - 1u] == 'U'))
            {
                --valueEnd;
            }

            int64_t value = 0;
            const char* first = text.data() + digitsBegin;
            const char* last = text.data() + valueEnd;
            const std::from_chars_result result = std::from_chars(first, last, value, base);

            if (result.ec != std::errc{} || result.ptr != last || valueEnd == digitsBegin)
            {
                std::println(stderr,
                             "[shader_cooker] attribute expression '{}' has a malformed integer at "
                             "offset {}",
                             text,
                             cursor);
                return std::unexpected(CookError::SizeExpressionParseFailed);
            }

            cursor = digitsEnd;
            return value;
        }

        CookResult<int64_t> parseIdentifier()
        {
            const size_t nameBegin = cursor;
            while (cursor < text.size() && IsIdentifierCharacter(text[cursor]))
            {
                ++cursor;
            }

            const std::string_view name = text.substr(nameBegin, cursor - nameBegin);
            for (const SizeSymbol& symbol : symbolTable)
            {
                if (symbol.Name == name)
                {
                    return symbol.Value;
                }
            }

            std::println(stderr,
                         "[shader_cooker] attribute expression '{}' names '{}', which is not a "
                         "permutation constant of this module",
                         text,
                         name);
            return std::unexpected(CookError::SizeExpressionUnknownSymbol);
        }

        /** `<` and `>` alone are comparisons now, so a shift needs both characters to tell it from a
         * comparison. This two-character peek is what lets the comparison level trust that no shift
         * is left when it reads a single `<` or `>`. */
        bool peekIsShift() noexcept
        {
            skipWhitespace();
            return text.compare(cursor, 2u, "<<") == 0 || text.compare(cursor, 2u, ">>") == 0;
        }

        enum class Comparison : uint8_t
        {
            None,
            Equal,
            NotEqual,
            Less,
            LessOrEqual,
            Greater,
            GreaterOrEqual
        };

        static bool Compare(Comparison comparison, int64_t left, int64_t right) noexcept
        {
            switch (comparison)
            {
            case Comparison::Equal:
                return left == right;
            case Comparison::NotEqual:
                return left != right;
            case Comparison::Less:
                return left < right;
            case Comparison::LessOrEqual:
                return left <= right;
            case Comparison::Greater:
                return left > right;
            case Comparison::GreaterOrEqual:
                return left >= right;
            case Comparison::None:
                break;
            }

            return false;
        }

        std::string_view text;
        std::span<const SizeSymbol> symbolTable;
        size_t cursor{};
    };

} // namespace

CookResult<int64_t> EvaluateExpression(std::string_view expression,
                                       std::span<const SizeSymbol> symbols)
{
    if (expression.empty())
    {
        std::println(stderr, "[shader_cooker] attribute expression is empty");
        return std::unexpected(CookError::SizeExpressionParseFailed);
    }

    ExpressionParser parser{ expression, symbols };
    return parser.ParseComplete();
}


#ifdef __clang__
#pragma clang diagnostic pop
//NOLINTEND(misc-no-recursion)
#endif

} // namespace lodestone
