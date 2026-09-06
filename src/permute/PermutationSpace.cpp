#include "permute/PermutationSpace.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/ExternConstantScanner.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationValue.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    /** Lets a later extern's default read an earlier one, which is how shaders usually derive them. */
    std::vector<AttrExprSymbol> AsAttrExprSymbols(const std::vector<ExternConstantDefault>& defaults)
    {
        std::vector<AttrExprSymbol> symbols;
        symbols.reserve(defaults.size());

        for (const ExternConstantDefault& entry : defaults)
        {
            symbols.emplace_back(entry.Name, entry.Value);
        }

        return symbols;
    }

    [[nodiscard]] CookError VerifyVariantIndicesAreUnique(const std::vector<VariantDescriptor>& variants)
    {
        auto firstDuplicateIter =
            std::ranges::adjacent_find(variants,
                                       [](const VariantDescriptor& lhs, const VariantDescriptor& rhs)
                                       {
                                           return lhs.Index == rhs.Index;
                                       });

        if (firstDuplicateIter != variants.end()) [[unlikely]]
        {
            // get variant that caused the collision
            const VariantDescriptor& duplicate = *firstDuplicateIter;
            std::println(stderr,
                         "[shader_cooker] two variants share index {}: [{}] collides. The mixed-radix "
                         "encoding and the enumerated set disagree.",
                         duplicate.Index,
                         DescribeAssignment(duplicate.Canonical));
            return CookError::PermutationVariantIndexCollision;
        }
        else [[likely]]
        {
            return CookError::Success;
        }
    }

    [[nodiscard]] std::vector<AttrExprSymbol> SymbolsFromCanonicalAssignment(
        const CanonicalAssignment& assignment)
    {
        std::vector<AttrExprSymbol> symbols;
        symbols.reserve(assignment.size());
        for (const auto& binding : assignment)
        {
            symbols.emplace_back(binding.Axis->Name, PermutationValueToInt64(binding.Value));
        }
        return symbols;
    }

    [[nodiscard]] CookResult<bool> EvaluateActiveWhen(const PermutationAxis& axis,
                                                      const std::vector<AttrExprSymbol>& symbols,
                                                      DiagnosticSink& sink)
    {
        const CookResult<int64_t> result = EvaluateExpression(axis.ActiveWhen, symbols, sink);
        if (!result) [[unlikely]]
        {
            const std::string errStr =
                std::format("Failed to evaluate ActiveWhen expression '{}' for axis '{}'",
                            axis.ActiveWhen,
                            axis.Name);
            return std::unexpected(ReportError(sink, result.error(), errStr));
        }

        return static_cast<bool>(result.value());
    }

    [[nodiscard]] CookResult<bool> CheckRequires(std::ptrdiff_t depth,
                                                 const std::vector<AttrExprSymbol>& symbols,
                                                 const RequireReadyMap& require_ready_at,
                                                 DiagnosticSink& sink)
    {
        const auto iter = require_ready_at.find(depth);
        if (iter == require_ready_at.end())
        {
            return true;
        }

        const std::vector<std::string_view>& requireExpressions = iter->second;
        for (const std::string_view& requireExpr : requireExpressions)
        {
            const CookResult<int64_t> result = EvaluateExpression(requireExpr, symbols, sink);
            if (!result) [[unlikely]]
            {
                const std::string errStr = std::format("Failed to evaluate require expression '{}'", requireExpr);
                return std::unexpected(ReportError(sink, result.error(), errStr));
            }

            if (result.value() == 0)
            {
                return false;
            }
        }

        return true;
    }


} // namespace

PermutationSpace::PermutationSpace(std::string _name,
                                   std::span<const PermutationAxis> _axes,
                                   std::vector<std::string> require_expressions) noexcept
    : name{ std::move(_name) },
      axes{ _axes.begin(), _axes.end() },
      requireExpressions{ std::move(require_expressions) }
{
}

PermutationSpace::PermutationSpace(std::string _name,
                                   std::initializer_list<PermutationAxis> _axes,
                                   std::vector<std::string> require_expressions) noexcept
    : name{ std::move(_name) },
      axes{ _axes },
      requireExpressions{ std::move(require_expressions) }
{
}

