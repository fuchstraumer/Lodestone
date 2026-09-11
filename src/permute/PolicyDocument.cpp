#include "permute/PolicyDocument.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "permute/AttributeExpression.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define TOML_EXCEPTIONS 0
#include <toml++/toml.hpp>

namespace lodestone
{

namespace
{

    // Builds a parse error that points at a node. toml++ records a source region for every node, so a
    // wrong type reports the line and the column of the offending value.
    PolicyParseError ErrorAt(const toml::node& node, std::string message);
    PolicyDocResult<StringMap<std::vector<std::string>>> ReadInertAxes(const toml::table& module_table);
    PolicyDocResult<AxisCookValues> ReadAxisCookValues(std::string_view axis_name,
                                                       const toml::node& values_node);
    PolicyDocResult<TargetPolicy> ReadTargetPolicy(const toml::table& target_table);
    // Reads the optional targets table of one module. Each key is a target profile name.
    PolicyDocResult<StringMap<TargetPolicy>> ReadTargets(const toml::table& module_table);
    PolicyParseError MakeParseError(const toml::parse_result& result);
    PolicyDocResult<StringMap<ModulePolicyEntry>> BuildModules(const toml::table& root);
    const PermutationAxis* FindDeclaredAxis(std::span<const PermutationAxis> axes, std::string_view name);
    bool AxisDeclaresValue(const PermutationAxis& axis, int64_t value);
    CookError ValidateExpectedInfluenceTable(const std::string_view module_name,
                                             const std::span<const PermutationAxis> axes,
                                             const std::span<const std::string> inert_axes,
                                             DiagnosticSink& sink);
    CookError ValidateTargetAndAxisValues(const std::string_view module_name,
                                          const StringMap<TargetPolicy>& targets,
                                          const std::span<const PermutationAxis> axes,
                                          DiagnosticSink& sink);

} // namespace

PolicyDocResult<PolicyDocument> PolicyDocument::Load(std::string_view path)
{
    const toml::parse_result result = toml::parse_file(path);
    if (result.failed())
    {
        return std::unexpected(MakeParseError(result));
    }

    PolicyDocResult<StringMap<ModulePolicyEntry>> modules = BuildModules(result.table());
    if (!modules)
    {
        return std::unexpected(std::move(modules.error()));
    }

    PolicyDocument document;
    document.modules = std::move(*modules);
    return document;
}

PolicyDocResult<PolicyDocument> PolicyDocument::Parse(std::string_view text)
{
    const toml::parse_result result = toml::parse(text);
    if (result.failed())
    {
        return std::unexpected(MakeParseError(result));
    }

    PolicyDocResult<StringMap<ModulePolicyEntry>> modules = BuildModules(result.table());
    if (!modules)
    {
        return std::unexpected(std::move(modules.error()));
    }

    PolicyDocument document;
    document.modules = std::move(*modules);
    return document;
}

const ModulePolicyEntry* PolicyDocument::FindModule(std::string_view module_name) const noexcept
{
    const auto found = modules.find(module_name);
    if (found == modules.end())
    {
        return nullptr;
    }
    return &found->second;
}

const TargetPolicy& PolicyDocument::FindTargetPolicy(std::string_view module_name,
                                                     std::string_view target_name) const noexcept
{
    static const TargetPolicy empty;

    const ModulePolicyEntry* entry = FindModule(module_name);
    if (entry == nullptr)
    {
        return empty;
    }

    const auto found = entry->Targets.find(target_name);
    if (found == entry->Targets.end())
    {
        return empty;
    }
    return found->second;
}

std::span<const std::string> PolicyDocument::InertAxesForEntryPoint(std::string_view module_name,
                                                                    std::string_view entry_point_name) const noexcept
{
    const ModulePolicyEntry* entry = FindModule(module_name);
    if (entry == nullptr)
    {
        return {};
    }

    const auto found = entry->InertAxesForEntryPoints.find(entry_point_name);
    if (found == entry->InertAxesForEntryPoints.end())
    {
        return {};
    }
    else
    {
        const std::vector<std::string>& names = found->second;
        return names;
    }
}

CookError PolicyDocument::ValidateAgainstSpace(std::string_view module_name,
                                               const PermutationSpace& space,
                                               DiagnosticSink& sink) const
{
    const ModulePolicyEntry* entry = FindModule(module_name);
    if (entry == nullptr)
    { 
        // the file names no policy for this module, so nothing to check
        return CookError::Success;
    }

    // collate all of the inert axes for the current module
    auto inertAxesStrs = entry->InertAxesForEntryPoints | std::views::values | std::views::join;
    // use views so we don't copy a bunch
    std::vector<std::string_view> inertAxesNames(inertAxesStrs.begin(), inertAxesStrs.end());
    // now filter out to only uniques
    std::ranges::sort(inertAxesNames);
    auto [firstToErase, lastToErase] = std::ranges::unique(inertAxesNames);
    inertAxesNames.erase(firstToErase, lastToErase);

    // now extract names from axes
    auto allAxesStrs = space.Axes() |
                       std::views::transform([](const PermutationAxis& axis) { return std::string_view{ axis.Name }; }) |
                       std::ranges::to<std::vector<std::string_view>>();
    std::ranges::sort(allAxesStrs);

    // find if there are any names in inertAxesNames that are not present in allAxesStrs
    std::vector<std::string_view> missingAxes;
    std::ranges::set_difference(inertAxesNames, allAxesStrs, std::back_inserter(missingAxes));
    if (!missingAxes.empty())
    {
        CookError err = CookError::Invalid;
        for (const auto& axis : missingAxes)
        {
            const std::string errStr = std::format("Inert axis '{}' is not present in the permutation space", axis);
            err = ReportError(sink, CookError::PolicyAxisNotDeclared, errStr);
        }
        return err;
    }

    return ValidateTargetAndAxisValues(module_name, entry->Targets, space.Axes(), sink);

}

size_t PolicyDocument::ModuleCount() const noexcept
{
    return modules.size();
}

namespace
{
    // Builds a parse error that points at a node. toml++ records a source region for every node, so a
    // wrong type reports the line and the column of the offending value.
    PolicyParseError ErrorAt(const toml::node& node, std::string message)
    {
        const toml::source_region& region = node.source();
        return PolicyParseError{ .Message = std::move(message),
                                 .Line = region.begin.line,
                                 .Column = region.begin.column };
    }

