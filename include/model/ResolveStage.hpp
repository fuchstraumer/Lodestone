#pragma once
#ifndef LODESTONE_RESOLVE_STAGE_HPP
#define LODESTONE_RESOLVE_STAGE_HPP
#include "CookerErrors.hpp"
#include "ShaderDataSchema.hpp"
#include "compile/RawLibrary.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationTypes.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>


/** Stage 4: evaluates the `[ls_*]` attributes of a `RawVariant`. It names no Slang type, so it tests without a compiler
 * and serves every target language. */
namespace lodestone
{

class DiagnosticSink;

/** @brief The symbols one variant's size expressions may name. The strings must outlive the context. */
struct ResolveContext
{
    std::vector<AttrExprSymbol> Symbols;
};

/** @brief A module's derived constants sorted by name. `RawModule::DerivedConstants` must outlive it. */
struct DerivedConstantTable
{
    std::span<const RawDerivedConstant> Constants;
    std::vector<uint32_t> ByName;
};

/** @brief Runs once per module before any compile, so that a naming error fails early. Two declarations with the same
 * expression are one constant. */
CookResult<DerivedConstantTable> MakeDerivedConstantTable(
    std::span<const RawDerivedConstant> derived_constants,
    std::span<const PermutationAxis> axes,
    std::span<const ExternConstantDefault> extern_defaults,
    DiagnosticSink& sink);

/** @brief Builds one variant's symbol table from the canonical assignment, and evaluates in post-order every derived
 * constant that a size attribute reaches. */
CookResult<ResolveContext> MakeResolveContext(const PermutationAssignment& canonical,
                                              std::span<const ExternConstantDefault> extern_defaults,
                                              const DerivedConstantTable& derived_constants,
                                              const RawVariant& raw,
                                              DiagnosticSink& sink);

/** @brief Returns the axes that a binding's size attributes name, directly or through derived constants. */
CookResult<std::vector<uint32_t>> CollectDeclaredFootprintAxes(const RawVariant& raw,
                                                               size_t binding_index,
                                                               std::span<const PermutationAxis> axes,
                                                               const DerivedConstantTable& derived_constants,
                                                               DiagnosticSink& sink);

CookResult<CompiledVariant> ResolveVariant(const RawVariant& raw,
                                           const ResolveContext& context,
                                           DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_RESOLVE_STAGE_HPP