std::string_view PermutationSpace::Name() const noexcept
{
    return name;
}

std::span<const PermutationAxis> PermutationSpace::Axes() const noexcept
{
    return axes;
}

std::size_t PermutationSpace::AxisCount() const noexcept
{
    return axes.size();
}

bool PermutationSpace::IsEmpty() const noexcept
{
    return axes.empty();
}

std::span<const std::string> PermutationSpace::RequireExpressions() const noexcept
{
    return requireExpressions;
}

CookResult<VariantSet> PermutationSpace::EnumerateVariants(const size_t max_variant_count, DiagnosticSink& sink) const
{
    // constructing this with ranges/views so we can make it const, which couldn't
    // happen with ye olde for loop. kinda neat.
    const std::unordered_map<std::string_view, std::ptrdiff_t> axisIndexMap = 
        axes |
        std::views::enumerate |
        std::views::transform(
            [](const auto& pair)
            {
                // important: explicitly construct string_view, otherwise the map
                // might construct a view pointing to a temp string copy made here
                auto [index, axis] = pair;
                return std::pair(std::string_view{ axis.Name }, index);
            }) |
        std::ranges::to<std::unordered_map<std::string_view, std::ptrdiff_t>>();
    
    // build require-ready-at-depth map - this helps save some legwork during the already
    // hot recursive enumeration of permutations
    RequireReadyMap requireReadyAt;
    for (const std::string& expr : requireExpressions)
    {
        const auto exprEval = CollectExpressionIdentifiers(expr, sink);
        // errors should've already been handled in validation step, done earlier
        // skip error checking bc we *know* the expressions have already been validated

        // find the deepest axis index referenced by this require expression, and store that
        std::ptrdiff_t deepest = 0;
        for (const std::string& identifier : *exprEval)
        {
            deepest = std::max(deepest, axisIndexMap.at(identifier));
        }

        requireReadyAt[deepest].push_back(expr);
    }

    PermutationAssignment partial;
    std::vector<VariantDescriptor> descriptors;
    const CookError walkResult = expandFrom(0, partial, requireReadyAt, descriptors, max_variant_count, sink);
    if (!walkResult)
    {
        return std::unexpected(walkResult);
    }

    VariantSet variantSet;
    variantSet.Space = this;
    variantSet.SpaceSize = ComputeVariantSpaceSize();
    variantSet.Variants = std::move(descriptors);

    // sort first, because then uniqueness check can assume the indices are in order
    std::ranges::sort(variantSet.Variants, std::ranges::less{}, &VariantDescriptor::Index);

    const CookError verifyUnique = VerifyVariantIndicesAreUnique(variantSet.Variants);
    if (verifyUnique != CookError::Success)
    {
        return std::unexpected(verifyUnique);
    }

    return variantSet;
}

// Canonicalization is another expansion: for every axis in the space, we need to find the concrete
// value of it bound in *this* assignment. If the axis is not present in the assignment, we will
// retrieve the default value (the first value) of the axis. This equalizes each assignment to the
// same length, and allows us to compute a unique index for each assignment.
CanonicalAssignment PermutationSpace::CanonicalizeAssignment(const PermutationAssignment& assignment) const
{
    auto findBinding = [&assignment](const PermutationAxis& axis) -> PermutationBinding
    {
        auto foundIter = std::ranges::find(assignment, &axis, &PermutationBinding::Axis);
        const PermutationValue value = foundIter != assignment.end() ? foundIter->Value : axis.GetDefault();
        return PermutationBinding{ .Axis = &axis, .Value = value };
    };
    auto canonical = axes | std::views::transform(findBinding) | std::ranges::to<PermutationAssignment>();
    return CanonicalAssignment{ std::move(canonical) };
}

