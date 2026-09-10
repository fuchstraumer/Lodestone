#include "permute/PolicyDocument.hpp"

#include <cstdint>
#include <expected>
#include <format>
#include <optional>
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
    PolicyParseError ErrorAt(const toml::node& node, std::string message)
    {
        const toml::source_region& region = node.source();
        return PolicyParseError{ .Message = std::move(message),
                                 .Line = region.begin.line,
                                 .Column = region.begin.column };
    }

    // Reads the optional ExpectedInfluence array of one module. Each entry is a table with an
    // EntryPoint, an Axis, and an optional Inert flag that defaults to true.
    std::expected<std::vector<PolicyInfluence>, PolicyParseError> ReadInfluence(const toml::table& module_table)
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

            influence.push_back(PolicyInfluence{ .EntryPoint = *entryPoint,
                                                 .Axis = *axis,
                                                 .IsInert = (*record)["Inert"].value<bool>().value_or(true) });
        }

        return influence;
    }

    // Reads one axis value list of a CookValues table. A TOML integer is int64, which is the type the
    // evaluator reads, so no conversion is needed. A boolean maps to 0 or 1, so a boolean axis can list
    // its values.
    std::expected<AxisCookValues, PolicyParseError> ReadAxisCookValues(std::string_view axis_name,
                                                                       const toml::node& values_node)
    {
        const toml::array* values = values_node.as_array();
        if (values == nullptr)
        {
            return std::unexpected(ErrorAt(values_node, std::format("CookValues.{} must be an array", axis_name)));
        }

        AxisCookValues cookValues;
        cookValues.Axis = std::string{ axis_name };
        for (const toml::node& element : *values)
        {
            if (const std::optional<int64_t> asInt = element.value<int64_t>())
            {
                cookValues.Values.push_back(*asInt);
            }
            else if (const std::optional<bool> asBool = element.value<bool>())
            {
                cookValues.Values.push_back(*asBool ? 1 : 0);
            }
            else
            {
                return std::unexpected(
                    ErrorAt(element, std::format("CookValues.{} entries must be integers or booleans", axis_name)));
            }
        }

        return cookValues;
    }

    // Reads one target section: MaxVariants, CookWhen, and the CookValues table. Every key is optional.
    std::expected<TargetPolicy, PolicyParseError> ReadTargetPolicy(const toml::table& target_table)
    {
        TargetPolicy policy;

        if (const auto maxVariants = target_table["MaxVariants"])
        {
            const std::optional<int64_t> value = maxVariants.value<int64_t>();
            if (!value || *value < 0)
            {
                return std::unexpected(ErrorAt(*maxVariants.node(), "MaxVariants must be a non-negative integer"));
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
                return std::unexpected(ErrorAt(*cookValues.node(), "CookValues must be a table of axis arrays"));
            }
            for (auto&& [axisKey, valuesNode] : *table)
            {
                std::expected<AxisCookValues, PolicyParseError> axisValues =
                    ReadAxisCookValues(axisKey.str(), valuesNode);
                if (!axisValues)
                {
                    return std::unexpected(std::move(axisValues.error()));
                }
                policy.CookValues.push_back(std::move(*axisValues));
            }
        }

        return policy;
    }

    // Reads the optional targets table of one module. Each key is a target profile name.
    std::expected<StringMap<TargetPolicy>, PolicyParseError> ReadTargets(const toml::table& module_table)
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

            std::expected<TargetPolicy, PolicyParseError> policy = ReadTargetPolicy(*targetTable);
            if (!policy)
            {
                return std::unexpected(std::move(policy.error()));
            }
            targets.emplace(std::string{ targetKey.str() }, std::move(*policy));
        }

        return targets;
    }

} // namespace

std::expected<PolicyDocument, PolicyParseError> PolicyDocument::Load(std::string_view path)
{
    const toml::parse_result result = toml::parse_file(path);
    if (result.failed())
    {
        const toml::parse_error& error = result.error();
        const toml::source_region& region = error.source();
        return std::unexpected(PolicyParseError{ .Message = std::string{ error.description() },
                                                 .Line = region.begin.line,
                                                 .Column = region.begin.column });
    }

    const toml::table& root = result.table();
    PolicyDocument document;
    for (auto&& [moduleKey, moduleNode] : root)
    {
        const toml::table* moduleTable = moduleNode.as_table();
        if (moduleTable == nullptr)
        {
            return std::unexpected(
                ErrorAt(moduleNode, std::format("top-level entry '{}' must be a table", moduleKey.str())));
        }

        ModulePolicyEntry entry;

        std::expected<std::vector<PolicyInfluence>, PolicyParseError> influence = ReadInfluence(*moduleTable);
        if (!influence)
        {
            return std::unexpected(std::move(influence.error()));
        }
        entry.ExpectedInfluence = std::move(*influence);

        std::expected<StringMap<TargetPolicy>, PolicyParseError> targets = ReadTargets(*moduleTable);
        if (!targets)
        {
            return std::unexpected(std::move(targets.error()));
        }
        entry.Targets = std::move(*targets);

        document.modules.emplace(std::string{ moduleKey.str() }, std::move(entry));
    }

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

} // namespace lodestone
