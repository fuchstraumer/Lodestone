#include "compile/ShimLinkSource.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "target/ShimTable.hpp"
#include "target/TargetProfile.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <expected>
#include <format>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

namespace
{
    constexpr std::string_view k_ShimModuleName = "LodestoneShims";

    std::string_view Trim(std::string_view text) noexcept
    {
        constexpr std::string_view k_Whitespace = " \t\r\n";

        const size_t first = text.find_first_not_of(k_Whitespace);
        if (first == std::string_view::npos)
        {
            return {};
        }

        return text.substr(first, text.find_last_not_of(k_Whitespace) - first + 1u);
    }

    bool IsPlainIdentifier(std::string_view text) noexcept
    {
        if (text.empty() || std::isdigit(static_cast<unsigned char>(text.front())) != 0)
        {
            return false;
        }
        return std::ranges::all_of(text,
                                   [](char character)
                                   { return std::isalnum(static_cast<unsigned char>(character)) != 0 || character == '_'; });
    }

    std::string Spell(std::string_view name, std::string_view type_arguments)
    {
        return type_arguments.empty() ? std::string{ name } : std::format("{}<{}>", name, type_arguments);
    }

    std::string Join(const std::vector<std::string_view>& parts)
    {
        std::string joined;
        for (const std::string_view part : parts)
        {
            joined += joined.empty() ? "" : ", ";
            joined += part;
        }
        return joined;
    }
}

std::vector<std::string_view> SplitShimTypeArguments(std::string_view type_arguments)
{
    std::vector<std::string_view> arguments;
    if (Trim(type_arguments).empty())
    {
        return arguments;
    }

    for (auto part : type_arguments | std::views::split(','))
    {
        const std::string_view argument = Trim(std::string_view{ part.begin(), part.end() });
        if (!IsPlainIdentifier(argument))
        {
            return {};
        }

        arguments.push_back(argument);
    }
    return arguments;
}

CookResult<std::string> MakeShimLinkSource(std::span<const RawShimExtern> externs,
                                           AccessModel access,
                                           std::string_view module_name,
                                           std::string_view profile_name,
                                           DiagnosticSink& sink)
{
    if (externs.empty())
    {
        return std::string{};
    }

    std::set<std::string> imports;
    std::string exports;
    for (const RawShimExtern& shimExtern : externs)
    {
        const ShimInterface* shimInterface = FindShimInterface(shimExtern.Interface);
        if (shimInterface == nullptr)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ShimExternInvalid,
                                               std::format("module {}: '{}' is not a shim interface", module_name, shimExtern.Interface)));
        }

        const std::string_view implName = ShimImplName(*shimInterface, access);
        if (implName.empty())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ShimFormUnavailable,
                                               std::format("module {}: extern '{}' of '{}' has no {} implementation for profile '{}'",
                                                           module_name,
                                                           shimExtern.Name,
                                                           shimExtern.Interface,
                                                           ToString(access),
                                                           profile_name)));
        }

        const std::vector<std::string_view> arguments = SplitShimTypeArguments(shimExtern.TypeArguments);
        if (arguments.size() != shimInterface->TypeParameterCount ||
            shimExtern.ArgumentModules.size() != arguments.size())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ShimExternInvalid,
                                               std::format("{}({}): extern '{}' of '{}' needs {} plain type name(s) as type "
                                                           "arguments, and its declaration has '{}'",
                                                           shimExtern.SourceFile,
                                                           shimExtern.SourceLine,
                                                           shimExtern.Name,
                                                           shimExtern.Interface,
                                                           shimInterface->TypeParameterCount,
                                                           shimExtern.TypeArguments)));
        }

        for (const std::string& argumentModule : shimExtern.ArgumentModules)
        {
            if (!argumentModule.empty())
            {
                imports.insert(argumentModule);
            }
        }

        const std::string spelledArguments = Join(arguments);
        exports += std::format("export struct {} : {} = {};\n",
                               shimExtern.Name,
                               Spell(shimExtern.Interface, spelledArguments),
                               Spell(implName, spelledArguments));
    }

    std::string source = std::format("import {};\n", k_ShimModuleName);
    for (const std::string& importedModule : imports)
    {
        if (importedModule != k_ShimModuleName)
        {
            source += std::format("import {};\n", importedModule);
        }
    }

    source += exports;
    
    return source;
}

} // namespace lodestone