int32_t PermutationSpace::ComputeVariantIndex(const CanonicalAssignment& canonical) const
{
    std::ptrdiff_t index = 0;

    for (size_t i = 0; i < axes.size(); ++i)
    {
        // in canonical, the i-th element corresponds to the i-th axis in the space.
        const PermutationValue& value = canonical[i].Value;
        const std::span<const PermutationValue> values = axes[i].GetValues();
        const auto found = std::ranges::find(values, value);
        const std::ptrdiff_t valueIndex = std::distance(values.begin(), found);
        index = (index * std::ssize(values)) + valueIndex;
    }

    return static_cast<int32_t>(index);
}

int32_t PermutationSpace::ComputeVariantSpaceSize() const noexcept
{
    int64_t size = 1;

    for (const auto& axis : axes)
    {
        size *= axis.NumValues();
    }

    return static_cast<int32_t>(size);
}

CookError PermutationSpace::VerifyAxisNamesAreDeclared(std::span<const std::string_view> source_texts,
                                                       std::string_view module_name,
                                                       DiagnosticSink& sink) const
{
    int32_t undeclaredCount = 0;

    for (const PermutationAxis& axis : axes)
    {
        bool declared = false;
        for (const std::string_view source : source_texts)
        {
            if (DeclaresExternConstantNamed(source, axis.Name))
            {
                declared = true;
                break;
            }
        }

        if (!declared)
        {
            ++undeclaredCount;
            const std::string warningStr =
                std::format("Axis '{}' has no matching `extern static const` declaration "
                            "in module {}. Slang links this symbol, nothing references it, the shader "
                            "keeps its default, and every variant cooks identical output.",
                            axis.Name,
                            module_name);
            ReportWarning(sink, warningStr);
        }
    }

    if (undeclaredCount > 0)
    {
        return CookError::PermutationAxisNotDeclared;
    }

    return CookError::Success;
}

CookError PermutationSpace::ValidateConstraints(DiagnosticSink& sink) const
{
    // since we'll want to use indices to refer to axes, this makes it easier
    const std::vector<std::string_view> axesNames = axes |
                                                    std::views::transform(&PermutationAxis::Name) |
                                                    std::ranges::to<std::vector<std::string_view>>();

    const CookError activeWhenValidationResult = validateActiveWhen(axesNames, sink);
    if (!activeWhenValidationResult)
    {
        return activeWhenValidationResult;
    }

    const CookError requiresValidationResult = validateRequires(axesNames, sink);
    if (!requiresValidationResult)
    {
        return requiresValidationResult;
    }

    return CookError::Success;
}

void PermutationSpace::ReportUndrivenExternConstants(std::span<const std::string_view> source_texts,
                                                     std::string_view module_name,
                                                     DiagnosticSink& sink) const
{
    auto axesView = axes | std::views::transform(&PermutationAxis::Name);
    std::unordered_set<std::string_view> axesNames(axesView.begin(), axesView.end());
    std::vector<ExternConstantDeclaration> undriven;
    auto filterUndriven = [&axesNames](const ExternConstantDeclaration& decl)
    {
        return !axesNames.contains(decl.Name);
    };

    for (const std::string_view source : source_texts)
    {
        std::vector<ExternConstantDeclaration> declared = ScanExternConstants(source);
        undriven.append_range(declared | std::views::filter(filterUndriven) | std::views::as_rvalue);
    }

    // build report string
    std::string report;
    for (const ExternConstantDeclaration& decl : undriven)
    {
        const std::string warningStr =
            std::format("extern constant '{}' in module {} is declared extern but no axis "
                        "drives it. It keeps its declared default in every variant.",
                        decl.Name,
                        module_name);
        ReportWarning(sink, warningStr);
    }
}