    PolicyDocResult<StringMap<std::vector<std::string>>> ReadInertAxes(const toml::table& module_table)
    {
        StringMap<std::vector<std::string>> inertAxes;
        const auto node = module_table["InertAxesForEntryPoints"];
        if (node && node.is_table())
        {
            const toml::table* table = node.as_table();
            if (table == nullptr)
            {
                return std::unexpected(ErrorAt(*node.node(), "InertAxesForEntryPoints must be a table"));
            }
            
            for (const auto& [entryPointKey, axisNamesNode] : *table)
            {
                const std::string entryPointName{ entryPointKey.str() };
                const toml::array* axisNamesArray = axisNamesNode.as_array();
                if (axisNamesArray == nullptr)
                {
                    return std::unexpected(ErrorAt(axisNamesNode, "InertAxesForEntryPoints entries must be arrays"));
                }
                else
                {
                    std::vector<std::string> axisNames;
                    for (const toml::node& element : *axisNamesArray)
                    {
                        if (const std::optional<std::string> str = element.value<std::string>())
                        {
                            axisNames.push_back(*str);
                        }
                    }
                    inertAxes.emplace(entryPointName, std::move(axisNames));
                }
            }
        }
        return inertAxes;
    }

    PolicyDocResult<AxisCookValues> ReadAxisCookValues(std::string_view axis_name,
                                                       const toml::node& values_node)
    {
        const toml::array* values = values_node.as_array();
        if (values == nullptr)
        {
            return std::unexpected(
                ErrorAt(values_node, std::format("CookValues.{} must be an array", axis_name)));
        }

        AxisCookValues cookValues;
        cookValues.Axis = std::string{ axis_name };
        for (const toml::node& element : *values)
        {
            if (element.is_boolean())
            {
                cookValues.Values.emplace_back(*element.value<bool>());
            }
            else if (element.is_integer())
            {
                const int64_t val = *element.value<int64_t>();
                // TOML doesn't let us distinguish between signed or unsigned: we're going
                // to assume unsigned
                // todo-ship: We need to figure out how to handle this better. Either PermutationValue
                // loses it's signed support, or we make this handle signed somehow (also maybe widen?)
                cookValues.Values.emplace_back(static_cast<uint32_t>(val));
            }
            else
            {
                return std::unexpected(ErrorAt(
                    element, std::format("CookValues.{} entries must be integers or booleans", axis_name)));
            }
        }

        return cookValues;
    }

