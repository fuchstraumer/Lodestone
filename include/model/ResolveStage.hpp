#pragma once
#ifndef LODESTONE_RESOLVE_STAGE_HPP
#define LODESTONE_RESOLVE_STAGE_HPP
#include "CookerErrors.hpp"
#include "permute/PermutationSpace.hpp"
#include "compile/RawLibrary.hpp"
#include "ShaderDataSchema.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationTypes.hpp"
#include <span>
#include <vector>

/**Pipeline Stage 4. Turns what the compiler said into what the runtime needs.
 *
 * This file names no Slang type, and it must not gain one. That is the whole point of the split: the
 * arithmetic that decides every buffer size in the engine is a pure function of a `RawVariant` and a
 * symbol table, so it has a test that runs in milliseconds and needs no compiler, and a second target
 * language reuses it unchanged.
 *
 * The symbol table is the only input that does not come from stage 3, and it has three parts:
 * - The axis values, from the variant's canonical assignment.
 * - The `extern static const` constants that no axis drives, at their declared defaults. Only Slang knows
 *   those, so stage 3 stores them in `RawModule::ExternDefaults`.
 * - The derived constants (module-scope `static const` values computed from the two parts above), evaluated
 *   per variant from `RawModule::DerivedConstants`. Only the derived constants that a size attribute of the
 *   variant reaches are evaluated, so that a constant we cannot evaluate (a `float`, a function call) is
 *   harmless until an attribute uses it. */
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

/**@brief The derived constants of one module, sorted by name so that each variant finds a name with a binary
 * search.
 *
 * @note `Constants` points into `RawModule::DerivedConstants`, so that module must outlive the table. */
struct DerivedConstantTable
{
    std::span<const RawDerivedConstant> Constants;
    // indices into `Constants`, one for each distinct name, sorted by name
    std::vector<uint32_t> ByName;
};

/**@brief Builds the name index of one module's derived constants. This runs once for each module, before any
 * variant compiles, so that a naming error fails the cook before the expensive part.
 *
 * Returns `DerivedConstantShadowsSymbol` when a derived constant has the name of an axis or of an undriven
 * extern constant, and `DerivedConstantAmbiguous` when two declarations of one name have different
 * expressions. Two declarations with the same expression are one constant. */
CookResult<DerivedConstantTable> MakeDerivedConstantTable(std::span<const RawDerivedConstant> derived_constants,
                                                          std::span<const PermutationAxis> axes,
                                                          std::span<const ExternConstantDefault> extern_defaults,
                                                          DiagnosticSink& sink);

/**@brief Builds the symbol table for one variant, and evaluates every derived constant that a size attribute
 * of `raw` reaches.
 *
 * The derived constants are visited depth first. Each one is evaluated after every derived constant that its
 * expression uses (post-order), so its dependencies are already in the table. Returns `DerivedConstantCycle`
 * when a constant reaches itself, and `DerivedConstantUnreadable` when a reached constant does not parse or
 * evaluate.
 *
 * @note The canonical assignment is used rather than the active one, so every axis is nameable even when a
 * dependent axis is off. A disabled axis contributes nothing to the shader, so an expression that
 * reads it was already independent of the value. `MakeDerivedConstantTable` rejects a derived constant
 * with the name of an axis or an extern constant, so every name in the symbol table is unique. */
CookResult<ResolveContext> MakeResolveContext(const PermutationAssignment& canonical,
                                              std::span<const ExternConstantDefault> extern_defaults,
                                              const DerivedConstantTable& derived_constants,
                                              const RawVariant& raw,
                                              DiagnosticSink& sink);

/** Takes the RawVariant - "raw" here meaning just carrying our meta-annotations - and evaluates
 *  them to populate the `CompiledVariant` with resolved resource footprints and other derived information. */
CookResult<CompiledVariant> ResolveVariant(const RawVariant& raw, const ResolveContext& context, DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_RESOLVE_STAGE_HPP
