#include "permute/PermutationSpace.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TransparentHash.hpp"
#include "VariantKey.hpp"
#include "compile/SymbolTable.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationTypes.hpp"
#include "permute/PermutationValue.hpp"
#include "permute/PolicyDocument.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <magic_enum/magic_enum.hpp>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    std::vector<AttrExprSymbol> AsAttrExprSymbols(const std::vector<ExternConstantDefault>& defaults);
    [[nodiscard]] CookError VerifyVariantKeysAreUnique(const std::vector<VariantDescriptor>& variants);
    [[nodiscard]] std::vector<AttrExprSymbol> SymbolsFromCanonicalAssignment(
        const CanonicalAssignment& assignment);
    [[nodiscard]] CookResult<bool> EvaluateActiveWhen(const PermutationAxis& axis,
                                                      const std::vector<AttrExprSymbol>& symbols,
                                                      DiagnosticSink& sink);
    [[nodiscard]] CookResult<bool> CheckRequires(std::ptrdiff_t depth,
                                                 const std::vector<AttrExprSymbol>& symbols,
                                                 const RequireReadyMap& require_ready_at,
                                                 DiagnosticSink& sink); 

} // namespace

PermutationSpace::PermutationSpace(std::vector<PermutationAxis> _axes,
                                   std::vector<std::string> require_expressions) noexcept
    : axes{ std::move(_axes) },
      requireExpressions{ std::move(require_expressions) }
{
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

CookResult<VariantSet> PermutationSpace::EnumerateVariants(const TargetCookPolicy& policy, DiagnosticSink& sink) const
{
    using AxisIndexMapType = std::unordered_map<std::string_view, std::ptrdiff_t, TransparentStringHash, std::equal_to<>>;
    const AxisIndexMapType axisIndexMap = 
        axes |
        std::views::enumerate |
        std::views::transform(
            [](const auto& pair)
            {
                // an explicit string_view, or the map could view a temporary copy
                auto [index, axis] = pair;
                return std::pair(std::string_view{ axis.Name }, index);
            }) |
        std::ranges::to<AxisIndexMapType>();
    
    RequireReadyMap requireReadyAt;
    for (const std::string& expr : requireExpressions)
    {
        const auto exprEval = CollectExpressionIdentifiers(expr, sink);

        std::ptrdiff_t deepest = 0;
        for (const std::string& identifier : *exprEval)
        {
            deepest = std::max(deepest, axisIndexMap.at(identifier));
        }

        requireReadyAt[deepest].push_back(expr);
    }

    AxisValueOverrideMap axisValueOverrides;
    for (const AxisCookValues& axisValues : policy.CookValues)
    {
        const auto iter = axisIndexMap.find(axisValues.Axis);
        if (iter != axisIndexMap.end())
        {
            const std::ptrdiff_t axisIndex = iter->second;
            axisValueOverrides[axisIndex] = axisValues.Values;
        }
    }

    PermutationAssignment partial;
    std::vector<VariantDescriptor> descriptors;
    const CookError walkResult = expandFrom(0, partial, requireReadyAt, policy, axisValueOverrides, descriptors, sink);
    if (!walkResult)
    {
        return std::unexpected(walkResult);
    }

    VariantSet variantSet;
    variantSet.SpaceSize = ComputeVariantSpaceSize();
    if (variantSet.SpaceSize == std::numeric_limits<std::underlying_type_t<VariantKey>>::max())
    {
        return std::unexpected(CookError::PermutationKeySpaceTooLarge);
    }
    variantSet.Variants = std::move(descriptors);

    std::ranges::sort(variantSet.Variants, std::ranges::less{}, &VariantDescriptor::Key);

    const CookError verifyUnique = VerifyVariantKeysAreUnique(variantSet.Variants);
    if (verifyUnique != CookError::Success)
    {
        return std::unexpected(verifyUnique);
    }
    for (uint64_t i = 0; std::cmp_less(i, variantSet.Variants.size()); ++i)
    {
        variantSet.Variants[i].Index = i;
    }


    return variantSet;
}

// fills every axis the assignment lacks with its canonical value, so every assignment has one length
CanonicalAssignment PermutationSpace::CanonicalizeAssignment(const PermutationAssignment& assignment) const
{
    auto findBinding = [&assignment](const PermutationAxis& axis) -> PermutationBinding
    {
        auto foundIter = std::ranges::find(assignment, &axis, &PermutationBinding::Axis);
        const PermutationValue value = foundIter != assignment.end() ? foundIter->Value : axis.GetCanonicalValue();
        return PermutationBinding{ .Axis = &axis, .Value = value };
    };
    auto canonical = axes | std::views::transform(findBinding) | std::ranges::to<PermutationAssignment>();
    return CanonicalAssignment{ std::move(canonical) };
}

VariantKey PermutationSpace::ComputeVariantKey(const CanonicalAssignment& canonical) const
{  
    std::vector<uint32_t> valueIndices(axes.size());
    std::vector<uint32_t> radices(axes.size());

    for (size_t i = 0; i < axes.size(); ++i)
    {
        const PermutationValue& actualValue = canonical[i].Value;
        const auto& values = axes[i].GetValues();
        
        valueIndices[i] = static_cast<uint32_t>(std::distance(values.begin(),
                                                              std::ranges::find(values, actualValue)));
        radices[i] = static_cast<uint32_t>(axes[i].NumValues());
    }

    return PackVariantKey(valueIndices, radices);
}

uint64_t PermutationSpace::ComputeVariantSpaceSize() const noexcept
{
    uint64_t size = 1;

    for (const auto& axis : axes)
    {
        size *= axis.NumValues();
    }

    return size;
}

CookError PermutationSpace::ValidateConstraints(DiagnosticSink& sink) const
{
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

void PermutationSpace::ReportUndrivenExternConstants(std::string_view module_name,
                                                     const SymbolTable& symbol_table,
                                                     DiagnosticSink& sink) const
{
    auto axesView = axes | std::views::transform(&PermutationAxis::Name);
    std::unordered_set<std::string_view> axesNames(axesView.begin(), axesView.end());
    
    auto filterUndriven = [&axesNames](const ExternConstantDeclaration& decl)
    {
        return !axesNames.contains(decl.Name);
    };

    std::vector<ExternConstantDeclaration> externConstants = symbol_table.ExternConstantsForModule(module_name);
    auto undriven = externConstants | std::views::filter(filterUndriven);

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
    std::string_view module_name, const SymbolTable& symbol_table, DiagnosticSink& sink) const
{
    std::vector<ExternConstantDefault> defaults;

    auto axesView = axes | std::views::transform(&PermutationAxis::Name);
    std::unordered_set<std::string_view> axesNames(axesView.begin(), axesView.end());

    std::vector<ExternConstantDeclaration> undriven;
    auto filterUndriven = [&axesNames](const ExternConstantDeclaration& decl)
    {
        return !axesNames.contains(decl.Name);
    };

    undriven = symbol_table.ExternConstantsForModule(module_name) |
               std::views::filter(filterUndriven) |
               std::ranges::to<std::vector<ExternConstantDeclaration>>();

    defaults.reserve(undriven.size());

    for (const auto& [constName, valueText] : undriven)
    {
        if (valueText == "true" || valueText == "false")
        {
            defaults.emplace_back(std::string{ constName }, valueText == "true" ? 1 : 0);
            continue;
        }

        const std::vector<AttrExprSymbol> known = AsAttrExprSymbols(defaults);
        const CookResult<int64_t> value = EvaluateExpression(valueText, known, sink);
        if (!value)
        {
            std::string errorMessage = std::format(
                "[lodestone] could not read the default of extern constant '{}' from '{}'. A size expression naming it would silently disagree with the shader.",
                constName,
                valueText);
            return std::unexpected(ReportError(sink, value.error(), errorMessage));
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
                                       const TargetCookPolicy& policy,
                                       const AxisValueOverrideMap& axis_value_overrides,
                                       std::vector<VariantDescriptor>& expanded,
                                       DiagnosticSink& sink) const
{
    CanonicalAssignment canonical = CanonicalizeAssignment(partial);

    if (std::cmp_equal(depth, axes.size()))
    {
        if (!policy.CookIf.empty())
        {
            const std::vector<AttrExprSymbol> symbols = SymbolsFromCanonicalAssignment(canonical);
            const CookResult<int64_t> cookIfResult = EvaluateExpression(policy.CookIf, symbols, sink);
            if (!cookIfResult) [[unlikely]]
            {
                return cookIfResult.error();
            }
            
            if (*cookIfResult == 0)
            {
                return CookError::Success;
            }
        }

        const VariantKey key = ComputeVariantKey(canonical);
        // the dense index needs the sorted set
        expanded.emplace_back(PermutationAssignment{ partial }, std::move(canonical), key, 0u);
        if ((policy.MaxVariants > 0) && (expanded.size() >= policy.MaxVariants)) [[unlikely]]
        {
            const std::string errorMessage = std::format("Permutation variant budget exceeded (max {} variants)", policy.MaxVariants);
            return ReportError(sink,
                               CookError::PermutationVariantBudgetExceeded,
                               errorMessage);
        }
        return CookError::Success;
    }

    const PermutationAxis& axis = axes[static_cast<size_t>(depth)];

    bool active = true;
    const std::vector<AttrExprSymbol> symbols = SymbolsFromCanonicalAssignment(canonical);

    // ActiveWhen only references earlier axes, so it can be evaluated here
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
        const CookResult<bool> requireResult = CheckRequires(depth, symbols, require_ready_at, sink);
        if (!requireResult) [[unlikely]]
        {
            return requireResult.error();
        }

        if (!requireResult.value())
        {
            return CookError::Success;
        }

        return expandFrom(depth + 1,
                          partial,
                          require_ready_at,
                          policy,
                          axis_value_overrides,
                          expanded,
                          sink);
    }

    std::span<const PermutationValue> axisValues = axis.GetValues();
    const auto overrideIt = axis_value_overrides.find(depth);
    if (overrideIt != axis_value_overrides.end())
    {
        axisValues = std::span<const PermutationValue>(overrideIt->second.data(), overrideIt->second.size());
    }

    for (const PermutationValue& value : axisValues)
    {
        partial.emplace_back(&axis, value);
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
            const CookError subtree = expandFrom(depth + 1,
                                                 partial,
                                                 require_ready_at,
                                                 policy,
                                                 axis_value_overrides,
                                                 expanded,
                                                 sink);
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

namespace
{
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

    CookError VerifyVariantKeysAreUnique(const std::vector<VariantDescriptor>& variants)
    {
        auto firstDuplicateIter =
            std::ranges::adjacent_find(variants,
                                       [](const VariantDescriptor& lhs, const VariantDescriptor& rhs)
                                       {
                                           return lhs.Key == rhs.Key;
                                       });

        if (firstDuplicateIter != variants.end()) [[unlikely]]
        {
            const VariantDescriptor& duplicate = *firstDuplicateIter;
            std::println(stderr,
                         "[lodestone] two variants share key {}: [{}] collides. The mixed-radix "
                         "encoding and the enumerated set disagree.",
                         static_cast<uint64_t>(duplicate.Key),
                         DescribeAssignment(duplicate.Canonical));
            return CookError::PermutationVariantIndexCollision;
        }
        else [[likely]]
        {
            return CookError::Success;
        }
    }

    std::vector<AttrExprSymbol> SymbolsFromCanonicalAssignment(
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

    CookResult<bool> EvaluateActiveWhen(const PermutationAxis& axis,
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

    CookResult<bool> CheckRequires(std::ptrdiff_t depth,
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

}

} // namespace lodestone

