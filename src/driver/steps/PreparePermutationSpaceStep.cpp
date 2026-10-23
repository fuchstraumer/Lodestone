#include "driver/steps/PreparePermutationSpaceStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SymbolTable.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/StageDump.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "permute/PolicyDocument.hpp"
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include "magic_enum/magic_enum.hpp"

namespace lodestone
{

namespace
{
    AxisKind AxisKindFromString(std::string_view str);
    CookResult<std::vector<PermutationValue>> ValuesFromStr(const std::string_view str,
                                                            DiagnosticSink& sink);
    CookResult<PermutationSpace> BuildPermutationSpace(const SymbolTable& symbol_table,
                                                       std::span<const std::string> module_names,
                                                       std::string_view module_name,
                                                       std::vector<RawAxisDeclaration> raw_axes,
                                                       DiagnosticSink& sink);
    CookResult<uint32_t> ResolveDeclaredDefaultIndex(const RawAxisDeclaration& raw_axis,
                                                     const PermutationAxis& axis,
                                                     const SymbolTable& symbol_table,
                                                     std::string_view module_name,
                                                     DiagnosticSink& sink);
    void WarnWhenDefaultsAreNotCooked(std::string_view module_name,
                                      std::string_view target_name,
                                      const PermutationSpace& space,
                                      const VariantSet& variants,
                                      DiagnosticSink& sink);
}

CookResult<PreparedPermutationSpace> PreparePermutationSpaceStep::operator()(const SharedCookState& shared_state,   
                                                                             std::string_view module_name,
                                                                             std::string_view target_name,
                                                                             const SymbolTable& symbol_table,
                                                                             std::vector<RawAxisDeclaration> raw_axes) const
{
    CookResult<PermutationSpace> result = BuildPermutationSpace(symbol_table,
                                                                shared_state.AllModuleNames,
                                                                module_name,
                                                                std::move(raw_axes),
                                                                *shared_state.Diagnostics);
    if (!result)
    {
        return std::unexpected(result.error());
    }

    const PermutationSpace& space = result.value();

    std::optional<std::string> spaceDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Space))
    {
        spaceDump = DumpPermutationSpace(module_name, space);
    }

    const CookError constraintResult = space.ValidateConstraints(*shared_state.Diagnostics); 
    if (!constraintResult)
    {
        return std::unexpected(constraintResult);
    }

    const PolicyDocument& policy = shared_state.Policy;
    const CookError policyValidationResult = policy.ValidateAgainstSpace(module_name,
                                                                         space,
                                                                         *shared_state.Diagnostics);
    if (!policyValidationResult)
    {
        return std::unexpected(policyValidationResult);
    }

    if (policy.FindTargetCoverage(module_name, target_name) == TargetPolicyCoverage::OtherTargetsOnly)
    {
        const std::string warningMessage =
            std::format("Target '{}' in module '{}' has no specific policy coverage, only other targets are covered",
                        target_name,
                        module_name);
                        
        ReportWarning(*shared_state.Diagnostics, warningMessage);
    }

    const TargetCookPolicy& targetPolicy = policy.FindTargetPolicy(module_name, target_name);

    CookResult<VariantSet> variantsResult = space.EnumerateVariants(targetPolicy, *shared_state.Diagnostics);
    if (!variantsResult)
    {
        return std::unexpected(variantsResult.error());
    }

    WarnWhenDefaultsAreNotCooked(module_name, target_name, space, variantsResult.value(), *shared_state.Diagnostics);

    std::optional<std::string> variantsDump = std::nullopt;
    if (IsStageDumpRequested(shared_state.Options, StageDumpKind::Variants))
    {
        variantsDump = DumpVariantSet(module_name, variantsResult.value());
    }

    return PreparedPermutationSpace{ std::move(*result),
                                     std::move(spaceDump),
                                     std::move(*variantsResult),
                                     std::move(variantsDump) };
}

namespace
{
    AxisKind AxisKindFromString(std::string_view str)
    {
        if (str.empty())
        {
            return AxisKind::None;
        }
        else
        {
            std::optional<AxisKind> kind = magic_enum::enum_cast<AxisKind>(str, magic_enum::case_insensitive);
            if (kind.has_value())
            {
                return kind.value();
            }
            else
            {
                return AxisKind::None;
            }
        }
    }

