#include "permute/PolicyDocument.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderManifestIndex.hpp"
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

    // toml++ records a source region per node, so errors give the line and column
    PolicyParseError ErrorAt(const toml::node& node, std::string message);
    PolicyDocResult<StringMap<std::vector<std::string>>> ReadInertAxes(const toml::table& module_table);
    PolicyDocResult<StringMap<std::vector<PolicyInterfaceType>>> ReadInterfaceAxes(const toml::table& module_table);
    PolicyDocResult<std::vector<PolicyInterfaceType>> ReadInterfaceTypes(std::string_view axis_name,
                                                                         const toml::table& axis_table);
    PolicyDocResult<PolicyInterfaceType> ReadInterfaceType(std::string_view axis_name, const toml::node& type_node);
    PolicyDocResult<AxisCookValues> ReadAxisCookValues(std::string_view axis_name,
                                                       const toml::node& values_node);
    PolicyDocResult<TargetCookPolicy> ReadTargetPolicy(const toml::table& target_table);
    PolicyDocResult<StringMap<TargetCookPolicy>> ReadTargets(const toml::table& module_table);
    PolicyParseError MakeParseError(const toml::parse_result& result);
    std::string FileOf(const toml::source_region& region);
    void MoveNodeInto(toml::table& destination, std::string_view key, toml::node& node);
    PolicyParseError MakeKeyConflict(const toml::node& existing, const toml::node& incoming, std::string_view dotted_key);
    /** Moves every key of `source` into `destination`. Returns the conflict when two files set one key. */
    std::optional<PolicyParseError> MergeTables(toml::table& destination,
                                                toml::table& source,
                                                std::string_view key_prefix);
    PolicyDocResult<StringMap<ModulePolicyEntry>> BuildModules(const toml::table& root);
    const PermutationAxis* FindDeclaredAxis(std::span<const PermutationAxis> axes, std::string_view name);
    CookError ValidateTargetAndAxisValues(const std::string_view module_name,
                                          const StringMap<TargetCookPolicy>& targets,
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

PolicyDocResult<PolicyDocument> PolicyDocument::LoadMerged(std::span<const std::string> paths)
{
    toml::table merged;
    for (const std::string& path : paths)
    {
        toml::parse_result result = toml::parse_file(path);
        if (result.failed())
        {
            return std::unexpected(MakeParseError(result));
        }

        const std::optional<PolicyParseError> conflict = MergeTables(merged, result.table(), "");
        if (conflict)
        {
            return std::unexpected(*conflict);
        }
    }

    PolicyDocResult<StringMap<ModulePolicyEntry>> modules = BuildModules(merged);
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

const TargetCookPolicy& PolicyDocument::FindTargetPolicy(std::string_view module_name,
                                                     std::string_view target_name) const noexcept
{
    static const TargetCookPolicy empty;

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

std::span<const PolicyInterfaceType> PolicyDocument::InterfaceTypesForAxis(std::string_view module_name,
                                                                           std::string_view axis_name) const noexcept
{
    const ModulePolicyEntry* entry = FindModule(module_name);
    if (entry == nullptr)
    {
        return {};
    }

    const auto found = entry->InterfaceAxes.find(axis_name);
    if (found == entry->InterfaceAxes.end())
    {
        return {};
    }

    const std::vector<PolicyInterfaceType>& types = found->second;
    return types;
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
        return CookError::Success;
    }

    size_t inertAxesCount = 0;
    for (const auto& [entryPoint, axes] : entry->InertAxesForEntryPoints)
    {
        inertAxesCount += axes.size();
    }

    std::vector<std::string_view> inertAxesNames;
    inertAxesNames.reserve(inertAxesCount);

    auto inertAxesStrs = entry->InertAxesForEntryPoints | std::views::values | std::views::join;
    inertAxesNames.append_range(inertAxesStrs);

    std::ranges::sort(inertAxesNames);
    auto [firstToErase, lastToErase] = std::ranges::unique(inertAxesNames);
    inertAxesNames.erase(firstToErase, lastToErase);

    auto allAxesStrs = space.Axes() |
                       std::views::transform(&PermutationAxis::Name) |
                       std::ranges::to<std::vector<std::string_view>>();
    std::ranges::sort(allAxesStrs);

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

TargetPolicyCoverage PolicyDocument::FindTargetCoverage(std::string_view module_name,
                                                        std::string_view target_name) const noexcept
{
    const auto moduleIter = modules.find(module_name);
    if (moduleIter == modules.end())
    {
        return TargetPolicyCoverage::NoModuleEntry;
    }

    const StringMap<TargetCookPolicy>& targets = moduleIter->second.Targets;
    if (targets.empty())
    {
        return TargetPolicyCoverage::NoTargetSections;
    }

    const auto targetIter = targets.find(target_name);
    if (targetIter != targets.end())
    {
        return TargetPolicyCoverage::Present;
    }

    return TargetPolicyCoverage::OtherTargetsOnly;
}

CookError PolicyDocument::ValidateTargetNames(std::span<const std::string_view> target_names,
                                              DiagnosticSink& sink) const
{
    for (const auto& [module_name, module_entry] : modules)
    {
        for (const auto& targetName: std::views::keys(module_entry.Targets))
        {
            if (!std::ranges::contains(target_names, targetName))
            {
                const std::string_view nearest = NearestName(targetName, target_names);
                return ReportError(sink,
                                   CookError::PolicyUnknownTargetName,
                                   std::format("module '{}' has a section for unknown target '{}'{}",
                                               module_name,
                                               targetName,
                                               nearest.empty() ? std::string{}
                                                               : std::format(". Did you mean '{}'?", nearest)));
            }
        }
    }

    return CookError::Success;
}

namespace
{
    PolicyParseError ErrorAt(const toml::node& node, std::string message)
    {
        const toml::source_region& region = node.source();
        return PolicyParseError{ .Message = std::move(message),
                                 .File = FileOf(region),
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

    PolicyDocResult<StringMap<std::vector<PolicyInterfaceType>>> ReadInterfaceAxes(const toml::table& module_table)
    {
        StringMap<std::vector<PolicyInterfaceType>> interfaceAxes;

        const auto node = module_table["axes"];
        if (!node)
        {
            return interfaceAxes;
        }

        const toml::table* table = node.as_table();
        if (table == nullptr)
        {
            return std::unexpected(ErrorAt(*node.node(), "axes must be a table"));
        }

        for (auto&& [axisKey, axisNode] : *table)
        {
            const toml::table* axisTable = axisNode.as_table();
            if (axisTable == nullptr)
            {
                return std::unexpected(ErrorAt(axisNode, std::format("axes.{} must be a table", axisKey.str())));
            }

            PolicyDocResult<std::vector<PolicyInterfaceType>> types = ReadInterfaceTypes(axisKey.str(), *axisTable);
            if (!types)
            {
                return std::unexpected(std::move(types.error()));
            }
            interfaceAxes.emplace(std::string{ axisKey.str() }, std::move(*types));
        }

        return interfaceAxes;
    }

    PolicyDocResult<std::vector<PolicyInterfaceType>> ReadInterfaceTypes(std::string_view axis_name,
                                                                         const toml::table& axis_table)
    {
        for (auto&& [key, node] : axis_table)
        {
            if (key.str() != "Types")
            {
                return std::unexpected(ErrorAt(node,
                                               std::format("axes.{} has the unknown key '{}'. The only key is Types",
                                                           axis_name,
                                                           key.str())));
            }
        }

        const auto typesNode = axis_table["Types"];
        const toml::array* typesArray = typesNode.as_array();
        if (typesArray == nullptr)
        {
            return std::unexpected(ErrorAt(axis_table,
                                           std::format("axes.{}.Types must be an array of tables", axis_name)));
        }

        std::vector<PolicyInterfaceType> types;
        types.reserve(typesArray->size());
        for (const toml::node& element : *typesArray)
        {
            PolicyDocResult<PolicyInterfaceType> type = ReadInterfaceType(axis_name, element);
            if (!type)
            {
                return std::unexpected(std::move(type.error()));
            }
            types.push_back(std::move(*type));
        }

        return types;
    }

    PolicyDocResult<PolicyInterfaceType> ReadInterfaceType(std::string_view axis_name, const toml::node& type_node)
    {
        const toml::table* typeTable = type_node.as_table();
        if (typeTable == nullptr)
        {
            return std::unexpected(
                ErrorAt(type_node, std::format("axes.{}.Types entries must be tables with Type and Imports", axis_name)));
        }

        for (auto&& [key, node] : *typeTable)
        {
            if (key.str() != "Type" && key.str() != "Imports")
            {
                return std::unexpected(ErrorAt(node,
                                               std::format("axes.{}.Types has the unknown key '{}'. The keys are Type "
                                                           "and Imports",
                                                           axis_name,
                                                           key.str())));
            }
        }

        PolicyInterfaceType type;
        const toml::source_region& region = type_node.source();
        type.File = FileOf(region);
        type.Line = region.begin.line;

        const std::optional<std::string> typeText = (*typeTable)["Type"].value<std::string>();
        if (!typeText || typeText->find_first_not_of(" \t\r\n") == std::string::npos)
        {
            return std::unexpected(
                ErrorAt(type_node, std::format("axes.{}.Types entries need a non-empty string Type", axis_name)));
        }
        type.Type = *typeText;

        const auto importsNode = (*typeTable)["Imports"];
        if (!importsNode)
        {
            return type;
        }

        const toml::array* importsArray = importsNode.as_array();
        if (importsArray == nullptr)
        {
            return std::unexpected(
                ErrorAt(*importsNode.node(), std::format("axes.{}.Types Imports must be an array of strings", axis_name)));
        }

        for (const toml::node& element : *importsArray)
        {
            const std::optional<std::string> moduleName = element.value<std::string>();
            if (!moduleName)
            {
                return std::unexpected(ErrorAt(
                    element, std::format("axes.{}.Types Imports must be an array of strings", axis_name)));
            }
            type.Imports.push_back(*moduleName);
        }

        return type;
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
                if (val < 0)
                {
                    return std::unexpected(ErrorAt(
                        element, std::format("CookValues.{} entries must be non-negative integers", axis_name)));
                }
                cookValues.Values.emplace_back(static_cast<uint32_t>(val));
            }
            else if (element.is_string())
            {
                cookValues.ValueNames.emplace_back(*element.value<std::string>());
            }
            else
            {
                return std::unexpected(ErrorAt(
                    element, std::format("CookValues.{} entries must be integers, booleans, enum case names, or interface type names", axis_name)));
            }
        }

        return cookValues;
    }

    PolicyDocResult<TargetCookPolicy> ReadTargetPolicy(const toml::table& target_table)
    {
        TargetCookPolicy policy;

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

    PolicyDocResult<StringMap<TargetCookPolicy>> ReadTargets(const toml::table& module_table)
    {
        StringMap<TargetCookPolicy> targets;

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

            PolicyDocResult<TargetCookPolicy> policy = ReadTargetPolicy(*targetTable);
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
                                 .File = FileOf(region),
                                 .Line = region.begin.line,
                                 .Column = region.begin.column };
    }

    std::string FileOf(const toml::source_region& region)
    {
        return region.path != nullptr ? *region.path : std::string{};
    }

    void MoveNodeInto(toml::table& destination, std::string_view key, toml::node& node)
    {
        // moving keeps the source region of each node, which a later conflict message reads
        node.visit([&destination, key](auto& element) { static_cast<void>(destination.insert(key, std::move(element))); });
    }

    PolicyParseError MakeKeyConflict(const toml::node& existing, const toml::node& incoming, std::string_view dotted_key)
    {
        const toml::source_region& existingRegion = existing.source();
        const toml::source_region& incomingRegion = incoming.source();
        return PolicyParseError{ .Message = std::format("key '{}' is set in '{}' at line {} and in '{}' at line {}",
                                                        dotted_key,
                                                        FileOf(existingRegion),
                                                        existingRegion.begin.line,
                                                        FileOf(incomingRegion),
                                                        incomingRegion.begin.line),
                                 .File = FileOf(incomingRegion),
                                 .Code = CookError::PolicyKeyConflict,
                                 .Line = incomingRegion.begin.line,
                                 .Column = incomingRegion.begin.column };
    }

    // NOLINTBEGIN(misc-no-recursion)
    std::optional<PolicyParseError> MergeTables(toml::table& destination,
                                                toml::table& source,
                                                std::string_view key_prefix)
    {
        for (auto&& [key, node] : source)
        {
            const std::string dottedKey = key_prefix.empty() ? std::string{ key.str() }
                                                             : std::format("{}.{}", key_prefix, key.str());

            toml::node* existing = destination.get(key.str());
            if (existing == nullptr)
            {
                MoveNodeInto(destination, key.str(), node);
                continue;
            }

            toml::table* existingTable = existing->as_table();
            toml::table* incomingTable = node.as_table();
            if (existingTable == nullptr || incomingTable == nullptr)
            {
                return MakeKeyConflict(*existing, node, dottedKey);
            }

            std::optional<PolicyParseError> nested = MergeTables(*existingTable, *incomingTable, dottedKey);
            if (nested)
            {
                return nested;
            }
        }

        return std::nullopt;
    }
    // NOLINTEND(misc-no-recursion)

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

            PolicyDocResult<StringMap<std::vector<PolicyInterfaceType>>> interfaceAxes = ReadInterfaceAxes(*moduleTable);
            if (!interfaceAxes)
            {
                return std::unexpected(std::move(interfaceAxes.error()));
            }
            entry.InterfaceAxes = std::move(*interfaceAxes);

            PolicyDocResult<StringMap<TargetCookPolicy>> targets = ReadTargets(*moduleTable);
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

    CookError ValidateTargetAndAxisValues(const std::string_view module_name,
                                          const StringMap<TargetCookPolicy>& targets,
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
                    return ReportError(sink,
                                       CookError::PolicyAxisNotDeclared,
                                       std::format("target '{}' CookValues names axis '{}', which module '{}' does not declare",
                                       targetName,
                                       cookValues.Axis,
                                       module_name));
                }

                for (const std::string& valueName : cookValues.ValueNames)
                {
                    if (axis->ValueDomain != AxisValueDomain::Enum && axis->ValueDomain != AxisValueDomain::Type)
                    {
                        return ReportError(sink,
                                           CookError::PolicyValueNotInAxis,
                                           std::format("target '{}' CookValues for axis '{}' lists '{}', and only an enum "
                                                       "axis takes case names and only an interface axis takes type "
                                                       "names",
                                                       targetName,
                                                       cookValues.Axis,
                                                       valueName));
                    }

                    const std::optional<PermutationValue> named = axis->FindNamedValue(valueName);
                    if (!named.has_value() || !AxisDeclaresValue(*axis, *named))
                    {
                        std::vector<std::string_view> declaredNames(axis->NumValues());
                        for (const auto&&[idx, value] : axis->GetValues() | std::views::enumerate)
                        {
                            declaredNames[idx] = axis->NamedValueName(value);
                        }
                        
                        const std::string_view nearest = NearestName(RemoveWhitespace(valueName), declaredNames);
                        return ReportError(sink,
                                           CookError::PolicyValueNotInAxis,
                                           std::format("target '{}' CookValues for axis '{}' lists '{}', which the axis "
                                                       "does not declare{}",
                                                       targetName,
                                                       cookValues.Axis,
                                                       valueName,
                                                       nearest.empty() ? std::string{}
                                                                       : std::format(". Did you mean '{}'?", nearest)));
                    }
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