    PolicyDocResult<TargetPolicy> ReadTargetPolicy(const toml::table& target_table)
    {
        TargetPolicy policy;

        if (const auto maxVariants = target_table["MaxVariants"])
        {
            const std::optional<int64_t> value = maxVariants.value<int64_t>();
            if (!value || *value < 0)
            {
                return std::unexpected(
                    ErrorAt(*maxVariants.node(), "MaxVariants must be a non-negative integer"));
            }
            policy.MaxVariants = static_cast<uint32_t>(*value);
        }

        if (const auto cookIf = target_table["CookIf"])
        {
            const std::optional<std::string> value = cookIf.value<std::string>();
            if (!value)
            {
                return std::unexpected(ErrorAt(*cookIf.node(), "CookIf must be a string"));
            }
            policy.CookIf = *value;
        }

        if (const auto cookValues = target_table["CookValues"])
        {
            const toml::table* table = cookValues.as_table();
            if (table == nullptr)
            {
                return std::unexpected(
                    ErrorAt(*cookValues.node(), "CookValues must be a table of axis arrays"));
            }
            for (auto&& [axisKey, valuesNode] : *table)
            {
                PolicyDocResult<AxisCookValues> axisValues = ReadAxisCookValues(axisKey.str(), valuesNode);
                if (!axisValues)
                {
                    return std::unexpected(std::move(axisValues.error()));
                }
                policy.CookValues.push_back(std::move(*axisValues));
            }
        }

        return policy;
    }

    PolicyDocResult<StringMap<TargetPolicy>> ReadTargets(const toml::table& module_table)
    {
        StringMap<TargetPolicy> targets;

        const auto node = module_table["targets"];
        if (!node)
        {
            return targets;
        }

        const toml::table* table = node.as_table();
        if (table == nullptr)
        {
            return std::unexpected(ErrorAt(*node.node(), "targets must be a table"));
        }

        for (auto&& [targetKey, targetNode] : *table)
        {
            const toml::table* targetTable = targetNode.as_table();
            if (targetTable == nullptr)
            {
                return std::unexpected(
                    ErrorAt(targetNode, std::format("target '{}' must be a table", targetKey.str())));
            }

            PolicyDocResult<TargetPolicy> policy = ReadTargetPolicy(*targetTable);
            if (!policy)
            {
                return std::unexpected(std::move(policy.error()));
            }
            targets.emplace(std::string{ targetKey.str() }, std::move(*policy));
        }

        return targets;
    }

    PolicyParseError MakeParseError(const toml::parse_result& result)
    {
        const toml::parse_error& error = result.error();
        const toml::source_region& region = error.source();
        return PolicyParseError{ .Message = std::string{ error.description() },
                                 .Line = region.begin.line,
                                 .Column = region.begin.column };
    }

