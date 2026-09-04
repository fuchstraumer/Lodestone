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
#include <memory>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

        for (size_t i = 0; i < assignment.size(); ++i)
        {
            const PermutationBinding& binding = assignment[i];
            symbols.emplace_back(binding.Axis->Name, PermutationValueToInt64(binding.Value));
        }

        return symbols;
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

// perform CCSP with classic backtracking, but skip any axis whose parent is not active
// this is a somewhat embarassing amount of commenting for me, but I have not done constraint satisfaction
// formally *ever* before, and I want to make sure I understand it. these are notes for me. i am not a learned
// woman
CookResult<std::vector<PermutationAssignment>> PermutationSpace::EnumerateActiveCombinations(
    DiagnosticSink& sink) const
{
    // A module with no registered space enumerates to the one empty assignment, so there is no first
    // axis to size against. `partials` is replaced by `expanded` on every pass anyway.
    std::vector<PermutationAssignment> partials{ PermutationAssignment{} };
    for (const PermutationAxis& axis : axes)
    {
        // Despite having to do recursive work here, we can at least reserve the right amount of space. I
        // guess.
        std::vector<PermutationAssignment> expanded;
        expanded.reserve(partials.size() * static_cast<size_t>(axis.NumValues()));
        // For the current axis, we need to traverse every partial assignment (incomplete combination) we have
        // thus far and expand/evaluate it for the current axis. This is a breadth-first search of the
        // combination space, and we will continue to expand the partials until we have a complete assignment
        // for every axis in the space.
        for (const PermutationAssignment& partial : partials)
        {
            bool active = true;
            // Evaluate the ActiveWhen expression on the axis, if it exists, to determine if the axis should
            // be active.
            if (!axis.ActiveWhen.empty())
            {
                // canonicalize the current partial assignment to evaluate the ActiveWhen expression
                // correctly, which works like default-substitution for missing values (i.e, if this value
                // depends on an axis that's previously deactivated, the canonicalization will provide a
                // default value for it)
                const CanonicalAssignment canonical = CanonicalizeAssignment(partial);
                const std::vector<AttrExprSymbol> symbols = SymbolsFromCanonicalAssignment(canonical);
                // now evaluate the actual expression
                const CookResult<int64_t> isActive = EvaluateExpression(axis.ActiveWhen, symbols, sink);
                if (!isActive) [[unlikely]]
                {
                    return std::unexpected(isActive.error());
                }
                else [[likely]]
                {
                    // if active, the result should be != 0
                    active = static_cast<bool>(isActive.value());
                }
            }

            if (!active)
            {
                // axis is inactive, so we just carry forward the current partial without expanding this axis
                // canonicalization will fill it with the default later, as needed
                expanded.emplace_back(partial);
                continue;
            }

            // Expand the current axis, evaluating/instantiating it for each of it's values
            // We store the axis (the abstract half) and the *value* (the concrete half). This
            // defines a *Binding* or unique instantiation of the axis for this current partial.
            // (thus a binding is just {abstract [axis*], concrete [value]})
            for (const PermutationValue& value : axis.GetValues())
            {
                // at each depth, we take the current partial as our starting point (as that's how
                // breadth-first constraint satisfaction like this works best for our data)
                PermutationAssignment next = partial;
                next.push_back(PermutationBinding{ .Axis = &axis, .Value = value });
                // note: we need expanded separate as we are using partials as the source of truth for
                // the current depth, and we don't want to modify it while iterating. the overwrite
                // has to come at the end
                expanded.push_back(std::move(next));
            }
        }

        // now that we're done reading partials, we can overwrite it with the expanded set of partials at the
        // current depth... to use at the next depth.
        partials = std::move(expanded);
    }

    return partials;
}

CookResult<VariantSet> PermutationSpace::EnumerateVariants(DiagnosticSink& sink) const
{
    CookResult<std::vector<PermutationAssignment>> enumerateActiveResult = EnumerateActiveCombinations(sink);
    if (!enumerateActiveResult)
    {
        return std::unexpected(enumerateActiveResult.error());
    }

    std::vector<PermutationAssignment> active{ std::move(enumerateActiveResult.value()) };

    VariantSet variantSet;
    variantSet.Space = this;
    variantSet.SpaceSize = ComputeVariantSpaceSize();
    variantSet.Variants.reserve(active.size());

    for (PermutationAssignment& assignment : active)
    {
        CanonicalAssignment canonical = CanonicalizeAssignment(assignment);

        if (!requireExpressions.empty())
        {
            const std::vector<AttrExprSymbol> symbols = SymbolsFromCanonicalAssignment(canonical);
            bool requirementSatisfied = true;

            for (const std::string& requireExpression : requireExpressions)
            {
                const CookResult<int64_t> exprResult = EvaluateExpression(requireExpression, symbols, sink);
                if (!exprResult) [[unlikely]]
                {
                    return std::unexpected(exprResult.error());
                }

                // require expression evaluated to false, meaning the current assignment does not satisfy this
                // requirement
                if (exprResult.value() == 0)
                {
                    requirementSatisfied = false;
                    break;
                }
            }

            // if the requirement was not satisfied, skip this assignment
            // (making it a "hole" in this slot in the variant space)
            if (!requirementSatisfied)
            {
                continue;
            }
        }

        const int32_t index = ComputeVariantIndex(canonical);
        variantSet.Variants.emplace_back(std::move(assignment), std::move(canonical), index);
    }

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
    auto axesNames = axes | std::views::transform(&PermutationAxis::Name);

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
            auto iter = std::ranges::find(axesNames, identifier);
            if (iter == axesNames.end())
            {
                return ReportError(
                    sink,
                    CookError::PermutationConstraintUnknownSymbol,
                    std::format("Axis '{}' used unknown symbol {} in ActiveWhen", curr.Name, identifier));
            }

            const std::ptrdiff_t index = std::distance(axesNames.begin(), iter);
            if (index >= i)
            {
                const std::string& forwardRefName = axesNames[index];
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
                const std::string& referencedName = referenced.Name;
                ReportWarning(
                    sink,
                    std::format("Axis '{}' references axis '{}' which has a non-empty ActiveWhen condition",
                                curr.Name,
                                referencedName));
            }
        }
    }

    // next loop: validate Requires expressions
    // no forward-reference rule with Requires, but the checks are otherwise similar to ActiveWhen
    for (const std::string& requireExpr : requireExpressions)
    {
        const auto exprIdentifiers = CollectExpressionIdentifiers(requireExpr, sink);
        if (!exprIdentifiers)
        {
            return ReportError(sink,
                               CookError::PermutationConstraintInvalidExpression,
                               std::format("Invalid Requires expression '{}'", requireExpr));
        }

        for (const std::string& identifier : *exprIdentifiers)
        {
            auto foundIter = std::ranges::find(axesNames, identifier);
            if (foundIter == axesNames.end())
            {
                return ReportError(
                    sink,
                    CookError::PermutationConstraintUnknownSymbol,
                    std::format("Requires expression '{}' uses unknown symbol {}", requireExpr, identifier));
            }

            const std::ptrdiff_t index = std::distance(axesNames.begin(), foundIter);
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

} // namespace lodestone
