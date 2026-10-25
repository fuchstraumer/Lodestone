#include "compile/ShimLinkSource.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestIndex.hpp"
#include "compile/RawLibrary.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "target/ShimTable.hpp"
#include "target/StorageFormatTable.hpp"
#include "target/TargetProfile.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <expected>
#include <format>
#include <iterator>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{
    constexpr std::string_view k_ShimModuleName = "LodestoneShims";
    constexpr std::string_view k_StorageFormatEnumName = "StorageFormat";

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

    ShaderLocation LocationOf(const RawShimExtern& shim_extern) noexcept
    {
        return ShaderLocation{ .File = shim_extern.SourceFile, .Line = shim_extern.SourceLine };
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
                                           const ShimForms& forms,
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

        const AccessModel access = ShimFormOf(forms, shimInterface->Class);
        const std::string_view implName = ShimImplName(*shimInterface, access);
        if (implName.empty())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ShimFormUnavailable,
                                               LocationOf(shimExtern),
                                               std::format("{} has no {} implementation for profile '{}'",
                                                           DescribeShimExtern(shimExtern),
                                                           ToString(access),
                                                           profile_name)));
        }

        // If this shim interface needs a format, make sure that the externs provide formats!
        if (shimInterface->ImplementationTakesFormat != !shimExtern.FormatAxis.empty())
        {
            const bool missing = shimInterface->ImplementationTakesFormat;
            return std::unexpected(ReportError(sink,
                                               missing ? CookError::ShimStorageFormatMissing : CookError::ShimExternInvalid,
                                               LocationOf(shimExtern),
                                               std::format("{} {}",
                                                           DescribeShimExtern(shimExtern),
                                                           missing ? "needs [ls_storage_format(\"AXIS\")] to name the axis "
                                                                     "of its storage format"
                                                                   : "takes no storage format, so [ls_storage_format] does "
                                                                     "not apply")));
        }

        if (shimInterface->ImplementationTakesFormat)
        {
            continue;
        }

        const std::vector<std::string_view> arguments = SplitShimTypeArguments(shimExtern.TypeArguments);
        if (arguments.size() != shimInterface->TypeParameterCount ||
            shimExtern.ArgumentModules.size() != arguments.size())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ShimExternInvalid,
                                               LocationOf(shimExtern),
                                               std::format("{} needs {} plain type name(s) as type arguments",
                                                           DescribeShimExtern(shimExtern),
                                                           shimInterface->TypeParameterCount)));
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

    if (exports.empty())
    {
        return std::string{};
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

CookResult<std::vector<ShimFormatLinkSource>> MakeShimFormatLinkSources(std::span<const RawShimExtern> externs,
                                                                        const PermutationSpace& space,
                                                                        const ShimForms& forms,
                                                                        std::string_view module_name,
                                                                        DiagnosticSink& sink)
{
    std::vector<ShimFormatLinkSource> sources;
    std::vector<const RawShimExtern*> firstExternOfAxis;
    for (const RawShimExtern& shimExtern : externs)
    {
        if (!shimExtern.FormatAxis.empty() &&
            std::ranges::none_of(firstExternOfAxis,
                                 [&](const RawShimExtern* first) { return first->FormatAxis == shimExtern.FormatAxis; }))
        {
            firstExternOfAxis.push_back(&shimExtern);
        }
    }

    for (const RawShimExtern* first : firstExternOfAxis)
    {
        const std::span<const PermutationAxis> axes = space.Axes();
        const auto axis = std::ranges::find(axes, first->FormatAxis, &PermutationAxis::Name);
        if (axis == axes.end())
        {
            std::vector<std::string_view> axisNames;
            std::ranges::transform(axes,
                                   std::back_inserter(axisNames),
                                   [](const PermutationAxis& candidate) -> std::string_view { return candidate.Name; });
                                   
            const std::string_view nearest = NearestName(first->FormatAxis, axisNames);
            return std::unexpected(ReportError(sink,
                                               CookError::ShimStorageFormatAxisUnknown,
                                               LocationOf(*first),
                                               std::format("{} names the storage format axis '{}', and module {} declares "
                                                           "no axis of that name{}",
                                                           DescribeShimExtern(*first),
                                                           first->FormatAxis,
                                                           module_name,
                                                           nearest.empty() ? std::string{}
                                                                           : std::format(". Did you mean '{}'?", nearest))));
        }

        // a gated axis has no value when the gate is off
        if (axis->ValueDomain != AxisValueDomain::Enum || axis->EnumTypeName() != k_StorageFormatEnumName ||
            axis->Module() != k_ShimModuleName || !axis->ActiveWhen.empty())
        {
            return std::unexpected(ReportError(sink,
                                               CookError::ShimStorageFormatAxisInvalid,
                                               LocationOf(*first),
                                               std::format("{} names the axis '{}', which must be an [ls_axis_enum] axis "
                                                           "of type {}.{} with no ls_axis_active_when",
                                                           DescribeShimExtern(*first),
                                                           first->FormatAxis,
                                                           k_ShimModuleName,
                                                           k_StorageFormatEnumName)));
        }

        for (const PermutationValue& value : axis->GetValues())
        {
            // StorageFormatTest proves that every case has a row
            const StorageFormatInfo* format = FindStorageFormat(axis->EnumCaseName(value.AsUInt()));
            std::string source = std::format("import {};\n", k_ShimModuleName);
            for (const RawShimExtern& shimExtern : externs)
            {
                if (shimExtern.FormatAxis != first->FormatAxis)
                {
                    continue;
                }

                const ShimInterface* shimInterface = FindShimInterface(shimExtern.Interface);
                source += std::format("export struct {} : {} = {}<{}>;\n",
                                      shimExtern.Name,
                                      shimExtern.Interface,
                                      ShimImplName(*shimInterface, ShimFormOf(forms, shimInterface->Class)),
                                      format->SlangType);
            }
            sources.push_back(ShimFormatLinkSource{ .AxisName = first->FormatAxis,
                                                    .CaseOrdinal = value.AsUInt(),
                                                    .Source = std::move(source) });
        }
    }

    return sources;
}

} // namespace lodestone
