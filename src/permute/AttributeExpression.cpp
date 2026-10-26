#include "permute/AttributeExpression.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnrvo"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage" // offsets from string pointers
// warnings about recursion... in a recursive descent parser. silenced.
// NOLINTBEGIN(misc-no-recursion)
#endif

namespace lodestone
{

namespace
{

    bool IsIdentifierStart(char character) noexcept;
    bool IsIdentifierCharacter(char character) noexcept;

    /** @brief `CollectOnly` walks the input and gathers identifiers without evaluating. */
    enum class ParserMode : uint8_t
    {
        None,
        Default,
        CollectOnly
    };

    /** Recursive descent. Every failure is an error, never a default: a silent zero allocates an empty buffer or prunes
     * a variant. Comparisons and logic yield 0 or 1, as in C and Slang. `&&` and `||` do not short circuit. */
    class ExpressionParser
    {
    public:
        ExpressionParser(std::string_view expression,
                         std::span<const AttrExprSymbol> symbols,
                         DiagnosticSink& _sink,
                         ParserMode mode) noexcept;

        CookResult<int64_t> ParseComplete();

        std::vector<std::string>&& ConsumeIdentifiers() noexcept;

    private:
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

        void skipWhitespace() noexcept;
        bool consumeOperator(std::string_view oper) noexcept;
        CookResult<int64_t> parseLogical();
        CookResult<int64_t> parseComparison();
        CookResult<int64_t> parseShift();
        CookResult<int64_t> parseSum();
        CookResult<int64_t> parseProduct();
        CookResult<int64_t> parseUnary();
        CookResult<int64_t> parsePrimary();
        CookResult<int64_t> parseInteger();
        CookResult<int64_t> parseIdentifier();
        /** A shift needs both characters, which lets the comparison level trust a single `<` or `>`. */
        bool peekIsShift() noexcept;
        static bool evaluateComparison(Comparison comparison, int64_t left, int64_t right) noexcept;

        std::string_view text;
        std::span<const AttrExprSymbol> symbolTable;
        DiagnosticSink& sink; // clang will complain we're using a ref member, but whatever dude. its fine.
        ParserMode parserMode;
        size_t cursor{};
        std::vector<std::string> identifiers;
    };

} // namespace

CookResult<int64_t> EvaluateExpression(std::string_view expression,
                                       std::span<const AttrExprSymbol> symbols,
                                       DiagnosticSink& sink)
{
    if (expression.empty())
    {
        const CookError error =
            ReportError(sink, CookError::AttributeExpressionParseFailed, "Attribute expression is empty");
        return std::unexpected(error);
    }

    ExpressionParser parser{ expression, symbols, sink, ParserMode::Default };
    return parser.ParseComplete();
}

CookResult<std::vector<std::string>> CollectExpressionIdentifiers(std::string_view expression,
                                                                  DiagnosticSink& sink)
{
    if (expression.empty())
    {
        const CookError error =
            ReportError(sink, CookError::AttributeExpressionParseFailed, "Attribute expression is empty");
        return std::unexpected(error);
    }

    ExpressionParser parser{ expression, {}, sink, ParserMode::CollectOnly };
    auto parseResult = parser.ParseComplete();
    if (!parseResult)
    {
        return std::unexpected(parseResult.error());
    }

    return parser.ConsumeIdentifiers();
}


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


    ExpressionParser::ExpressionParser(std::string_view expression,
                                     std::span<const AttrExprSymbol> symbols,
                                     DiagnosticSink& _sink,
                                     ParserMode mode) noexcept
        : text{ expression },
          symbolTable{ symbols },
          sink{ _sink },
          parserMode{ mode }
    {
    }

    CookResult<int64_t> ExpressionParser::ParseComplete()
    {
        const CookResult<int64_t> value = parseLogical();
        if (!value)
        {
            return value;
        }

        skipWhitespace();
        if (cursor != text.size())
        {
            const CookError error = ReportError(
                sink, CookError::AttributeExpressionParseFailed, "Trailing text in attribute expression");
            return std::unexpected(error);
        }

        return value;
    }


    std::vector<std::string>&& ExpressionParser::ConsumeIdentifiers() noexcept
    {
        return std::move(identifiers);
    }

    void ExpressionParser::skipWhitespace() noexcept
    {
        while (cursor < text.size() && std::isspace(static_cast<unsigned char>(text[cursor])) != 0)
        {
            ++cursor;
        }
    }


    bool ExpressionParser::consumeOperator(std::string_view oper) noexcept
    {
        skipWhitespace();
        if (text.compare(cursor, oper.size(), oper) != 0)
        {
            return false;
        }

        cursor += oper.size();
        return true;
    }


