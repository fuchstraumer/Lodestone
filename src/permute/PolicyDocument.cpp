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
    PolicyDocResult<std::vector<PolicyInfluence>> ReadInfluence(const toml::table& module_table);
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
                                             const std::span<const PolicyInfluence> influences,
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

std::span<const PolicyInfluence> PolicyDocument::ExpectedInfluenceFor(
    std::string_view module_name) const noexcept
{
    const ModulePolicyEntry* entry = FindModule(module_name);
    if (entry == nullptr)
    {
        return {};
    }
    return entry->ExpectedInfluence;
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

    CookError error = ValidateExpectedInfluenceTable(module_name, space.Axes(), entry->ExpectedInfluence, sink);
    if (!error)
    {
        return error;
    }

    error = ValidateTargetAndAxisValues(module_name, entry->Targets, space.Axes(), sink);

    return error;
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

    PolicyDocResult<std::vector<PolicyInfluence>> ReadInfluence(const toml::table& module_table)
    {
        std::vector<PolicyInfluence> influence;

        const auto node = module_table["ExpectedInfluence"];
        if (!node)
        {
            return influence; // absent, so the module states no expected influence
        }

        const toml::array* entries = node.as_array();
        if (entries == nullptr)
        {
            return std::unexpected(ErrorAt(*node.node(), "ExpectedInfluence must be an array of tables"));
        }

        for (const toml::node& element : *entries)
        {
            const toml::table* record = element.as_table();
            if (record == nullptr)
            {
                return std::unexpected(ErrorAt(element, "each ExpectedInfluence entry must be a table"));
            }

            const std::optional<std::string> entryPoint = (*record)["EntryPoint"].value<std::string>();
            const std::optional<std::string> axis = (*record)["Axis"].value<std::string>();
            if (!entryPoint)
            {
                return std::unexpected(ErrorAt(element, "ExpectedInfluence entry needs a string EntryPoint"));
            }
            if (!axis)
            {
                return std::unexpected(ErrorAt(element, "ExpectedInfluence entry needs a string Axis"));
            }

            influence.push_back(
                PolicyInfluence{ .EntryPoint = *entryPoint,
                                 .Axis = *axis,
                                 .IsInert = (*record)["Inert"].value<bool>().value_or(true) });
        }

        return influence;
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
            if (const std::optional<int32_t> asInt = element.value<int32_t>())
            {
                cookValues.Values.emplace_back(*asInt);
            }
            else if (const std::optional<uint32_t> asUInt = element.value<uint32_t>())
            {
                cookValues.Values.emplace_back(*asUInt);
            }
            else if (const std::optional<bool> asBool = element.value<bool>())
            {
                cookValues.Values.emplace_back(*asBool);
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

        if (const auto cookWhen = target_table["CookWhen"])
        {
            const std::optional<std::string> value = cookWhen.value<std::string>();
            if (!value)
            {
                return std::unexpected(ErrorAt(*cookWhen.node(), "CookWhen must be a string"));
            }
            policy.CookWhen = *value;
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

            PolicyDocResult<std::vector<PolicyInfluence>> influence = ReadInfluence(*moduleTable);
            if (!influence)
            {
                return std::unexpected(std::move(influence.error()));
            }
            entry.ExpectedInfluence = std::move(*influence);

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
                                             const std::span<const PolicyInfluence> influences,
                                             DiagnosticSink& sink)
    {
        for (const PolicyInfluence& influence : influences)
        {
            if (FindDeclaredAxis(axes, influence.Axis) == nullptr)
            {
                return ReportError(
                    sink,
                    CookError::PolicyAxisNotDeclared,
                    std::format("ExpectedInfluence names axis '{}', which module '{}' does not declare",
                                influence.Axis,
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

            if (!target.CookWhen.empty())
            {
                const CookResult<std::vector<std::string>> identifiers =
                    CollectExpressionIdentifiers(target.CookWhen, sink);
                if (!identifiers)
                {
                    return ReportError(sink,
                                    CookError::PolicyCookWhenInvalid,
                                    std::format("target '{}' CookWhen '{}' is not a valid expression",
                                                targetName,
                                                target.CookWhen));
                }

                for (const std::string& identifier : *identifiers)
                {
                    if (FindDeclaredAxis(axes, identifier) == nullptr)
                    {
                        return ReportError(sink,
                                        CookError::PolicyAxisNotDeclared,
                                        std::format("target '{}' CookWhen '{}' uses unknown axis '{}'",
                                                    targetName,
                                                    target.CookWhen,
                                                    identifier));
                    }
                }
            }
        }

        return CookError::Success;
    }
}

} // namespace lodestone