    CookResult<std::vector<PermutationValue>> ValuesFromStr(const std::string_view str,
                                                            DiagnosticSink& sink)
    {
        std::vector<PermutationValue> values;

        auto csvView = str |
                       std::views::split(',');
        
        for (auto chunk : csvView)
        {
            std::string_view valueStr = std::string_view(std::ranges::data(chunk), std::ranges::size(chunk));
            // from_chars fails on surrounding whitespace
            const size_t firstNonSpace = valueStr.find_first_not_of(" \t\r\n");
            if (firstNonSpace == std::string_view::npos)
            {
                continue;
            }
            const size_t lastNonSpace = valueStr.find_last_not_of(" \t\r\n");
            valueStr = valueStr.substr(firstNonSpace, lastNonSpace - firstNonSpace + 1);

            uint32_t value{ 0u };
            std::from_chars_result result = std::from_chars(valueStr.data(),
                                                            valueStr.data() + valueStr.size(),
                                                            value);
            if (result.ec != std::errc())
            {
                const std::string_view sysErrStr = magic_enum::enum_name(result.ec);
                const std::string errStr =
                    std::format("Failed to parse value '{}', error code: {}", valueStr, sysErrStr);
                return std::unexpected(ReportError(sink, CookError::FromCharsFailed, errStr));
            }
            
            values.emplace_back(value);
        }

        return values;
    }

    /** `QualityLevel::Low` and `QualityLevel.Low` both give `Low`. */
    std::string_view LastNameSegment(std::string_view name) noexcept
    {
        const size_t separator = name.find_last_of(":.");
        return separator == std::string_view::npos ? name : name.substr(separator + 1u);
    }

    std::string DescribeValue(const PermutationAxis& axis, const PermutationValue& value)
    {
        switch (axis.ValueDomain)
        {
        case AxisValueDomain::Boolean:
            return value.AsBool() ? "true" : "false";
        case AxisValueDomain::Integral:
            return std::to_string(value.AsUInt());
        case AxisValueDomain::Enum:
            return std::string{ value.AsEnumCase(axis) };
        case AxisValueDomain::Type:
            return std::string{ value.AsType(axis) };
        case AxisValueDomain::None:
            break;
        }
        return "<none>";
    }

    /** Interface axes are `extern struct`, so their initializers come from a separate table. */
    ShaderLocation LocationOf(const RawAxisDeclaration& raw_axis) noexcept
    {
        return ShaderLocation{ .File = raw_axis.SourceFile,
                               .Line = static_cast<uint32_t>(raw_axis.SourceLine),
                               .Column = static_cast<uint32_t>(raw_axis.SourceColumn) };
    }

    CookResult<std::string_view> FindDefaultText(const RawAxisDeclaration& raw_axis,
                                                 const SymbolTable& symbol_table,
                                                 std::string_view module_name,
                                                 DiagnosticSink& sink)
    {
        const std::vector<ExternConstantDeclaration> declarations =
            raw_axis.IsInterfaceAxis ? symbol_table.ExternTypeDefaultsForModule(module_name)
                                     : symbol_table.ExternConstantsForModule(module_name);
        std::optional<std::string_view> found = std::nullopt;
        for (const ExternConstantDeclaration& declaration : declarations)
        {
            if (declaration.Name != raw_axis.Name)
            {
                continue;
            }
            if (found.has_value() && found.value() != declaration.Value)
            {
                const std::string message =
                    std::format("axis '{}' in module '{}' is declared with two different defaults: '{}' and '{}'",
                                raw_axis.Name,
                                module_name,
                                found.value(),
                                declaration.Value);
                return std::unexpected(ReportError(sink, CookError::AxisDefaultAmbiguous, LocationOf(raw_axis), message));
            }
            found = declaration.Value;
        }

        if (!found.has_value() || found.value().empty())
        {
            const std::string message =
                std::format("axis '{}' in module '{}' has no default that the cooker can read. Add an initializer on "
                            "the same line as the declaration, for example `= <value>;`. Graph code fills an axis it "
                            "does not pin with this default.",
                            raw_axis.Name,
                            module_name);
            return std::unexpected(ReportError(sink, CookError::AxisDefaultMissing, LocationOf(raw_axis), message));
        }
        return found.value();
    }