    CookResult<int64_t> ExpressionParser::parseLogical()
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


    CookResult<int64_t> ExpressionParser::parseComparison()
    {
        CookResult<int64_t> left = parseShift();
        if (!left)
        {
            return left;
        }

        while (true)
        {
            // test `<=` before `<`. parseShift already consumed every `<<` and `>>`
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

            left = static_cast<int64_t>(evaluateComparison(comparison, left.value(), right.value()));
        }

        return left;
    }


    CookResult<int64_t> ExpressionParser::parseShift()
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

            if (parserMode == ParserMode::CollectOnly)
            {
                continue;
            }

            if (right.value() < 0 || right.value() >= 64)
            {
                const CookError error =
                    ReportError(sink,
                                CookError::AttributeExpressionOutOfRange,
                                "Attribute expression shifts by an out-of-range value");
                return std::unexpected(error);
            }

            left = shiftLeft ? (left.value() << right.value()) : (left.value() >> right.value());
        }

        return left;
    }


    CookResult<int64_t> ExpressionParser::parseSum()
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


    CookResult<int64_t> ExpressionParser::parseProduct()
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

            if (parserMode == ParserMode::CollectOnly)
            {
                continue;
            }

            if ((isDivide || isModulo) && right.value() == 0)
            {
                const CookError error = ReportError(sink,
                                                    CookError::AttributeExpressionDivideByZero,
                                                    "Attribute expression divides by zero");
                return std::unexpected(error);
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


    CookResult<int64_t> ExpressionParser::parseUnary()
    {
        skipWhitespace();
        // never a `!=`: parseComparison consumed it
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


    CookResult<int64_t> ExpressionParser::parsePrimary()
    {
        skipWhitespace();
        if (cursor >= text.size())
        {
            const CookError error = ReportError(
                sink, CookError::AttributeExpressionParseFailed, "Attribute expression ends early");
            return std::unexpected(error);
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
                const CookError error =
                    ReportError(sink,
                                CookError::AttributeExpressionParseFailed,
                                "Attribute expression is missing a closing parenthesis");
                return std::unexpected(error);
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

        const CookError error = ReportError(sink,
                                            CookError::AttributeExpressionParseFailed,
                                            "Attribute expression has an unexpected character");
        return std::unexpected(error);
    }


    CookResult<int64_t> ExpressionParser::parseInteger()
    {
        int base = 10;
        size_t digitsBegin = cursor;

        if (text.compare(cursor, 2u, "0x") == 0 || text.compare(cursor, 2u, "0X") == 0)
        {
            base = 16;
            digitsBegin = cursor + 2u;
        }

        size_t digitsEnd = digitsBegin;
        while (digitsEnd < text.size() && std::isalnum(static_cast<unsigned char>(text[digitsEnd])) != 0)
        {
            ++digitsEnd;
        }

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
            const CookError error = ReportError(sink,
                                                CookError::AttributeExpressionParseFailed,
                                                "Attribute expression has a malformed integer");
            return std::unexpected(error);
        }

        cursor = digitsEnd;
        return value;
    }


    CookResult<int64_t> ExpressionParser::parseIdentifier()
    {
        const size_t nameBegin = cursor;
        while (cursor < text.size() && IsIdentifierCharacter(text[cursor]))
        {
            ++cursor;
        }

        const std::string_view name = text.substr(nameBegin, cursor - nameBegin);
        CookResult<int64_t> result = std::unexpected(CookError::AttributeExpressionParseFailed);
        
        if (parserMode == ParserMode::CollectOnly)
        {
            identifiers.emplace_back(name);
            result = int64_t{ 1  }; // not evaluated, just return 1!
        }
        else if (parserMode == ParserMode::Default)
        {
            for (const AttrExprSymbol& symbol : symbolTable)
            {
                if (symbol.Name == name)
                {
                    return symbol.Value;
                }
            }

            const CookError error = ReportError(sink,
                                                CookError::AttributeExpressionUnknownSymbol,
                                                "Attribute expression names an unknown symbol");
            result = std::unexpected(error);
        }

        return result;
    }


    bool ExpressionParser::peekIsShift() noexcept
    {
        skipWhitespace();
        return text.compare(cursor, 2u, "<<") == 0 || text.compare(cursor, 2u, ">>") == 0;
    }


    bool ExpressionParser::evaluateComparison(Comparison comparison, int64_t left, int64_t right) noexcept
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


} // namespace

} // namespace lodestone

#ifdef __clang__
#pragma clang diagnostic pop
// NOLINTEND(misc-no-recursion)
#endif
