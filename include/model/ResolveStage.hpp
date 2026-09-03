#pragma once
#ifndef LODESTONE_RESOLVE_STAGE_HPP
#define LODESTONE_RESOLVE_STAGE_HPP
#include "CookerErrors.hpp"
#include "permute/PermutationSpace.hpp"
#include "compile/RawLibrary.hpp"
#include "ShaderDataSchema.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAssignment.hpp"
#include <span>
#include <vector>

/**Pipeline Stage 4. Turns what the compiler said into what the runtime needs.
 *
 * This file names no Slang type, and it must not gain one. That is the whole point of the split: the
 * arithmetic that decides every buffer size in the engine is a pure function of a `RawVariant` and a
 * symbol table, so it has a test that runs in milliseconds and needs no compiler, and a second target
 * language reuses it unchanged.
 *
 * The symbol table is the only input that does not come from stage 3, and it has two halves. The axis
 * values come from the variant's canonical assignment. The `extern static const` constants that no
 * axis drives keep their declared defaults, and only Slang knows those, so stage 3 carries them out in
 * `RawModule::ExternDefaults`. */
namespace lodestone
{

class DiagnosticSink;

/**@brief The symbols one variant's size expressions may name.
 *
 * @note `Symbols` holds `std::string_view` values, so the strings it points at must outlive the context.*/
struct ResolveContext
{
    std::vector<AttrExprSymbol> Symbols;
};

/**@brief Builds the symbol table for one variant.
 *
 * @note The canonical assignment is used rather than the active one, so every axis is nameable even when a
 * dependent axis is off. A disabled axis contributes nothing to the shader, so an expression that
 * reads it was already independent of the value. The undriven externs come first, so an axis of the
 * same name would win, though the two sets are disjoint by construction. */
ResolveContext MakeResolveContext(const PermutationAssignment& canonical,
                                  std::span<const ExternConstantDefault> extern_defaults);

/** Takes the RawVariant - "raw" here meaning just carrying our meta-annotations - and evaluates
 *  them to populate the `CompiledVariant` with resolved resource footprints and other derived information. */
CookResult<CompiledVariant> ResolveVariant(const RawVariant& raw, const ResolveContext& context, DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_RESOLVE_STAGE_HPP