    /** `true` and `false` are not expression keywords, so they are read directly. */
    CookResult<int64_t> EvaluateDefaultText(std::string_view default_text, DiagnosticSink& sink)
    {
        if (default_text == "true" || default_text == "false")
        {
            return default_text == "true" ? 1 : 0;
        }
        return EvaluateExpression(default_text, std::span<const AttrExprSymbol>{}, sink);
    }

    /** No value when none or several values match. */
    template<typename Predicate>
    std::optional<uint32_t> FindSingleValueIndex(uint32_t value_count, Predicate predicate)
    {
        std::optional<uint32_t> found = std::nullopt;
        for (uint32_t i = 0u; i < value_count; ++i)
        {
            if (!predicate(i))
            {
                continue;
            }
            if (found.has_value())
            {
                return std::nullopt;
            }
            found = i;
        }
        return found;
    }

    CookError ReportUnreadable(DiagnosticSink& sink,
                               const RawAxisDeclaration& raw_axis,
                               std::string_view module_name,
                               std::string_view default_text,
                               std::string_view reason)
    {
        const std::string message = std::format("axis '{}' in module '{}': the default '{}' {}",
                                                raw_axis.Name,
                                                module_name,
                                                default_text,
                                                reason);
        return ReportError(sink, CookError::AxisDefaultUnreadable, LocationOf(raw_axis), message);
    }

    CookResult<uint32_t> ResolveDeclaredDefaultIntegralType(int64_t& comparable_value,
                                                            const RawAxisDeclaration& raw_axis,
                                                            const PermutationAxis& axis,
                                                            const std::string_view module_name,
                                                            std::string_view default_text,
                                                            DiagnosticSink& sink)
    {
        const CookResult<int64_t> evaluated = EvaluateDefaultText(default_text, sink);
        if (!evaluated)
        {
            return std::unexpected(ReportUnreadable(sink, raw_axis, module_name, default_text, "could not be evaluated"));
        }
        comparable_value = axis.ValueDomain == AxisValueDomain::Boolean ? (evaluated.value() != 0 ? 1 : 0)
                                                                        : evaluated.value();
        auto matchesValue = [&](uint32_t idx)
        {
            return static_cast<int64_t>(axis[idx].AsUInt()) == comparable_value;
        };
        std::optional<uint32_t> index = FindSingleValueIndex(axis.NumValues(), matchesValue);

        if (!index.has_value())
        {
            const std::string message =
                std::format("axis '{}' in module '{}' declares the default {}, which is not one of its values",
                            raw_axis.Name,
                            module_name,
                            comparable_value);
            return std::unexpected(ReportError(sink, CookError::AxisDefaultNotDeclared, LocationOf(raw_axis), message));
        }
        else
        {
            return *index;
        }
    }

    CookResult<uint32_t> ResolveDeclaredDefaultEnumType(int64_t& comparable_value,
                                                        const RawAxisDeclaration& raw_axis,
                                                        const PermutationAxis& axis,
                                                        const std::string_view module_name,
                                                        std::string_view default_text,
                                                        DiagnosticSink& sink)
    {
        const std::string_view caseName = LastNameSegment(default_text);

        auto matchesCase = [&](uint32_t i)
        { 
            return axis.EnumCaseName(i) == caseName;
        };
        std::optional<uint32_t> index =
            FindSingleValueIndex(axis.NumValues(), matchesCase);

        if (!index.has_value())
        {
            return std::unexpected(ReportUnreadable(sink,
                                                    raw_axis,
                                                    module_name,
                                                    default_text,
                                                    "does not name a case of the enum"));
        }
        comparable_value = axis.EnumCase(index.value()).Value;
        return *index;
    }

