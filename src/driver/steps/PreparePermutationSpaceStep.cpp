#include "driver/steps/PreparePermutationSpaceStep.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SymbolTable.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/StageDump.hpp"
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
                                                       std::vector<RawAxisDeclaration> raw_axes,
                                                       DiagnosticSink& sink);
}

CookResult<PreparedPermutationSpace> PreparePermutationSpaceStep::operator()(const SharedCookState& shared_state,   
                                                                             std::string_view module_name,
                                                                             std::string_view target_name,
                                                                             const SymbolTable& symbol_table,
                                                                             std::vector<RawAxisDeclaration> raw_axes) const
{
    // Fairly simple control flow: build the space, then validate it's constraints and cross-check it against the policy
    CookResult<PermutationSpace> result = BuildPermutationSpace(symbol_table,
                                                                shared_state.AllModuleNames,
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

    // finally, validate space against policy document
    const PolicyDocument& policy = shared_state.Policy;
    const CookError policyValidationResult = policy.ValidateAgainstSpace(module_name,
                                                                         space,
                                                                         *shared_state.Diagnostics);
    if (!policyValidationResult)
    {
        return std::unexpected(policyValidationResult);
    }

    const TargetCookPolicy& targetPolicy = policy.FindTargetPolicy(module_name, target_name);

    CookResult<VariantSet> variantsResult = space.EnumerateVariants(targetPolicy, *shared_state.Diagnostics);
    if (!variantsResult)
    {
        return std::unexpected(variantsResult.error());
    }

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
            // make sure to use case-insensitive, otherwise "tuning" would not match AxisKind::Tuning
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
            // we have to trim leading and trailing whitespace, if it's present, as from_chars will fail 
            // if we don't make sure to trim it out
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

    CookResult<PermutationSpace> BuildPermutationSpace(const SymbolTable& symbol_table,
                                                       std::span<const std::string> module_names,
                                                       std::vector<RawAxisDeclaration> raw_axes,
                                                       DiagnosticSink& sink)
    {
        // First step: prune axes in raw axes that aren't actually used
        auto extractNameStrView = [](const RawAxisDeclaration& raw_axis)
        {
            return std::string_view{ raw_axis.Name };
        };
        std::vector<std::string_view> axisNamesVec = raw_axes |
                                                     std::views::transform(extractNameStrView) |
                                                     std::ranges::to<std::vector<std::string_view>>();
        std::vector<std::string_view> missingAxisNames = symbol_table.MissingTokens(module_names, axisNamesVec);

        // build the condensed span - use views and filter to remove the axes from raw_axes that aren't
        // used in any of the source code for the given modules.
        auto filterUnusedAxis = [&missingAxisNames](const RawAxisDeclaration& raw_axis)
        {
            return std::ranges::find(missingAxisNames, raw_axis.Name) == missingAxisNames.end();
        };
        std::vector<RawAxisDeclaration> filteredAxes = raw_axes |
                                                       std::views::as_rvalue |
                                                       std::views::filter(filterUnusedAxis) |
                                                       std::ranges::to<std::vector<RawAxisDeclaration>>();
        
        // Just for info sake (and because we can filter this), if missingAxisNames is not empty, we can log which axes were missing.
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

        // Second step: build the axes, using the filtered list of only the axes that are actually used
        std::vector<PermutationAxis> axes;
        for (RawAxisDeclaration& rawAxis : filteredAxes)
        {
            const AxisKind kind = AxisKindFromString(rawAxis.Kind);
            // values extraction - fork on boolean, if not boolean it's just a comma split
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
                // build the expanded list of permutation values for the interface axis.
                // each value is just the index of that interface implementation in the list of all implementations
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

        }

        return PermutationSpace{ axes };
    }
}

}
