#pragma once
#ifndef LODESTONE_PERMUTATION_SPACE_HPP
#define LODESTONE_PERMUTATION_SPACE_HPP
#include "CookerErrors.hpp"
#include "compile/SymbolTable.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "VariantKey.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lodestone
{

class PermutationSpace;
class DiagnosticSink;
struct TargetCookPolicy;
using RequireReadyMap = std::unordered_map<std::ptrdiff_t, std::vector<std::string_view>>;
using AxisValueOverrideMap = std::unordered_map<std::ptrdiff_t, std::vector<PermutationValue>>;


/** One variant's identity. `Active` holds the axes the variant uses, and drives the linker. `Canonical` holds every
 * axis, with the first value for disabled ones, and drives the stable key only. */
struct VariantDescriptor
{
    PermutationAssignment Active;
    CanonicalAssignment Canonical;
    VariantKey Key{ 0 };
    uint64_t Index{ 0 };
};

/** A space expanded. `SpaceSize` includes the holes that disabled axes leave. */
struct VariantSet
{
    std::vector<VariantDescriptor> Variants;
    uint64_t SpaceSize{ 0u };
};

class PermutationSpace
{
public:
    PermutationSpace(std::vector<PermutationAxis> _axes,
                     std::vector<std::string> require_expressions = {}) noexcept;
    ~PermutationSpace() noexcept = default;

    /** Bindings point into the owned axes, so a copy would aim them at another object. A move keeps the buffer. */
    PermutationSpace(const PermutationSpace&) = delete;
    PermutationSpace& operator=(const PermutationSpace&) = delete;
    PermutationSpace(PermutationSpace&&) noexcept = default;
    PermutationSpace& operator=(PermutationSpace&&) noexcept = default;

    [[nodiscard]] std::span<const PermutationAxis> Axes() const noexcept;
    [[nodiscard]] std::size_t AxisCount() const noexcept;
    [[nodiscard]] bool IsEmpty() const noexcept;
    [[nodiscard]] std::span<const std::string> RequireExpressions() const noexcept;

    [[nodiscard]] CookResult<VariantSet> EnumerateVariants(const TargetCookPolicy& policy,
                                                           DiagnosticSink& sink) const;
    [[nodiscard]] CanonicalAssignment CanonicalizeAssignment(const PermutationAssignment& assignment) const;
    [[nodiscard]] VariantKey ComputeVariantKey(const CanonicalAssignment& canonical) const;
    [[nodiscard]] uint64_t ComputeVariantSpaceSize() const noexcept;
    [[nodiscard]] CookError ValidateConstraints(DiagnosticSink& sink) const;
    /** Reports `extern` constants that no axis drives and a source reads: they keep their default in every variant. */
    void ReportUndrivenExternConstants(std::string_view module_name,
                                       const SymbolTable& symbol_table,
                                       DiagnosticSink& sink) const;
    [[nodiscard]] CookResult<std::vector<ExternConstantDefault>> CollectUndrivenExternDefaults(
        std::string_view module_name, const SymbolTable& symbol_table, DiagnosticSink& sink) const;

private:
    /** @brief Skips the constants that no source reads, so that no default of theirs is needed. */
    [[nodiscard]] std::vector<ExternConstantDeclaration> readUndrivenExternConstants(std::string_view module_name,
                                                                                     const SymbolTable& symbol_table) const;
    CookError validateActiveWhen(const std::vector<std::string_view>& axes_names, DiagnosticSink& sink) const;
    CookError validateRequires(const std::vector<std::string_view>& axes_names, DiagnosticSink& sink) const;

    CookError expandFrom(std::ptrdiff_t depth,
                         PermutationAssignment& partial,
                         const RequireReadyMap& require_ready_at,
                         const TargetCookPolicy& policy,
                         const AxisValueOverrideMap& axis_value_overrides,
                         std::vector<VariantDescriptor>& expanded,
                         DiagnosticSink& sink) const;
    std::vector<PermutationAxis> axes;
    std::vector<std::string> requireExpressions;
};

} // namespace lodestone

#endif // !LODESTONE_PERMUTATION_SPACE_HPP