    CookResult<uint32_t> ResolveDeclaredDefaultInterfaceType(const RawAxisDeclaration& raw_axis,
                                                             const PermutationAxis& axis,
                                                             const std::string_view module_name,
                                                             std::string_view default_text,
                                                             DiagnosticSink& sink)
    {
        const std::string_view typeName = LastNameSegment(default_text);

        auto matchesImpl = [&](uint32_t idx)
        {
            return LastNameSegment(axis.InterfaceImplTypeName(idx)) == typeName;
        };
        std::optional<uint32_t> index =
            FindSingleValueIndex(axis.NumValues(), matchesImpl);

        if (!index.has_value())
        {
            const std::string message =
                std::format("axis '{}' in module '{}' declares the default type '{}', which is not exactly one of "
                            "its tagged implementations",
                            raw_axis.Name,
                            module_name,
                            default_text);
            return std::unexpected(ReportError(sink, CookError::AxisDefaultNotDeclared, LocationOf(raw_axis), message));
        }
        else
        {
            return *index;
        }
    }

    CookResult<uint32_t> ResolveDeclaredDefaultIndex(const RawAxisDeclaration& raw_axis,
                                                     const PermutationAxis& axis,
                                                     const SymbolTable& symbol_table,
                                                     std::string_view module_name,
                                                     DiagnosticSink& sink)
    {
        const CookResult<std::string_view> defaultText = FindDefaultText(raw_axis, symbol_table, module_name, sink);
        if (!defaultText)
        {
            return std::unexpected(defaultText.error());
        }
        int64_t comparableValue = 0;
        CookResult<uint32_t> index = std::unexpected(CookError::Success);
        switch (axis.ValueDomain)
        {
        case AxisValueDomain::Boolean:
        case AxisValueDomain::Integral:
            index = ResolveDeclaredDefaultIntegralType(comparableValue,
                                                       raw_axis,
                                                       axis,
                                                       module_name,
                                                       defaultText.value(),
                                                       sink);
            break;
        case AxisValueDomain::Enum:
            index = ResolveDeclaredDefaultEnumType(comparableValue,
                                                   raw_axis,
                                                   axis,
                                                   module_name,
                                                   defaultText.value(),
                                                   sink);
            break;
        case AxisValueDomain::Type:
            index = ResolveDeclaredDefaultInterfaceType(raw_axis,
                                                        axis,
                                                        module_name,
                                                        defaultText.value(),
                                                        sink);
            break;
        case AxisValueDomain::None:
            std::unreachable();
        }

        if (!index)
        {
            return std::unexpected(index.error());
        }

        // the source text decides, and reflection checks it where it can
        if (raw_axis.ReflectedDefault.has_value() && raw_axis.ReflectedDefault.value() != comparableValue)
        {
            const std::string message =
                std::format("axis '{}' in module '{}': the source text reads the default as {}, but Slang reflection "
                            "reads {}",
                            raw_axis.Name,
                            module_name,
                            comparableValue,
                            raw_axis.ReflectedDefault.value());
            return std::unexpected(ReportError(sink, CookError::AxisDefaultMismatch, LocationOf(raw_axis), message));
        }

        return index.value();
    }

    void WarnWhenDefaultsAreNotCooked(std::string_view module_name,
                                      std::string_view target_name,
                                      const PermutationSpace& space,
                                      const VariantSet& variants,
                                      DiagnosticSink& sink)
    {
        for (const PermutationAxis& axis : space.Axes())
        {
            bool axisActiveSomewhere = false;
            bool defaultCooked = false;
            for (const VariantDescriptor& variant : variants.Variants)
            {
                const auto binding = std::ranges::find(variant.Active, &axis, &PermutationBinding::Axis);
                if (binding == variant.Active.end())
                {
                    continue;
                }
                axisActiveSomewhere = true;
                defaultCooked = defaultCooked || binding->Value == axis.GetDeclaredDefault();
            }

            if (axisActiveSomewhere && !defaultCooked)
            {
                ReportWarning(sink,
                              std::format("module '{}', target '{}': no cooked variant uses the default of axis '{}' "
                                          "({}), so a query that fills this axis with its default finds no variant",
                                          module_name,
                                          target_name,
                                          axis.Name,
                                          DescribeValue(axis, axis.GetDeclaredDefault())));
            }
        }
    }