CookResult<std::vector<ExternConstantDefault>> PermutationSpace::CollectUndrivenExternDefaults(
    std::span<const std::string_view> source_texts, DiagnosticSink& sink) const
{
    std::vector<ExternConstantDefault> defaults;

    auto axesView = axes | std::views::transform(&PermutationAxis::Name);
    std::unordered_set<std::string_view> axesNames(axesView.begin(), axesView.end());

    std::vector<ExternConstantDeclaration> undriven;
    auto filterUndriven = [&axesNames](const ExternConstantDeclaration& decl)
    {
        return !axesNames.contains(decl.Name);
    };
    for (const std::string_view source : source_texts)
    {
        auto declared = ScanExternConstants(source);
        undriven.append_range(declared | std::views::filter(filterUndriven) | std::views::as_rvalue);
    }

    defaults.reserve(undriven.size());

    for (const auto& [constName, valueText] : undriven)
    {
        const std::string_view trimmed = TrimWhitespace(valueText);
        if (trimmed == "true" || trimmed == "false")
        {
            defaults.emplace_back(std::string{ constName }, trimmed == "true" ? 1 : 0);
            continue;
        }

        const std::vector<AttrExprSymbol> known = AsAttrExprSymbols(defaults);
        const CookResult<int64_t> value = EvaluateExpression(trimmed, known, sink);
        if (!value)
        {
            std::println(stderr,
                         "[shader_cooker] could not read the default of extern constant '{}' from "
                         "'{}'. A size expression naming it would silently disagree with the shader.",
                         constName,
                         trimmed);
            return std::unexpected(value.error());
        }

        defaults.emplace_back(std::string{ constName }, value.value());
    }

    return defaults;
}

CookError PermutationSpace::validateActiveWhen(const std::vector<std::string_view>& axes_names,
                                               DiagnosticSink& sink) const
{
    for (std::ptrdiff_t i = 0; std::cmp_less(i, axes.size()); ++i)
    {
        const PermutationAxis& curr = axes[static_cast<size_t>(i)];
        if (curr.ActiveWhen.empty())
        {
            continue;
        }

        auto activeWhenResult = CollectExpressionIdentifiers(curr.ActiveWhen, sink);
        if (!activeWhenResult)
        {
            return CookError::PermutationConstraintInvalidExpression;
        }

        const std::vector<std::string>& identifiers = *activeWhenResult;
        // first error-out case: is there a discrepancy between the identifiers and the declared axes?
        for (const std::string& identifier : identifiers)
        {
            auto iter = std::ranges::find(axes_names, identifier);
            if (iter == axes_names.end())
            {
                return ReportError(
                    sink,
                    CookError::PermutationConstraintUnknownSymbol,
                    std::format("Axis '{}' used unknown symbol {} in ActiveWhen", curr.Name, identifier));
            }

            const std::ptrdiff_t index = std::distance(axes_names.begin(), iter);
            if (index >= i)
            {
                const std::string_view& forwardRefName = axes_names[static_cast<size_t>(index)];
                return ReportError(
                    sink,
                    CookError::PermutationConstraintForwardReference,
                    std::format("Axis '{}' has a forward reference to axis '{}'", curr.Name, forwardRefName));
            }

            const PermutationAxis& referenced = axes[static_cast<size_t>(index)];
            // check to see if the referenced axis has a non-empty ActiveWhen: explain the default
            // substitution, that this axis may be made inactive by another axis's ActiveWhen condition.
            if (!referenced.ActiveWhen.empty())
            {
                const std::string_view& referencedName = referenced.Name;
                ReportWarning(
                    sink,
                    std::format("Axis '{}' references axis '{}' which has a non-empty ActiveWhen condition",
                                curr.Name,
                                referencedName));
            }
        }
    }

    return CookError::Success;
}

CookError PermutationSpace::validateRequires(const std::vector<std::string_view>& axes_names,
                                             DiagnosticSink& sink) const
{
    for (const std::string& requireExpr : requireExpressions)
    {
        const auto exprIdentifiers = CollectExpressionIdentifiers(requireExpr, sink);
        if (!exprIdentifiers)
        {
            return ReportError(sink,
                               CookError::PermutationConstraintInvalidExpression,
                               std::format("Invalid Requires expression '{}'", requireExpr));
        }

        const std::vector<std::string>& exprIdentifiersRef = *exprIdentifiers;
        // an empty require expression is just a constant: it shouldn't be considered valid.
        if (exprIdentifiersRef.empty())
        {
            return ReportError(sink,
                               CookError::PermutationConstraintEmptyRequireExpression,
                               std::format("Requires expression '{}' is empty", requireExpr));
        }

        for (const std::string& identifier : exprIdentifiersRef)
        {
            auto foundIter = std::ranges::find(axes_names, identifier);
            if (foundIter == axes_names.end())
            {
                return ReportError(
                    sink,
                    CookError::PermutationConstraintUnknownSymbol,
                    std::format("Requires expression '{}' uses unknown symbol {}", requireExpr, identifier));
            }

            const std::ptrdiff_t index = std::distance(axes_names.begin(), foundIter);
            const PermutationAxis& referenced = axes[static_cast<size_t>(index)];
            if (!referenced.ActiveWhen.empty())
            {
                ReportWarning(
                    sink,
                    std::format("Requires expression '{}' references axis '{}' which has a non-empty ActiveWhen condition",
                                requireExpr,
                                referenced.Name));
            }
        }
    }
    return CookError::Success;
}

