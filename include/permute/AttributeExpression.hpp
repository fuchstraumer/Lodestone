#pragma once
#ifndef LODESTONE_ATTRIBUTE_EXPRESSION_HPP
#define LODESTONE_ATTRIBUTE_EXPRESSION_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** Evaluates `[ls_*]` attribute expressions and axis constraints. They travel as strings, because Slang folds
 * attribute integers at compile time and our constants bind at link time. */
namespace lodestone
{

class DiagnosticSink;

struct AttrExprSymbol
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
 * Integers are decimal or `0x` hex, with an optional `u` suffix, as in Slang source. */
CookResult<int64_t> EvaluateExpression(std::string_view expression, std::span<const AttrExprSymbol> symbols, DiagnosticSink& sink);

/** @brief Returns the identifiers an expression names, without evaluating it. */
CookResult<std::vector<std::string>> CollectExpressionIdentifiers(std::string_view expression, DiagnosticSink& sink);



} // namespace lodestone

#endif // !LODESTONE_ATTRIBUTE_EXPRESSION_HPP