    PolicyDocResult<StringMap<ModulePolicyEntry>> BuildModules(const toml::table& root)
    {
        StringMap<ModulePolicyEntry> modules;
        for (auto&& [moduleKey, moduleNode] : root)
        {
            const toml::table* moduleTable = moduleNode.as_table();
            if (moduleTable == nullptr)
            {
                return std::unexpected(ErrorAt(
                    moduleNode, std::format("top-level entry '{}' must be a table", moduleKey.str())));
            }

            ModulePolicyEntry entry;
            PolicyDocResult<StringMap<std::vector<std::string>>> inertAxes = ReadInertAxes(*moduleTable);
            if (!inertAxes)
            {
                return std::unexpected(std::move(inertAxes.error()));
            }

            entry.InertAxesForEntryPoints = std::move(*inertAxes);

            PolicyDocResult<StringMap<TargetPolicy>> targets = ReadTargets(*moduleTable);
            if (!targets)
            {
                return std::unexpected(std::move(targets.error()));
            }
            entry.Targets = std::move(*targets);

            modules.emplace(std::string{ moduleKey.str() }, std::move(entry));
        }

        return modules;
    }

    const PermutationAxis* FindDeclaredAxis(std::span<const PermutationAxis> axes, std::string_view name)
    {
        auto findName = [&name](const PermutationAxis& axis) { return axis.Name == name; };
        auto axisIter = std::ranges::find_if(axes, findName);
        if (axisIter != axes.end())
        {
            return std::to_address(axisIter);
        }
        return nullptr;
    }

    bool AxisDeclaresValue(const PermutationAxis& axis, const PermutationValue value)
    {
        const auto axisValues = axis.GetValues();
        auto found = std::ranges::find(axisValues, value);
        return found != axisValues.end();
    }

    CookError ValidateExpectedInfluenceTable(const std::string_view module_name,
                                             const std::span<const PermutationAxis> axes,
                                             const std::span<std::string> influences,
                                             DiagnosticSink& sink)
    {
        for (const std::string& axisName : influences)
        {
            if (FindDeclaredAxis(axes, axisName) == nullptr)
            {
                return ReportError(
                    sink,
                    CookError::PolicyAxisNotDeclared,
                    std::format("ExpectedInfluence names axis '{}', which module '{}' does not declare",
                                axisName,
                                module_name));
            }
        }

        return CookError::Success;
    }

    CookError ValidateTargetAndAxisValues(const std::string_view module_name,
                                          const StringMap<TargetPolicy>& targets,
                                          const std::span<const PermutationAxis> axes,
                                          DiagnosticSink& sink)
    {
        for (const auto& [targetName, target] : targets)
        {
            for (const AxisCookValues& cookValues : target.CookValues)
            {
                const PermutationAxis* axis = FindDeclaredAxis(axes, cookValues.Axis);
                if (axis == nullptr)
                {
                    return ReportError(
                        sink,
                        CookError::PolicyAxisNotDeclared,
                        std::format("target '{}' CookValues names axis '{}', which module '{}' does not declare",
                                    targetName,
                                    cookValues.Axis,
                                    module_name));
                }

                for (const PermutationValue value : cookValues.Values)
                {
                    if (!AxisDeclaresValue(*axis, value))
                    {
                        return ReportError(sink,
                                        CookError::PolicyValueNotInAxis,
                                        std::format("target '{}' CookValues for axis '{}' lists value {}, "
                                                    "which the axis does not declare",
                                                    targetName,
                                                    cookValues.Axis,
                                                    ValueToPrintableString(value)));
                    }
                }
            }

            if (!target.CookIf.empty())
            {
                const CookResult<std::vector<std::string>> identifiers =
                    CollectExpressionIdentifiers(target.CookIf, sink);
                if (!identifiers)
                {
                    return ReportError(sink,
                                    CookError::PolicyCookIfInvalid,
                                    std::format("target '{}' CookIf '{}' is not a valid expression",
                                                targetName,
                                                target.CookIf));
                }

                for (const std::string& identifier : *identifiers)
                {
                    if (FindDeclaredAxis(axes, identifier) == nullptr)
                    {
                        return ReportError(sink,
                                        CookError::PolicyAxisNotDeclared,
                                        std::format("target '{}' CookIf '{}' uses unknown axis '{}'",
                                                    targetName,
                                                    target.CookIf,
                                                    identifier));
                    }
                }
            }
        }

        return CookError::Success;
    }
}

} // namespace lodestone