//NOLINTBEGIN(misc-no-recursion)
CookError PermutationSpace::expandFrom(std::ptrdiff_t depth,
                                       PermutationAssignment& partial,
                                       const RequireReadyMap& require_ready_at,
                                       std::vector<VariantDescriptor>& expanded,
                                       const size_t max_variant_count,
                                       DiagnosticSink& sink) const
{
    // canonicalize the current partial assignment here, since it will be 
    // used directly below, then to make symbol table for checks
    CanonicalAssignment canonical = CanonicalizeAssignment(partial);

    if (std::cmp_equal(depth, axes.size()))
    {
        // completed a full permutation assignment, add it to the expanded list
        const int32_t index = ComputeVariantIndex(canonical);
        expanded.emplace_back(PermutationAssignment{ partial }, std::move(canonical), index);
        if ((max_variant_count > 0) && (expanded.size() >= max_variant_count)) [[unlikely]]
        {
            const std::string errorMessage = std::format("Permutation variant budget exceeded (max {} variants)", max_variant_count);
            return ReportError(
                sink,
                CookError::PermutationVariantBudgetExceeded,
                errorMessage);
        }
        return CookError::Success;
    }

    const PermutationAxis& axis = axes[static_cast<size_t>(depth)];

    bool active = true;
    // both ActiveWhen and Require checks will need the symbols, get them once
    const std::vector<AttrExprSymbol> symbols = SymbolsFromCanonicalAssignment(canonical);

    if (!axis.ActiveWhen.empty())
    {
        const CookResult<bool> activeResult = EvaluateActiveWhen(axis, symbols, sink);
        if (!activeResult) [[unlikely]]
        {
            return activeResult.error();
        }

        active = activeResult.value();

    }

    if (!active)
    {
        // axis skipped: will be canonicalized to default value
        const CookResult<bool> requireResult = CheckRequires(depth, symbols, require_ready_at, sink);
        if (!requireResult) [[unlikely]]
        {
            return requireResult.error();
        }

        if (!requireResult.value())
        {
            // prune the subtree
            return CookError::Success;
        }

        // continue expanding the next axis
        return expandFrom(depth + 1, partial, require_ready_at, expanded, max_variant_count, sink);
    }

    // axis is active: expand partial to include all possible values of this axis
    for (const PermutationValue& value : axis.GetValues())
    {
        partial.emplace_back(&axis, value);
        // need to rebuild symbol table since partial assignment has changed
        const CanonicalAssignment innerCanonical = CanonicalizeAssignment(partial);
        const std::vector<AttrExprSymbol> innerSymbols = SymbolsFromCanonicalAssignment(innerCanonical);
        const CookResult<bool> keepAxis = CheckRequires(depth, innerSymbols, require_ready_at, sink);
        if (!keepAxis) [[unlikely]]
        {
            partial.pop_back();
            return keepAxis.error();
        }

        if (keepAxis.value())
        {
            const CookError subtree = expandFrom(depth + 1, partial, require_ready_at, expanded, max_variant_count, sink);
            if (!subtree)
            {
                partial.pop_back();
                return subtree;
            }
        }

        partial.pop_back();
    }

    return CookError::Success;
}
//NOLINTEND(misc-no-recursion)

} // namespace lodestone