    CookResult<PermutationSpace> BuildPermutationSpace(const SymbolTable& symbol_table,
                                                       std::span<const std::string> module_names,
                                                       std::string_view module_name,
                                                       std::vector<RawAxisDeclaration> raw_axes,
                                                       DiagnosticSink& sink)
    {
        // prune the axes no source uses
        auto extractNameStrView = [](const RawAxisDeclaration& raw_axis)
        {
            return std::string_view{ raw_axis.Name };
        };
        std::vector<std::string_view> axisNamesVec = raw_axes |
                                                     std::views::transform(extractNameStrView) |
                                                     std::ranges::to<std::vector<std::string_view>>();
        std::vector<std::string_view> missingAxisNames = symbol_table.MissingTokens(module_names, axisNamesVec);

        auto filterUnusedAxis = [&missingAxisNames](const RawAxisDeclaration& raw_axis)
        {
            return std::ranges::find(missingAxisNames, raw_axis.Name) == missingAxisNames.end();
        };
        std::vector<RawAxisDeclaration> filteredAxes = raw_axes |
                                                       std::views::as_rvalue |
                                                       std::views::filter(filterUnusedAxis) |
                                                       std::ranges::to<std::vector<RawAxisDeclaration>>();
        
        if (!missingAxisNames.empty())
        {
            auto foldStrNames = [](std::span<std::string_view> names)
            {
                return std::ranges::fold_left(names, std::string{}, [](std::string acc, std::string_view name)
                {
                    if (!acc.empty())
                    {
                        acc += ", ";
                    }
                    acc += name;
                    return acc;
                });
            };

            std::string messageStr =
                std::format("The following axes were declared but not used in any module: {}", foldStrNames(missingAxisNames));
            ReportInfo(sink, std::move(messageStr));
        }

        std::vector<PermutationAxis> axes;
        for (RawAxisDeclaration& rawAxis : filteredAxes)
        {
            const AxisKind kind = AxisKindFromString(rawAxis.Kind);
            std::vector<PermutationValue> values;
            if (rawAxis.IsBooleanAxis)
            { 
                axes.emplace_back(rawAxis.Name,
                                std::vector<PermutationValue>{ PermutationValue{ false }, PermutationValue{ true } },
                                kind,
                                EarliestBindingTime::Cook,
                                AxisValueDomain::Boolean,
                                rawAxis.ActiveWhen);
            }
            else if (rawAxis.IsInterfaceAxis)
            {
                // each value is the index of the implementation
                for (uint32_t i = 0; std::cmp_less(i, rawAxis.InterfaceImpls.size()); ++i)
                {
                    values.emplace_back(PermutationValue::MakeType(i));
                }
                axes.emplace_back(rawAxis.Name,
                                  values,
                                  kind,
                                  EarliestBindingTime::Cook,
                                  AxisValueDomain::Type,
                                  rawAxis.ActiveWhen,
                                  rawAxis.RootName,
                                  rawAxis.InterfaceImpls);
            }
            else if (rawAxis.IsEnumAxis)
            {
                for (uint32_t i = 0; std::cmp_less(i, rawAxis.EnumCases.size()); ++i)
                {
                    values.emplace_back(PermutationValue::MakeEnum(i));
                }
                axes.emplace_back(rawAxis.Name,
                                  values,
                                  kind,
                                  EarliestBindingTime::Cook,
                                  AxisValueDomain::Enum,
                                  rawAxis.ActiveWhen,
                                  rawAxis.RootName,
                                  rawAxis.RootModule,
                                  rawAxis.EnumCases);
            }
            else
            {
                CookResult<std::vector<PermutationValue>> splitValues = ValuesFromStr(rawAxis.AxisValues, sink);
                if (!splitValues)
                {
                    return std::unexpected(splitValues.error());
                }
                values = std::move(*splitValues);
                axes.emplace_back(rawAxis.Name,
                                  values,
                                  kind,
                                  EarliestBindingTime::Cook,
                                  AxisValueDomain::Integral,
                                  rawAxis.ActiveWhen);
            }

            const CookResult<uint32_t> defaultIndex =
                ResolveDeclaredDefaultIndex(rawAxis, axes.back(), symbol_table, module_name, sink);
            if (!defaultIndex)
            {
                return std::unexpected(defaultIndex.error());
            }
            axes.back().SetDeclaredDefaultIndex(defaultIndex.value());
        }

        return PermutationSpace{ axes };
    }
}

}
