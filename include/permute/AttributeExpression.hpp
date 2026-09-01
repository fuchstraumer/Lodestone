#pragma once
#ifndef LODESTONE_ATTRIBUTE_EXPRESSION_HPP
#define LODESTONE_ATTRIBUTE_EXPRESSION_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <span>
#include <string_view>

/**Evaluates the expression carried by a lodestone attribute declared in Slang source code.
 * The expression travels as a string because integral attribute expressions fold at compile time,
 * but all of our attribute expressions are intended to be evaluated at link-time. Leaving it as a
 * string allows it to pass through untouched, so that we can evaluate it and substitute values
 * ourselves as part of cook.
 * 
 * This used to be `SizeExpression.hpp`, as we only used this for sizing resources. Now it also
 * evaluates expressions for permutation and constraint axes, so it's more general-use.
 *
 * Nothing here knows about Slang or about the permutation types. It takes a string and a symbol
 * table, and it is therefore testable on its own. */
namespace lodestone
{

struct SizeSymbol
{
    std::string_view Name;
    int64_t Value{ 0 };
};

/** Grammar, lowest precedence first:
 *     logical    := comparison (( '&&' | '||' ) comparison)*
 *     comparison := shift (( '==' | '!=' | '<' | '<=' | '>' | '>=' ) shift)* 
 *     shift      := sum (( '<<' | '>>' ) sum)*
 *     sum        := product (( '+' | '-' ) product)*
 *     product    := unary (( '*' | '/' | '%' ) unary)*
 *     unary      := ('!' | '-') unary | primary
 *     primary    := integer | identifier | '(' logical ')'
 *
 * Integers are decimal or `0x` hexadecimal, with an optional `u` or `U` suffix so an expression can
 * be copied out of Slang source unchanged. */
CookResult<int64_t> EvaluateExpression(std::string_view expression, std::span<const SizeSymbol> symbols);

} // namespace lodestone

#endif // !LODESTONE_ATTRIBUTE_EXPRESSION_HPP
