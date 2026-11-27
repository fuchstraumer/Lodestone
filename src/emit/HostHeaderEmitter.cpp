#include "emit/HostHeaderEmitter.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "model/CookedLibrary.hpp"
#include "model/HostTypeTable.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "ShaderLibraryTypes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{

    /** One C++ struct. Two layouts of one Slang name are two of these. `Layout.Rule` is always `Std140` and means
     * nothing here, so that equal layouts compare equal. A nested field stores an index into the catalog. */
    struct ConcreteStruct
    {
        std::string SlangName;
        std::string Identifier;
        HostStruct Layout;
    };

    /** Maps a profile index and a layout rule to the index of a concrete struct. */
    using LayoutSelection = std::map<std::pair<size_t, LayoutRule>, uint32_t>;

    struct HostTypeCatalog
    {
        /** A struct comes after the structs that its fields use. */
        std::vector<ConcreteStruct> Structs;
        /** One entry for each environment: the concrete index of each struct of the module table. */
        std::vector<std::vector<uint32_t>> EnvironmentRemaps;
        /** Keyed by Slang name. */
        std::map<std::string, LayoutSelection> SelectionsByName;
        /** Maps each C++ identifier to the Slang name that produced it. */
        std::map<std::string, std::string> NameOfIdentifier;
        /** The `CookProfile` enumerator of each profile. */
        std::vector<std::string> ProfileEnumerators;
    };

    struct BaseType
    {
        std::string Type;
        uint32_t Size{ 0u };
        std::string Comment;
        /** A padded matrix is an array of its major vectors. */
        std::optional<HostArrayDimension> MajorVectors;
    };

    struct FieldCode
    {
        std::string Type;
        /** The member name and the array dimensions. */
        std::string Declarator;
        uint32_t Size{ 0u };
        std::string Comment;
    };

    /** For each axis of one environment, the groups of variants that agree on every other axis. */
    using AxisClasses = std::vector<std::vector<std::vector<size_t>>>;

    /** A binding of the pipeline layout of one variant. `Position` is the index in the resource list of the variant. */
    struct UsedBinding
    {
        std::string_view Name;
        uint32_t Position{ 0u };
        const ReflectedBinding* Resource{ nullptr };
    };

    struct BindingPresence
    {
        /** For each profile, the variant count of the module, or 0 when the module was not cooked for it. */
        std::vector<size_t> VariantCounts;
        /** For each binding name, the count of variants that use it, for each profile. */
        std::map<std::string, std::vector<size_t>> VariantsWithBinding;
        std::map<std::string, std::set<std::string>> AxesChangingBinding;
    };

    /** What the host header says about the type of one binding. */
    struct BindingTypeFacts
    {
        std::set<std::string> Kinds;
        std::set<std::string> Accesses;
        std::set<std::string> StructNames;
        /** For each profile, the concrete structs that the binding has in any variant. */
        std::map<size_t, std::set<uint32_t>> StructsByProfile;
        /** The axes that change which struct the binding has. */
        std::set<std::string> StructAxes;
    };

    struct NumThreadsFacts
    {
        bool IsCompute{ false };
        std::set<std::array<uint32_t, 3>> Values;
        std::set<std::string> Axes;
        /** As written in the source. Empty strings when the source text has no readable attribute. */
        std::array<std::string, 3> Expressions;
    };

    struct EntryPointFacts
    {
        std::string Name;
        ShaderStageKind Stage{ ShaderStageKind::Invalid };
        BindingPresence Presence;
        std::set<std::string> EveryVariant;
        std::set<std::string> SomeVariants;
        std::map<std::string, BindingTypeFacts> Bindings;
        /** The concrete struct of the entry point constants, for each profile that has any. */
        std::map<size_t, uint32_t> Params;
        NumThreadsFacts NumThreads;
    };

    struct ModuleFacts
    {
        std::string Name;
        std::string Identifier;
        std::vector<EntryPointFacts> EntryPoints;
        /** The binding names of every entry point and variant. */
        std::set<std::string> BindingNames;
    };

    struct EmitContext
    {
        const CookedLibrary& Library;
        const HostTypeCatalog& Catalog;
        /** One entry for each environment. */
        const std::vector<AxisClasses>& Classes;
        std::string QualifiedPrefix;
        DiagnosticSink& Sink;
    };

    std::string MakeIdentifier(std::string_view name);
    std::string MakeProfileEnumerator(std::string_view target_name);
    bool IsCppKeyword(std::string_view word) noexcept;
    bool IsIdentifier(std::string_view text) noexcept;
    uint32_t ScalarSizeOf(HostScalarType type) noexcept;
    std::string_view ScalarTypeName(HostScalarType type) noexcept;
    std::string_view VectorElementName(HostScalarType type) noexcept;
    std::string MakeArraySuffix(std::span<const uint32_t> counts);
    std::string MakeQualifiedPrefix(std::string_view host_namespace);
    std::string JoinSet(const std::set<std::string>& items, std::string_view separator);

    uint32_t FindOrAddConcreteStruct(HostTypeCatalog& catalog,
                                     const HostStruct& host_struct,
                                     std::span<const uint32_t> remap);
    void AssignIdentifiers(HostTypeCatalog& catalog);
    CookError CheckIdentifiers(HostTypeCatalog& catalog, DiagnosticSink& sink);
    CookResult<HostTypeCatalog> BuildHostTypeCatalog(const CookedLibrary& library, DiagnosticSink& sink);

    CookResult<BaseType> DescribeMatrix(const HostField& field,
                                        std::string_view struct_name,
                                        DiagnosticSink& sink);
    CookResult<BaseType> DescribeBaseType(const HostField& field,
                                          std::span<const ConcreteStruct> structs,
                                          std::string_view struct_name,
                                          DiagnosticSink& sink);
    CookResult<FieldCode> MakeFieldCode(const HostField& field,
                                        std::span<const ConcreteStruct> structs,
                                        std::string_view struct_name,
                                        DiagnosticSink& sink);
    void AppendPadding(std::string& text, uint32_t& pad_count, uint32_t byte_count);
    CookError AppendConcreteStruct(std::string& text,
                                   std::span<const ConcreteStruct> structs,
                                   const ConcreteStruct& concrete,
                                   DiagnosticSink& sink);

    void AppendProfileEnum(std::string& text, const CookedLibrary& library);

    const std::optional<CookedModule>& EnvironmentOf(const CookedLibrary& library,
                                                     size_t profile_index,
                                                     size_t module_index) noexcept;
    const CookedModule* FindFirstEnvironment(const CookedLibrary& library, size_t module_index) noexcept;
    AxisClasses BuildAxisClasses(const CookedModule& module);
    std::vector<AxisClasses> BuildEnvironmentClasses(const CookedLibrary& library);
    std::set<std::string> AxesWhereClassesDiffer(const CookedModule& module,
                                                 const AxisClasses& classes,
                                                 std::span<const std::string> fingerprints);
    std::vector<UsedBinding> UsedBindings(const CookedModule& module,
                                          const LibraryVariant& variant,
                                          size_t entry_point_index);
    void AddEnvironmentPresence(const CookedModule& module,
                                const AxisClasses& classes,
                                size_t entry_point_index,
                                size_t profile_index,
                                BindingPresence& presence);
    void AddEnvironmentBindingFacts(const CookedModule& module,
                                    const AxisClasses& classes,
                                    const HostTypeCatalog& catalog,
                                    std::span<const uint32_t> remap,
                                    size_t entry_point_index,
                                    size_t profile_index,
                                    std::map<std::string, BindingTypeFacts>& bindings);
    void AddEnvironmentNumThreads(const CookedModule& module,
                                  const AxisClasses& classes,
                                  size_t entry_point_index,
                                  NumThreadsFacts& facts);
    bool LacksInSomeVariant(const BindingPresence& presence, const std::vector<size_t>& counts);
    std::string ListMissingProfiles(const CookedLibrary& library,
                                    const BindingPresence& presence,
                                    const std::vector<size_t>& counts);
    std::map<size_t, uint32_t> CollectParamsSelection(const EmitContext& context,
                                                      size_t module_index,
                                                      size_t entry_point_index);
    EntryPointFacts CollectEntryPointFacts(const EmitContext& context, size_t module_index, size_t entry_point_index);
    bool StructChangesWithAxes(const BindingTypeFacts& facts) noexcept;
    bool HasSingleStruct(const BindingTypeFacts& facts) noexcept;
    CookError CheckEntryPointNames(const EntryPointFacts& entry_point, DiagnosticSink& sink);
    CookError CheckModuleBindingNames(const ModuleFacts& module, DiagnosticSink& sink);
    CookResult<std::vector<ModuleFacts>> CollectModuleFacts(const EmitContext& context);

    std::string_view DescribeKind(BindingKind kind) noexcept;
    std::string_view DescribeAccess(const ReflectedBinding& binding) noexcept;
    bool IsLiteralInteger(std::string_view text) noexcept;
    std::string DescribeNumThreads(const NumThreadsFacts& facts);
    void AppendWrappedLine(std::string& text,
                           std::string_view first_prefix,
                           std::string_view next_prefix,
                           std::span<const std::string> pieces);
    std::string DescribeFieldForComment(const HostField& field, std::span<const ConcreteStruct> structs);
    void AppendParamsComment(std::string& text, const EmitContext& context, const EntryPointFacts& entry_point);
    void AppendBindingRows(std::string& text, const EmitContext& context, const EntryPointFacts& entry_point);
    void AppendEntryPointComment(std::string& text, const EmitContext& context, const EntryPointFacts& entry_point);

    void AppendTypeSelectDeclarations(std::string& text, const HostTypeCatalog& catalog);
    void AppendEntryPointSelectDeclarations(std::string& text, std::span<const ModuleFacts> modules);
    void AppendTypeAliases(std::string& text, const HostTypeCatalog& catalog);
    void AppendBindingConstant(std::string& text, std::string_view name);
    void AppendBindingUsing(std::string& text, std::string_view qualifier, std::string_view name);
    void AppendBindingTypeAlias(std::string& text, std::string_view name);
    void AppendBindingArray(std::string& text,
                            std::string_view array_name,
                            std::string_view scope,
                            const std::set<std::string>& names);
    void AppendEntryPoint(std::string& text,
                          const EmitContext& context,
                          std::string_view module_identifier,
                          const EntryPointFacts& entry_point);
    void AppendModules(std::string& text, const EmitContext& context, std::span<const ModuleFacts> modules);

    CookError AppendHostTypes(std::string& text, const HostTypeCatalog& catalog, DiagnosticSink& sink);
    void AppendEntryPointSelections(std::string& text, const EmitContext& context, std::span<const ModuleFacts> modules);

} // namespace

bool IsValidHostNamespace(std::string_view text) noexcept
{
    if (text.empty())
    {
        return false;
    }

    for (const auto segment : std::views::split(text, std::string_view{ "::" }))
    {
        const std::string_view segmentText{ segment.begin(), segment.end() };
        if (!IsIdentifier(segmentText) || IsCppKeyword(segmentText))
        {
            return false;
        }
    }

    return !text.starts_with("::") && !text.ends_with("::");
}

CookResult<HostHeaderFiles> EmitHostHeader(const CookedLibrary& library,
                                           std::string_view host_types_include,
                                           std::string_view host_namespace,
                                           std::string_view inline_name,
                                           DiagnosticSink& sink)
{
    CookResult<HostTypeCatalog> catalogResult = BuildHostTypeCatalog(library, sink);
    if (!catalogResult)
    {
        return std::unexpected(catalogResult.error());
    }

    const HostTypeCatalog& catalog = *catalogResult;
    const std::vector<AxisClasses> classes = BuildEnvironmentClasses(library);
    const EmitContext context{ .Library = library,
                               .Catalog = catalog,
                               .Classes = classes,
                               .QualifiedPrefix = MakeQualifiedPrefix(host_namespace),
                               .Sink = sink };

    CookResult<std::vector<ModuleFacts>> modulesResult = CollectModuleFacts(context);
    if (!modulesResult)
    {
        return std::unexpected(modulesResult.error());
    }

    const std::vector<ModuleFacts>& modules = *modulesResult;

    std::string profileList;
    for (const CookedProfile& profile : library.Profiles)
    {
        profileList += profileList.empty() ? "" : ", ";
        profileList += profile.TargetName;
    }

    std::string header;
    header += "// Generated by the lodestone cooker. Do not edit.\n";
    header += std::format("// Profiles: {}.\n", profileList);
    header += std::format("// The modules and entry points are below. The struct layouts are in {}, which this file\n"
                          "// includes at its end.\n",
                          inline_name);
    header += "#pragma once\n\n";
    header += "#include <array>\n#include <cstddef>\n#include <cstdint>\n#include <string_view>\n\n";
    header += "#include \"ShaderLibraryTypes.hpp\"\n";
    header += std::format("#include \"{}\"\n\n", host_types_include);
    header += std::format("namespace {}\n{{\n\n", host_namespace);

    AppendProfileEnum(header, library);
    AppendTypeSelectDeclarations(header, catalog);
    AppendEntryPointSelectDeclarations(header, modules);
    AppendTypeAliases(header, catalog);

    AppendModules(header, context, modules);
    header += std::format("}} // namespace {}\n\n", host_namespace);
    header += std::format("#include \"{}\"\n", inline_name);

    std::string inlineText;
    inlineText += "// Generated by the lodestone cooker. Do not edit.\n";
    inlineText += "// The struct layouts, their size and offset checks, and the selection of a type by profile. The host\n";
    inlineText += "// header of the same name includes this file at its end.\n\n";
    inlineText += std::format("namespace {}\n{{\n\n", host_namespace);

    const CookError typesError = AppendHostTypes(inlineText, catalog, sink);
    if (!typesError)
    {
        return std::unexpected(typesError);
    }

    AppendEntryPointSelections(inlineText, context, modules);
    inlineText += std::format("}} // namespace {}\n", host_namespace);

    return HostHeaderFiles{ .Header = std::move(header), .Inline = std::move(inlineText) };
}

namespace
{

    constexpr std::string_view k_CppKeywords[]{
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
        "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
        "const_cast", "consteval", "constexpr", "constinit", "continue", "co_await", "co_return", "co_yield",
        "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit", "export",
        "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "register", "reinterpret_cast", "requires", "return", "short",
        "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template",
        "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq"
    };

    bool IsCppKeyword(std::string_view word) noexcept
    {
        return std::ranges::find(k_CppKeywords, word) != std::end(k_CppKeywords);
    }

    bool IsIdentifier(std::string_view text) noexcept
    {
        if (text.empty() || (text.front() >= '0' && text.front() <= '9'))
        {
            return false;
        }

        return std::ranges::all_of(text,
                                   [](char character)
                                   {
                                       const bool isLetter = (character >= 'a' && character <= 'z') ||
                                                             (character >= 'A' && character <= 'Z');
                                       const bool isDigit = character >= '0' && character <= '9';
                                       return isLetter || isDigit || character == '_';
                                   });
    }

    std::string MakeIdentifier(std::string_view name)
    {
        std::string identifier;
        identifier.reserve(name.size() + 1u);
        for (const char character : name)
        {
            const bool isLetter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
            const bool isDigit = character >= '0' && character <= '9';
            if (isLetter || isDigit || character == '_')
            {
                identifier += character;
            }
            else if (!identifier.empty() && identifier.back() != '_')
            {
                identifier += '_';
            }
        }

        while (!identifier.empty() && identifier.back() == '_' && !name.ends_with('_'))
        {
            identifier.pop_back();
        }

        if (identifier.empty() || (identifier.front() >= '0' && identifier.front() <= '9'))
        {
            identifier.insert(identifier.begin(), '_');
        }

        if (IsCppKeyword(identifier))
        {
            identifier += '_';
        }

        return identifier;
    }

    std::string MakeProfileEnumerator(std::string_view target_name)
    {
        std::string enumerator;
        bool startOfWord = true;
        for (const char character : target_name)
        {
            if (character == '-' || character == '_')
            {
                startOfWord = true;
                continue;
            }

            const bool isLower = character >= 'a' && character <= 'z';
            enumerator += (startOfWord && isLower) ? static_cast<char>(character - 'a' + 'A') : character;
            startOfWord = false;
        }

        return enumerator;
    }

    uint32_t ScalarSizeOf(HostScalarType type) noexcept
    {
        switch (type)
        {
        case HostScalarType::Int8:
        case HostScalarType::UInt8:
            return 1u;
        case HostScalarType::Int16:
        case HostScalarType::UInt16:
        case HostScalarType::Float16:
            return 2u;
        case HostScalarType::Bool:
        case HostScalarType::Int32:
        case HostScalarType::UInt32:
        case HostScalarType::Float32:
            return 4u;
        case HostScalarType::Int64:
        case HostScalarType::UInt64:
        case HostScalarType::Float64:
            return 8u;
        case HostScalarType::Invalid:
            break;
        }

        return 0u;
    }

    std::string_view ScalarTypeName(HostScalarType type) noexcept
    {
        switch (type)
        {
        case HostScalarType::Bool:
            return "uint32_t";
        case HostScalarType::Int8:
            return "int8_t";
        case HostScalarType::UInt8:
            return "uint8_t";
        case HostScalarType::Int16:
            return "int16_t";
        case HostScalarType::UInt16:
            return "uint16_t";
        case HostScalarType::Int32:
            return "int32_t";
        case HostScalarType::UInt32:
            return "uint32_t";
        case HostScalarType::Int64:
            return "int64_t";
        case HostScalarType::UInt64:
            return "uint64_t";
        case HostScalarType::Float16:
            return "half";
        case HostScalarType::Float32:
            return "float";
        case HostScalarType::Float64:
            return "double";
        case HostScalarType::Invalid:
            break;
        }

        return "";
    }

    /** The prefix of a host vector or matrix name, as in `float3` or `uint2`. */
    std::string_view VectorElementName(HostScalarType type) noexcept
    {
        switch (type)
        {
        case HostScalarType::Bool:
        case HostScalarType::UInt32:
            return "uint";
        case HostScalarType::Int32:
            return "int";
        default:
            return ScalarTypeName(type);
        }
    }

    std::string MakeArraySuffix(std::span<const uint32_t> counts)
    {
        std::string suffix;
        for (const uint32_t count : counts)
        {
            suffix += std::format("[{}]", count);
        }

        return suffix;
    }

    std::string MakeQualifiedPrefix(std::string_view host_namespace)
    {
        return std::format("::{}::", host_namespace);
    }

    std::string JoinSet(const std::set<std::string>& items, std::string_view separator)
    {
        std::string text;
        for (const std::string& item : items)
        {
            text += text.empty() ? "" : separator;
            text += item;
        }

        return text;
    }

    uint32_t FindOrAddConcreteStruct(HostTypeCatalog& catalog,
                                     const HostStruct& host_struct,
                                     std::span<const uint32_t> remap)
    {
        HostStruct layout = host_struct;
        layout.Rule = LayoutRule::Std140;
        for (HostField& field : layout.Fields)
        {
            if (field.Kind == HostFieldKind::Struct)
            {
                field.StructIndex = remap[field.StructIndex];
            }
        }

        for (auto&& [index, concrete] : std::views::enumerate(catalog.Structs))
        {
            if (concrete.SlangName == layout.Name && concrete.Layout == layout)
            {
                return static_cast<uint32_t>(index);
            }
        }

        const uint32_t newIndex = static_cast<uint32_t>(catalog.Structs.size());
        catalog.Structs.push_back(
            ConcreteStruct{ .SlangName = layout.Name, .Identifier = {}, .Layout = std::move(layout) });

        return newIndex;
    }

    void AssignIdentifiers(HostTypeCatalog& catalog)
    {
        std::map<std::string, std::vector<uint32_t>> layoutsOfName;
        for (auto&& [index, concrete] : std::views::enumerate(catalog.Structs))
        {
            layoutsOfName[concrete.SlangName].push_back(static_cast<uint32_t>(index));
        }

        for (const auto& [slangName, layouts] : layoutsOfName)
        {
            const std::string identifier = MakeIdentifier(slangName);
            if (layouts.size() == 1u)
            {
                catalog.Structs[layouts.front()].Identifier = identifier;
                continue;
            }

            // the layout rule alone separates the layouts when no rule selects two of them
            const LayoutSelection& selection = catalog.SelectionsByName.at(slangName);
            std::map<LayoutRule, std::set<uint32_t>> layoutsOfRule;
            for (const auto& [key, concreteIndex] : selection)
            {
                layoutsOfRule[key.second].insert(concreteIndex);
            }

            const bool ruleSeparates = std::ranges::all_of(
                layoutsOfRule, [](const auto& entry) { return entry.second.size() == 1u; });

            // each layout takes the first profile and rule that select it, and (name, profile, rule) has one layout
            for (const uint32_t layoutIndex : layouts)
            {
                const auto first = std::ranges::find_if(
                    selection, [layoutIndex](const auto& entry) { return entry.second == layoutIndex; });
                const auto& [profileIndex, rule] = first->first;
                const std::string suffix =
                    ruleSeparates ? std::string{ ToString(rule) }
                                  : std::format("{}_{}", catalog.ProfileEnumerators[profileIndex], ToString(rule));
                catalog.Structs[layoutIndex].Identifier = std::format("{}_{}", identifier, suffix);
            }
        }
    }

    CookError CheckIdentifiers(HostTypeCatalog& catalog, DiagnosticSink& sink)
    {
        // each name claims its alias, its selection struct, and its concrete structs, all of which share a namespace
        std::map<std::string, std::string> owners;
        const auto claim = [&owners, &sink](const std::string& identifier, const std::string& slang_name) -> CookError
        {
            const auto [position, inserted] = owners.try_emplace(identifier, slang_name);
            if (inserted || position->second == slang_name)
            {
                return CookError::Success;
            }

            return ReportError(sink,
                               CookError::HostTypeNameCollision,
                               std::format("host struct names '{}' and '{}' both map to the C++ identifier '{}'",
                                           position->second,
                                           slang_name,
                                           identifier));
        };

        for (const auto& [slangName, selection] : catalog.SelectionsByName)
        {
            const std::string identifier = MakeIdentifier(slangName);
            const CookError aliasError = claim(identifier, slangName);
            if (!aliasError)
            {
                return aliasError;
            }

            const CookError selectError = claim(identifier + "Select", slangName);
            if (!selectError)
            {
                return selectError;
            }

            catalog.NameOfIdentifier.emplace(identifier, slangName);
        }

        for (const ConcreteStruct& concrete : catalog.Structs)
        {
            const CookError concreteError = claim(concrete.Identifier, concrete.SlangName);
            if (!concreteError)
            {
                return concreteError;
            }
        }

        return CookError::Success;
    }

    CookResult<HostTypeCatalog> BuildHostTypeCatalog(const CookedLibrary& library, DiagnosticSink& sink)
    {
        HostTypeCatalog catalog;
        catalog.EnvironmentRemaps.resize(library.Environments.size());
        for (const CookedProfile& profile : library.Profiles)
        {
            catalog.ProfileEnumerators.push_back(MakeProfileEnumerator(profile.TargetName));
        }

        const size_t moduleCount = library.ModuleNames.size();

        for (auto&& [environmentIndex, environment] : std::views::enumerate(library.Environments))
        {
            if (!environment.has_value())
            {
                continue;
            }

            const size_t profileIndex = static_cast<size_t>(environmentIndex) / moduleCount;
            std::vector<uint32_t>& remap = catalog.EnvironmentRemaps[static_cast<size_t>(environmentIndex)];
            remap.reserve(environment->HostTypes.Structs.size());

            for (const HostStruct& hostStruct : environment->HostTypes.Structs)
            {
                const uint32_t concreteIndex = FindOrAddConcreteStruct(catalog, hostStruct, remap);
                remap.push_back(concreteIndex);
                catalog.SelectionsByName[hostStruct.Name].try_emplace(std::pair{ profileIndex, hostStruct.Rule },
                                                                     concreteIndex);
            }
        }

        AssignIdentifiers(catalog);

        const CookError identifierError = CheckIdentifiers(catalog, sink);
        if (!identifierError)
        {
            return std::unexpected(identifierError);
        }

        return catalog;
    }

    CookResult<BaseType> DescribeMatrix(const HostField& field, std::string_view struct_name, DiagnosticSink& sink)
    {
        const uint32_t scalarSize = ScalarSizeOf(field.ScalarType);
        const std::string_view elementName = VectorElementName(field.ScalarType);
        if (field.MatrixLayout == MatrixLayout::Invalid || scalarSize == 0u)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::HostTypeNotEmittable,
                                               std::format("member '{}' of host struct '{}' is a matrix with no "
                                                           "scalar type or matrix layout",
                                                           field.Name,
                                                           struct_name)));
        }

        const bool rowMajor = field.MatrixLayout == MatrixLayout::RowMajor;
        const uint32_t majorCount = rowMajor ? field.Rows : field.Columns;
        const uint32_t minorCount = rowMajor ? field.Columns : field.Rows;
        const uint32_t matrixSize = field.ArrayDimensions.empty() ? field.Size : field.ArrayDimensions.back().Stride;
        const std::string slangType = std::format("{}{}x{}", elementName, field.Rows, field.Columns);
        const std::string comment = std::format("{} {}", slangType, rowMajor ? "row major" : "column major");

        if (matrixSize == field.Rows * field.Columns * scalarSize)
        {
            return BaseType{ .Type = slangType, .Size = matrixSize, .Comment = {}, .MajorVectors = std::nullopt };
        }

        const uint32_t majorStride = majorCount == 0u ? 0u : matrixSize / majorCount;
        const uint32_t vectorWidth = majorStride / scalarSize;
        const bool stridesFit = majorCount != 0u && matrixSize % majorCount == 0u && majorStride % scalarSize == 0u &&
                                vectorWidth >= minorCount && vectorWidth <= 4u;
        if (!stridesFit)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::HostTypeNotEmittable,
                                               std::format("member '{}' of host struct '{}' is a {} of {} bytes, which "
                                                           "no array of host vectors mirrors",
                                                           field.Name,
                                                           struct_name,
                                                           comment,
                                                           matrixSize)));
        }

        return BaseType{ .Type = std::format("{}{}", elementName, vectorWidth),
                         .Size = majorStride,
                         .Comment = comment,
                         .MajorVectors = HostArrayDimension{ .Count = majorCount, .Stride = majorStride } };
    }

    CookResult<BaseType> DescribeBaseType(const HostField& field,
                                          std::span<const ConcreteStruct> structs,
                                          std::string_view struct_name,
                                          DiagnosticSink& sink)
    {
        if (field.Kind == HostFieldKind::Struct)
        {
            const ConcreteStruct& nested = structs[field.StructIndex];

            return BaseType{ .Type = nested.Identifier, .Size = nested.Layout.ByteSize, .Comment = {}, .MajorVectors = {} };
        }

        if (field.Kind == HostFieldKind::Matrix)
        {
            return DescribeMatrix(field, struct_name, sink);
        }

        const uint32_t scalarSize = ScalarSizeOf(field.ScalarType);
        const bool isVector = field.Kind == HostFieldKind::Vector;
        const uint32_t componentCount = isVector ? field.Columns : 1u;
        const bool isBool = field.ScalarType == HostScalarType::Bool;
        // the size of a bool is checkable only where no array padding hides it
        const bool boolSizeWrong = isBool && field.ArrayDimensions.empty() && field.Size != componentCount * 4u;
        if (scalarSize == 0u || boolSizeWrong)
        {
            return std::unexpected(ReportError(sink,
                                               CookError::HostTypeNotEmittable,
                                               std::format("member '{}' of host struct '{}' has no scalar type, or a "
                                                           "bool that is not 4 bytes",
                                                           field.Name,
                                                           struct_name)));
        }

        std::string comment = isBool ? "bool, 4 bytes in a shader" : std::string{};
        if (!isVector)
        {
            return BaseType{ .Type = std::string{ ScalarTypeName(field.ScalarType) },
                             .Size = scalarSize,
                             .Comment = std::move(comment),
                             .MajorVectors = {} };
        }

        return BaseType{ .Type = std::format("{}{}", VectorElementName(field.ScalarType), componentCount),
                         .Size = componentCount * scalarSize,
                         .Comment = std::move(comment),
                         .MajorVectors = {} };
    }

    CookResult<FieldCode> MakeFieldCode(const HostField& field,
                                        std::span<const ConcreteStruct> structs,
                                        std::string_view struct_name,
                                        DiagnosticSink& sink)
    {
        CookResult<BaseType> baseResult = DescribeBaseType(field, structs, struct_name, sink);
        if (!baseResult)
        {
            return std::unexpected(baseResult.error());
        }

        const BaseType& base = *baseResult;
        std::vector<HostArrayDimension> dimensions = field.ArrayDimensions;
        if (base.MajorVectors.has_value())
        {
            dimensions.push_back(*base.MajorVectors);
        }

        // from the innermost dimension out: a stride that differs from the size of its element wraps the element
        std::string type = base.Type;
        uint32_t elementSize = base.Size;
        std::vector<uint32_t> counts;
        for (const HostArrayDimension& dimension : dimensions | std::views::reverse)
        {
            if (dimension.Stride < elementSize)
            {
                return std::unexpected(ReportError(sink,
                                                   CookError::HostTypeNotEmittable,
                                                   std::format("member '{}' of host struct '{}' has an array stride of "
                                                               "{} bytes, which is less than its element of {} bytes",
                                                               field.Name,
                                                               struct_name,
                                                               dimension.Stride,
                                                               elementSize)));
            }

            if (dimension.Stride != elementSize)
            {
                type = std::format("detail::Padded<{}{}, {}>", type, MakeArraySuffix(counts), dimension.Stride);
                counts.clear();
            }

            counts.insert(counts.begin(), dimension.Count);
            elementSize = dimension.Count * dimension.Stride;
        }

        const std::string name = IsCppKeyword(field.Name) ? field.Name + "_" : field.Name;

        return FieldCode{ .Type = std::move(type),
                          .Declarator = name + MakeArraySuffix(counts),
                          .Size = elementSize,
                          .Comment = base.Comment };
    }

    void AppendPadding(std::string& text, uint32_t& pad_count, uint32_t byte_count)
    {
        text += std::format("    uint8_t pad{}_[{}];\n", pad_count, byte_count);
        ++pad_count;
    }

    CookError AppendConcreteStruct(std::string& text,
                                   std::span<const ConcreteStruct> structs,
                                   const ConcreteStruct& concrete,
                                   DiagnosticSink& sink)
    {
        const HostStruct& layout = concrete.Layout;
        std::string body;
        std::string asserts;
        uint32_t padCount = 0u;
        uint32_t runningEnd = 0u;

        for (const HostField& field : layout.Fields)
        {
            CookResult<FieldCode> codeResult = MakeFieldCode(field, structs, layout.Name, sink);
            if (!codeResult)
            {
                return codeResult.error();
            }

            const FieldCode& code = *codeResult;
            if (field.Offset < runningEnd)
            {
                return ReportError(sink,
                                   CookError::HostTypeNotEmittable,
                                   std::format("member '{}' of host struct '{}' starts at offset {}, inside the "
                                               "previous member, which ends at {}",
                                               field.Name,
                                               layout.Name,
                                               field.Offset,
                                               runningEnd));
            }

            if (field.Offset > runningEnd)
            {
                AppendPadding(body, padCount, field.Offset - runningEnd);
            }

            body += std::format("    {} {};", code.Type, code.Declarator);
            body += code.Comment.empty() ? "" : std::format(" // {}", code.Comment);
            body += '\n';

            const std::string memberName = IsCppKeyword(field.Name) ? field.Name + "_" : field.Name;
            asserts += std::format("static_assert(offsetof({}, {}) == {}u);\n", concrete.Identifier, memberName, field.Offset);
            runningEnd = field.Offset + code.Size;
        }

        if (runningEnd > layout.ByteSize)
        {
            return ReportError(sink,
                               CookError::HostTypeNotEmittable,
                               std::format("the members of host struct '{}' end at byte {}, past its size of {}",
                                           layout.Name,
                                           runningEnd,
                                           layout.ByteSize));
        }

        if (runningEnd < layout.ByteSize)
        {
            AppendPadding(body, padCount, layout.ByteSize - runningEnd);
        }

        text += std::format("// Slang type: {}\n", layout.Name);
        text += std::format("struct {}\n{{\n{}}};\n\n", concrete.Identifier, body);
        text += std::format("static_assert(sizeof({}) == {}u);\n", concrete.Identifier, layout.ByteSize);
        text += asserts;
        text += '\n';

        return CookError::Success;
    }

    void AppendProfileEnum(std::string& text, const CookedLibrary& library)
    {
        text += "// The value of an enumerator is the index of the profile in the manifest.\n";
        text += "enum class CookProfile : uint32_t\n{\n";
        for (const CookedProfile& profile : library.Profiles)
        {
            text += std::format("    {},\n", MakeProfileEnumerator(profile.TargetName));
        }

        text += "};\n\n";
    }

    CookError AppendHostTypes(std::string& text, const HostTypeCatalog& catalog, DiagnosticSink& sink)
    {
        text += "namespace detail\n{\n\n";
        text += "// Wraps an array element whose stride in the shader is larger than the host type.\n";
        text += "template <class T, std::size_t Stride>\nstruct Padded\n{\n    T Value;\n";
        text += "    uint8_t Pad[Stride - sizeof(T)];\n};\n\n";

        for (const ConcreteStruct& concrete : catalog.Structs)
        {
            const CookError structError = AppendConcreteStruct(text, catalog.Structs, concrete, sink);
            if (!structError)
            {
                return structError;
            }
        }

        for (const auto& [slangName, selection] : catalog.SelectionsByName)
        {
            const std::string identifier = MakeIdentifier(slangName);
            for (const auto& [key, concreteIndex] : selection)
            {
                const auto& [profileIndex, rule] = key;
                text += std::format("template <>\nstruct {}Select<CookProfile::{}, ::lodestone::LayoutRule::{}>\n"
                                    "{{\n    using Type = {};\n}};\n\n",
                                    identifier,
                                    catalog.ProfileEnumerators[profileIndex],
                                    ToString(rule),
                                    catalog.Structs[concreteIndex].Identifier);
            }
        }

        text += "} // namespace detail\n\n";

        return CookError::Success;
    }

    void AppendTypeSelectDeclarations(std::string& text, const HostTypeCatalog& catalog)
    {
        if (catalog.SelectionsByName.empty())
        {
            return;
        }

        text += "// The selection of a struct by profile and layout rule. The definitions are in the .inl file.\n";
        text += "namespace detail\n{\n\n";
        for (const auto& [slangName, selection] : catalog.SelectionsByName)
        {
            text += std::format("template <CookProfile P, ::lodestone::LayoutRule R>\nstruct {}Select;\n\n",
                                MakeIdentifier(slangName));
        }

        text += "} // namespace detail\n\n";
    }

    void AppendTypeAliases(std::string& text, const HostTypeCatalog& catalog)
    {
        for (const auto& [slangName, selection] : catalog.SelectionsByName)
        {
            const std::string identifier = MakeIdentifier(slangName);
            text += std::format("// Slang type: {}\n", slangName);
            text += std::format("template <CookProfile P, ::lodestone::LayoutRule R>\n"
                                "using {} = typename detail::{}Select<P, R>::Type;\n\n",
                                identifier,
                                identifier);
        }
    }

    const CookedModule* FindFirstEnvironment(const CookedLibrary& library, size_t module_index) noexcept
    {
        const size_t moduleCount = library.ModuleNames.size();
        for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
        {
            const std::optional<CookedModule>& environment = library.Environments[(profileIndex * moduleCount) + module_index];
            if (environment.has_value())
            {
                return std::to_address(environment);
            }
        }

        return nullptr;
    }

    const std::optional<CookedModule>& EnvironmentOf(const CookedLibrary& library,
                                                     size_t profile_index,
                                                     size_t module_index) noexcept
    {
        const size_t moduleCount = library.ModuleNames.size();

        return library.Environments[(profile_index * moduleCount) + module_index];
    }

    AxisClasses BuildAxisClasses(const CookedModule& module)
    {
        std::vector<std::vector<std::string>> valuesOfVariant;
        valuesOfVariant.reserve(module.Variants.size());
        for (const LibraryVariant& variant : module.Variants)
        {
            std::vector<std::string>& values = valuesOfVariant.emplace_back();
            for (const PermutationBinding& binding : variant.Canonical)
            {
                values.push_back(ValueToPrintableString(binding.Value));
            }
        }

        // for each axis, the variants that agree on every other axis form one class, found with a key
        const size_t axisCount = module.Space->Axes().size();
        AxisClasses classes(axisCount);
        for (size_t axisIndex = 0u; axisIndex < axisCount; ++axisIndex)
        {
            std::unordered_map<std::string, std::vector<size_t>> membersOfKey;
            for (size_t variantIndex = 0u; variantIndex < valuesOfVariant.size(); ++variantIndex)
            {
                std::string key;
                for (auto&& [otherIndex, value] : std::views::enumerate(valuesOfVariant[variantIndex]))
                {
                    key += static_cast<size_t>(otherIndex) == axisIndex ? std::string{ "*" } : value;
                    key += '|';
                }

                membersOfKey[key].push_back(variantIndex);
            }

            classes[axisIndex].reserve(membersOfKey.size());
            for (auto& [key, members] : membersOfKey)
            {
                classes[axisIndex].push_back(std::move(members));
            }
        }

        return classes;
    }

    std::vector<AxisClasses> BuildEnvironmentClasses(const CookedLibrary& library)
    {
        std::vector<AxisClasses> classes(library.Environments.size());
        for (auto&& [environmentIndex, environment] : std::views::enumerate(library.Environments))
        {
            if (environment.has_value())
            {
                classes[static_cast<size_t>(environmentIndex)] = BuildAxisClasses(*environment);
            }
        }

        return classes;
    }

    std::set<std::string> AxesWhereClassesDiffer(const CookedModule& module,
                                                 const AxisClasses& classes,
                                                 std::span<const std::string> fingerprints)
    {
        std::set<std::string> names;
        std::span<const PermutationAxis> axes = module.Space->Axes();
        for (auto&& [axisIndex, axisClasses] : std::views::enumerate(classes))
        {
            // a variant with an empty fingerprint has no value to compare
            const auto differsInClass = [&fingerprints](const std::vector<size_t>& members)
            {
                const auto first = std::ranges::find_if(members, [&fingerprints](size_t member) { return !fingerprints[member].empty(); });
                if (first == members.end())
                {
                    return false;
                }

                const std::string& reference = fingerprints[*first];

                return std::ranges::any_of(members,
                                           [&fingerprints, &reference](size_t member)
                                           {
                                               return !fingerprints[member].empty() && fingerprints[member] != reference;
                                           });
            };

            if (std::ranges::any_of(axisClasses, differsInClass))
            {
                names.insert(axes[static_cast<size_t>(axisIndex)].Name);
            }
        }

        return names;
    }

    std::vector<UsedBinding> UsedBindings(const CookedModule& module,
                                          const LibraryVariant& variant,
                                          size_t entry_point_index)
    {
        const CodeFormTables& tables = FormTables(module, CodeForm::Delivered);
        const CodeFormIndices& indices = FormIndices(variant, CodeForm::Delivered);
        const VisibilityList& visible = tables.VisibilityLists[indices.VisibilityIndices[entry_point_index]];
        const ResourceList& resources = module.ResourceLists[variant.ResourceListIndex];
        const EntryPointConstants& constants = tables.EntryPointConstantsTable[indices.ConstantsIndices[entry_point_index]];
        const bool hasConstantsBuffer = constants.Delivery == EntryPointConstantsDelivery::UniformBuffer;

        // the uniform buffer of the entry point constants is the `Params` type, not a binding of the shader
        std::vector<UsedBinding> used;
        used.reserve(visible.size());
        for (auto&& [visibleIndex, position] : std::views::enumerate(visible))
        {
            if (hasConstantsBuffer && static_cast<uint32_t>(visibleIndex) == constants.Binding)
            {
                continue;
            }

            const ReflectedBinding& binding = module.Resources[resources[position]];
            // a Handle or Pointer shim is a value in a block, and not a binding of the pipeline layout
            if (GetStoredValuePlacement(binding.Placement) == nullptr)
            {
                used.push_back(UsedBinding{ .Name = binding.Name, .Position = position, .Resource = &binding });
            }
        }

        std::ranges::stable_sort(used, {}, &UsedBinding::Name);
        const auto [duplicatesBegin, duplicatesEnd] = std::ranges::unique(used, {}, &UsedBinding::Name);
        used.erase(duplicatesBegin, duplicatesEnd);

        return used;
    }

    void AddEnvironmentPresence(const CookedModule& module,
                                const AxisClasses& classes,
                                size_t entry_point_index,
                                size_t profile_index,
                                BindingPresence& presence)
    {
        std::vector<std::vector<std::string_view>> namesOfVariant;
        namesOfVariant.reserve(module.Variants.size());
        for (const LibraryVariant& variant : module.Variants)
        {
            std::vector<std::string_view>& names = namesOfVariant.emplace_back();
            for (const UsedBinding& used : UsedBindings(module, variant, entry_point_index))
            {
                names.push_back(used.Name);
                std::vector<size_t>& counts = presence.VariantsWithBinding[std::string{ used.Name }];
                counts.resize(presence.VariantCounts.size(), 0u);
                counts[profile_index] += 1u;
            }
        }

        presence.VariantCounts[profile_index] = module.Variants.size();

        std::span<const PermutationAxis> axes = module.Space->Axes();
        for (auto&& [axisIndex, axisClasses] : std::views::enumerate(classes))
        {
            for (const std::vector<size_t>& members : axisClasses)
            {
                std::map<std::string_view, size_t> counts;
                for (const size_t member : members)
                {
                    for (const std::string_view name : namesOfVariant[member])
                    {
                        counts[name] += 1u;
                    }
                }

                for (const auto& [name, count] : counts)
                {
                    if (count != members.size())
                    {
                        presence.AxesChangingBinding[std::string{ name }].insert(axes[static_cast<size_t>(axisIndex)].Name);
                    }
                }
            }
        }
    }

    void AddEnvironmentBindingFacts(const CookedModule& module,
                                    const AxisClasses& classes,
                                    const HostTypeCatalog& catalog,
                                    std::span<const uint32_t> remap,
                                    size_t entry_point_index,
                                    size_t profile_index,
                                    std::map<std::string, BindingTypeFacts>& bindings)
    {
        // for each binding, the concrete struct of each variant as text, empty where the variant has none
        std::map<std::string, std::vector<std::string>> fingerprintsOfName;
        for (auto&& [variantIndex, variant] : std::views::enumerate(module.Variants))
        {
            std::map<uint32_t, uint32_t> structOfPosition;
            for (const HostRoot& root : variant.HostRoots)
            {
                if (root.Kind == HostRootKind::Binding)
                {
                    structOfPosition.emplace(root.Owner, remap[root.Struct]);
                }
            }

            for (const UsedBinding& used : UsedBindings(module, variant, entry_point_index))
            {
                const std::string name{ used.Name };
                BindingTypeFacts& facts = bindings[name];
                facts.Kinds.emplace(DescribeKind(used.Resource->Kind));

                const std::string_view access = DescribeAccess(*used.Resource);
                if (!access.empty())
                {
                    facts.Accesses.emplace(access);
                }

                std::vector<std::string>& fingerprints = fingerprintsOfName[name];
                fingerprints.resize(module.Variants.size());

                const auto root = structOfPosition.find(used.Position);
                if (root == structOfPosition.end())
                {
                    continue;
                }

                facts.StructsByProfile[profile_index].insert(root->second);
                facts.StructNames.insert(catalog.Structs[root->second].SlangName);
                fingerprints[static_cast<size_t>(variantIndex)] = std::to_string(root->second);
            }
        }

        for (const auto& [name, fingerprints] : fingerprintsOfName)
        {
            BindingTypeFacts& facts = bindings.at(name);
            const auto structs = facts.StructsByProfile.find(profile_index);
            if (structs != facts.StructsByProfile.end() && structs->second.size() > 1u)
            {
                facts.StructAxes.merge(AxesWhereClassesDiffer(module, classes, fingerprints));
            }
        }
    }

    void AddEnvironmentNumThreads(const CookedModule& module,
                                  const AxisClasses& classes,
                                  size_t entry_point_index,
                                  NumThreadsFacts& facts)
    {
        const LibraryEntryPoint& entryPoint = module.EntryPoints[entry_point_index];
        if (entryPoint.Stage != ShaderStageKind::Compute)
        {
            return;
        }

        facts.IsCompute = true;
        facts.Expressions = entryPoint.WorkgroupExpressions;

        std::vector<std::string> fingerprints;
        fingerprints.reserve(module.Variants.size());
        for (const LibraryVariant& variant : module.Variants)
        {
            const WorkgroupSize& workgroup = variant.Workgroups[entry_point_index];
            facts.Values.insert(std::array<uint32_t, 3>{ workgroup.X, workgroup.Y, workgroup.Z });
            fingerprints.push_back(std::format("{},{},{}", workgroup.X, workgroup.Y, workgroup.Z));
        }

        facts.Axes.merge(AxesWhereClassesDiffer(module, classes, fingerprints));
    }

    /** True when, in some profile that uses the binding, some variant of the profile does not. */
    bool LacksInSomeVariant(const BindingPresence& presence, const std::vector<size_t>& counts)
    {
        for (size_t profileIndex = 0u; profileIndex < counts.size(); ++profileIndex)
        {
            if (counts[profileIndex] != 0u && counts[profileIndex] != presence.VariantCounts[profileIndex])
            {
                return true;
            }
        }

        return false;
    }

    /** Returns the cooked profiles that do not use the binding, as a comma separated list, or an empty string. */
    std::string ListMissingProfiles(const CookedLibrary& library,
                                    const BindingPresence& presence,
                                    const std::vector<size_t>& counts)
    {
        std::string names;
        for (size_t profileIndex = 0u; profileIndex < counts.size(); ++profileIndex)
        {
            if (presence.VariantCounts[profileIndex] != 0u && counts[profileIndex] == 0u)
            {
                names += names.empty() ? "" : ", ";
                names += library.Profiles[profileIndex].TargetName;
            }
        }

        return names;
    }

    std::map<size_t, uint32_t> CollectParamsSelection(const EmitContext& context,
                                                      size_t module_index,
                                                      size_t entry_point_index)
    {
        std::map<size_t, uint32_t> params;
        const CookedLibrary& library = context.Library;
        const size_t moduleCount = library.ModuleNames.size();
        for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
        {
            const size_t environmentIndex = (profileIndex * moduleCount) + module_index;
            const std::optional<CookedModule>& environment = library.Environments[environmentIndex];
            if (!environment.has_value())
            {
                continue;
            }

            const std::vector<uint32_t>& remap = context.Catalog.EnvironmentRemaps[environmentIndex];
            for (const LibraryVariant& variant : environment->Variants)
            {
                const auto root = std::ranges::find_if(variant.HostRoots,
                                                       [entry_point_index](const HostRoot& candidate)
                                                       {
                                                           return candidate.Kind == HostRootKind::EntryPointConstants &&
                                                                  candidate.Owner == entry_point_index;
                                                       });
                if (root != variant.HostRoots.end())
                {
                    params.emplace(profileIndex, remap[root->Struct]);
                    break;
                }
            }
        }

        return params;
    }

    EntryPointFacts CollectEntryPointFacts(const EmitContext& context, size_t module_index, size_t entry_point_index)
    {
        const CookedLibrary& library = context.Library;
        const CookedModule& first = *FindFirstEnvironment(library, module_index);
        const size_t moduleCount = library.ModuleNames.size();

        EntryPointFacts facts;
        facts.Name = first.EntryPoints[entry_point_index].Name;
        facts.Stage = first.EntryPoints[entry_point_index].Stage;
        facts.Presence.VariantCounts.assign(library.Profiles.size(), 0u);

        for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
        {
            const std::optional<CookedModule>& environment = EnvironmentOf(library, profileIndex, module_index);
            if (!environment.has_value())
            {
                continue;
            }

            const size_t environmentIndex = (profileIndex * moduleCount) + module_index;
            const AxisClasses& classes = context.Classes[environmentIndex];
            const std::vector<uint32_t>& remap = context.Catalog.EnvironmentRemaps[environmentIndex];

            AddEnvironmentPresence(*environment, classes, entry_point_index, profileIndex, facts.Presence);
            AddEnvironmentBindingFacts(
                *environment, classes, context.Catalog, remap, entry_point_index, profileIndex, facts.Bindings);
            AddEnvironmentNumThreads(*environment, classes, entry_point_index, facts.NumThreads);
        }

        facts.Params = CollectParamsSelection(context, module_index, entry_point_index);
        for (const auto& [name, counts] : facts.Presence.VariantsWithBinding)
        {
            (LacksInSomeVariant(facts.Presence, counts) ? facts.SomeVariants : facts.EveryVariant).insert(name);
        }

        return facts;
    }

    bool StructChangesWithAxes(const BindingTypeFacts& facts) noexcept
    {
        return std::ranges::any_of(facts.StructsByProfile, [](const auto& entry) { return entry.second.size() > 1u; });
    }

    bool HasSingleStruct(const BindingTypeFacts& facts) noexcept
    {
        return !facts.StructsByProfile.empty() && !StructChangesWithAxes(facts);
    }

    CookError CheckEntryPointNames(const EntryPointFacts& entry_point, DiagnosticSink& sink)
    {
        std::map<std::string, std::string> owners;
        const auto claim = [&owners, &entry_point, &sink](const std::string& identifier, const std::string& source) -> CookError
        {
            const auto [position, inserted] = owners.try_emplace(identifier, source);
            if (inserted || position->second == source)
            {
                return CookError::Success;
            }

            return ReportError(sink,
                               CookError::HostBindingNameCollision,
                               std::format("in entry point '{}', {} and {} both map to the C++ identifier '{}'",
                                           entry_point.Name,
                                           position->second,
                                           source,
                                           identifier));
        };

        for (const std::string_view reserved : { "Params", "SomeVariants", "Bindings", "SomeVariantsBindings", "detail" })
        {
            static_cast<void>(claim(std::string{ reserved }, std::format("the generated name '{}'", reserved)));
        }

        for (const auto& [name, facts] : entry_point.Bindings)
        {
            const std::string identifier = MakeIdentifier(name);
            const CookError constantError = claim(identifier, std::format("the binding '{}'", name));
            if (!constantError)
            {
                return constantError;
            }

            if (HasSingleStruct(facts))
            {
                const CookError aliasError = claim(identifier + "Type", std::format("the type alias of the binding '{}'", name));
                if (!aliasError)
                {
                    return aliasError;
                }
            }
        }

        return CookError::Success;
    }

    CookError CheckModuleBindingNames(const ModuleFacts& module, DiagnosticSink& sink)
    {
        std::map<std::string, std::string> owners;
        const auto claim = [&owners, &module, &sink](const std::string& identifier, const std::string& source) -> CookError
        {
            const auto [position, inserted] = owners.try_emplace(identifier, source);
            if (inserted || position->second == source)
            {
                return CookError::Success;
            }

            return ReportError(sink,
                               CookError::HostBindingNameCollision,
                               std::format("in module '{}', {} and {} both map to the C++ identifier '{}'",
                                           module.Name,
                                           position->second,
                                           source,
                                           identifier));
        };

        for (const EntryPointFacts& entryPoint : module.EntryPoints)
        {
            const CookError entryPointError =
                claim(MakeIdentifier(entryPoint.Name), std::format("the entry point '{}'", entryPoint.Name));
            if (!entryPointError)
            {
                return entryPointError;
            }
        }

        for (const std::string& name : module.BindingNames)
        {
            const CookError bindingError = claim(MakeIdentifier(name), std::format("the binding '{}'", name));
            if (!bindingError)
            {
                return bindingError;
            }
        }

        return CookError::Success;
    }

    CookResult<std::vector<ModuleFacts>> CollectModuleFacts(const EmitContext& context)
    {
        const CookedLibrary& library = context.Library;
        std::map<std::string, std::string> nameOfIdentifier{ { "CookProfile", "CookProfile" }, { "detail", "detail" } };
        for (const auto& [identifier, slangName] : context.Catalog.NameOfIdentifier)
        {
            nameOfIdentifier.emplace(identifier, slangName);
        }

        std::vector<ModuleFacts> modules;
        for (size_t moduleIndex = 0u; moduleIndex < library.ModuleNames.size(); ++moduleIndex)
        {
            const CookedModule* first = FindFirstEnvironment(library, moduleIndex);
            if (first == nullptr)
            {
                continue;
            }

            const std::string& moduleName = library.ModuleNames[moduleIndex];
            ModuleFacts module{ .Name = moduleName, .Identifier = MakeIdentifier(moduleName), .EntryPoints = {}, .BindingNames = {} };
            const auto [position, inserted] = nameOfIdentifier.try_emplace(module.Identifier, moduleName);
            if (!inserted)
            {
                return std::unexpected(ReportError(context.Sink,
                                                   CookError::HostTypeNameCollision,
                                                   std::format("the module name '{}' and the name '{}' both map to the C++ "
                                                               "identifier '{}' in the host header",
                                                               moduleName,
                                                               position->second,
                                                               module.Identifier)));
            }

            std::map<std::string, std::string> entryPointOfIdentifier;
            for (size_t entryPointIndex = 0u; entryPointIndex < first->EntryPoints.size(); ++entryPointIndex)
            {
                const std::string& entryPointName = first->EntryPoints[entryPointIndex].Name;
                const auto [entryPosition, entryInserted] =
                    entryPointOfIdentifier.try_emplace(MakeIdentifier(entryPointName), entryPointName);
                if (!entryInserted)
                {
                    return std::unexpected(ReportError(context.Sink,
                                                       CookError::HostTypeNameCollision,
                                                       std::format("in module '{}', the entry point names '{}' and '{}' both "
                                                                   "map to the C++ identifier '{}'",
                                                                   moduleName,
                                                                   entryPointName,
                                                                   entryPosition->second,
                                                                   entryPosition->first)));
                }

                EntryPointFacts facts = CollectEntryPointFacts(context, moduleIndex, entryPointIndex);
                const CookError nameError = CheckEntryPointNames(facts, context.Sink);
                if (!nameError)
                {
                    return std::unexpected(nameError);
                }

                for (const auto& [name, bindingFacts] : facts.Bindings)
                {
                    module.BindingNames.insert(name);
                }

                module.EntryPoints.push_back(std::move(facts));
            }

            const CookError moduleError = CheckModuleBindingNames(module, context.Sink);
            if (!moduleError)
            {
                return std::unexpected(moduleError);
            }

            modules.push_back(std::move(module));
        }

        return modules;
    }

    std::string_view DescribeKind(BindingKind kind) noexcept
    {
        switch (kind)
        {
        case BindingKind::Sampler:
            return "sampler";
        case BindingKind::Texture:
            return "sampled image";
        case BindingKind::UniformBuffer:
            return "uniform buffer";
        case BindingKind::ParameterBlock:
            return "parameter block";
        case BindingKind::StorageBuffer:
            return "storage buffer";
        case BindingKind::TexelBuffer:
            return "texel buffer";
        case BindingKind::CombinedTextureSampler:
            return "combined texture sampler";
        case BindingKind::InputRenderTarget:
            return "input render target";
        case BindingKind::InlineUniform:
            return "inline uniform";
        case BindingKind::RayTracingAccelerationStructure:
            return "acceleration structure";
        case BindingKind::StorageTexture:
            return "storage image";
        case BindingKind::PushConstant:
            return "push constant";
        case BindingKind::Invalid:
            break;
        }

        return "unknown";
    }

    std::string_view DescribeAccess(const ReflectedBinding& binding) noexcept
    {
        // reflection states no access for a uniform buffer, and a shader cannot write one
        if (binding.Kind == BindingKind::UniformBuffer || binding.Kind == BindingKind::InlineUniform)
        {
            return "read";
        }

        switch (binding.Access)
        {
        case ResourceAccess::ReadOnly:
            return "read";
        case ResourceAccess::WriteOnly:
            return "write";
        case ResourceAccess::ReadWrite:
            return "read-write";
        case ResourceAccess::RasterizerOrdered:
            return "rasterizer-ordered";
        case ResourceAccess::Append:
            return "append";
        case ResourceAccess::Consume:
            return "consume";
        case ResourceAccess::Feedback:
            return "feedback";
        case ResourceAccess::Invalid:
            break;
        }

        return "";
    }

    bool IsLiteralInteger(std::string_view text) noexcept
    {
        if (text.starts_with("0x") || text.starts_with("0X"))
        {
            const std::string_view digits = text.substr(2u);

            return !digits.empty() && std::ranges::all_of(digits, [](char character) { return std::isxdigit(static_cast<unsigned char>(character)) != 0; });
        }

        if (text.ends_with('u') || text.ends_with('U'))
        {
            text.remove_suffix(1u);
        }

        return !text.empty() && std::ranges::all_of(text, [](char character) { return character >= '0' && character <= '9'; });
    }

    std::string DescribeNumThreads(const NumThreadsFacts& facts)
    {
        // Slang reports 0 for a size that is an expression of link-time constants, and no workgroup has that size
        const auto componentText = [](uint32_t component) { return component == 0u ? std::string{ "?" } : std::to_string(component); };
        bool hasUnknownComponent = false;

        const bool varies = facts.Values.size() > 1u;
        std::string text = varies ? "varies: " : "";
        bool first = true;
        for (const std::array<uint32_t, 3>& value : facts.Values)
        {
            hasUnknownComponent = hasUnknownComponent || std::ranges::contains(value, 0u);
            text += first ? "" : " | ";
            text += std::format("({}, {}, {})", componentText(value[0]), componentText(value[1]), componentText(value[2]));
            first = false;
        }

        if (varies)
        {
            text += facts.Axes.empty() ? std::string{ " between profiles" } : std::format(" by {}", JoinSet(facts.Axes, ", "));
        }

        // a literal states itself, so the source text is shown only when an argument is an expression
        const auto isExpression = [](const std::string& argument) { return !argument.empty() && !IsLiteralInteger(argument); };
        if (std::ranges::any_of(facts.Expressions, isExpression))
        {
            text += std::format(" [{}, {}, {}]", facts.Expressions[0], facts.Expressions[1], facts.Expressions[2]);
        }

        if (hasUnknownComponent)
        {
            text += " (the manifest has 0 for ?: Slang does not fold this expression)";
        }

        return text;
    }

    void AppendWrappedLine(std::string& text,
                           std::string_view first_prefix,
                           std::string_view next_prefix,
                           std::span<const std::string> pieces)
    {
        constexpr size_t k_LineWidth = 110u;
        std::string line{ first_prefix };
        bool lineHasPiece = false;
        for (const std::string& piece : pieces)
        {
            if (lineHasPiece && line.size() + 1u + piece.size() > k_LineWidth)
            {
                text += line;
                text += '\n';
                line = next_prefix;
                lineHasPiece = false;
            }

            line += lineHasPiece ? " " : "";
            line += piece;
            lineHasPiece = true;
        }

        text += line;
        text += '\n';
    }

    std::string DescribeFieldForComment(const HostField& field, std::span<const ConcreteStruct> structs)
    {
        std::string type;
        if (field.Kind == HostFieldKind::Struct)
        {
            type = structs[field.StructIndex].SlangName;
        }
        else if (field.Kind == HostFieldKind::Matrix)
        {
            type = std::format("{}{}x{}", VectorElementName(field.ScalarType), field.Rows, field.Columns);
        }
        else if (field.Kind == HostFieldKind::Vector)
        {
            type = std::format("{}{}", VectorElementName(field.ScalarType), field.Columns);
        }
        else
        {
            type = field.ScalarType == HostScalarType::Bool ? std::string{ "bool" } : std::string{ VectorElementName(field.ScalarType) };
        }

        std::string declarator = field.Name;
        for (const HostArrayDimension& dimension : field.ArrayDimensions)
        {
            declarator += std::format("[{}]", dimension.Count);
        }

        return std::format("{} {};", type, declarator);
    }

    void AppendParamsComment(std::string& text, const EmitContext& context, const EntryPointFacts& entry_point)
    {
        if (entry_point.Params.empty())
        {
            text += "//   no Params\n";

            return;
        }

        // the names and types of the members do not depend on the profile, only the padding does
        const ConcreteStruct& params = context.Catalog.Structs[entry_point.Params.begin()->second];
        std::vector<std::string> pieces{ "Params:", params.SlangName, "{" };
        for (const HostField& field : params.Layout.Fields)
        {
            pieces.push_back(DescribeFieldForComment(field, context.Catalog.Structs));
        }

        pieces.emplace_back("}");
        AppendWrappedLine(text, "//   ", "//       ", pieces);
    }

    void AppendBindingRows(std::string& text, const EmitContext& context, const EntryPointFacts& entry_point)
    {
        if (entry_point.Bindings.empty())
        {
            text += "//   no bindings\n";

            return;
        }

        struct Row
        {
            std::string Name;
            std::string Kind;
            std::string Access;
            std::string Structs;
            std::string Marks;
        };

        std::vector<Row> rows;
        for (const auto& [name, facts] : entry_point.Bindings)
        {
            Row row{ .Name = name,
                     .Kind = JoinSet(facts.Kinds, "/"),
                     .Access = JoinSet(facts.Accesses, "/"),
                     .Structs = JoinSet(facts.StructNames, ", "),
                     .Marks = {} };

            const auto addMark = [&row](const std::string& mark)
            {
                row.Marks += row.Marks.empty() ? "" : " ";
                row.Marks += "[" + mark + "]";
            };

            if (entry_point.SomeVariants.contains(name))
            {
                addMark("some variants");
            }

            const std::string missing =
                ListMissingProfiles(context.Library, entry_point.Presence, entry_point.Presence.VariantsWithBinding.at(name));
            if (!missing.empty())
            {
                addMark(std::format("not a binding on profiles: {}", missing));
            }

            if (StructChangesWithAxes(facts))
            {
                addMark(std::format("struct changes with the axes: {}", JoinSet(facts.StructAxes, ", ")));
            }

            rows.push_back(std::move(row));
        }

        const auto widest = [&rows](std::string Row::*member)
        {
            size_t width = 0u;
            for (const Row& row : rows)
            {
                width = std::max(width, (row.*member).size());
            }

            return width;
        };

        const size_t nameWidth = widest(&Row::Name);
        const size_t kindWidth = widest(&Row::Kind);
        const size_t accessWidth = widest(&Row::Access);
        const size_t structWidth = widest(&Row::Structs);

        text += "//   bindings:\n";
        for (const Row& row : rows)
        {
            std::string line = std::format("//     {:<{}}  {:<{}}  {:<{}}  {:<{}}  {}",
                                           row.Name,
                                           nameWidth,
                                           row.Kind,
                                           kindWidth,
                                           row.Access,
                                           accessWidth,
                                           row.Structs,
                                           structWidth,
                                           row.Marks);
            while (line.ends_with(' '))
            {
                line.pop_back();
            }

            text += line;
            text += '\n';
        }
    }

    void AppendEntryPointComment(std::string& text, const EmitContext& context, const EntryPointFacts& entry_point)
    {
        const std::string rule = "// " + std::string(105u, '-') + "\n";
        std::string stage{ ToString(entry_point.Stage) };
        std::ranges::transform(stage, stage.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

        text += rule;
        text += std::format("// {} ({})\n", entry_point.Name, stage);
        if (entry_point.NumThreads.IsCompute)
        {
            text += std::format("//   numthreads: {}\n", DescribeNumThreads(entry_point.NumThreads));
        }

        AppendBindingRows(text, context, entry_point);
        AppendParamsComment(text, context, entry_point);

        if (entry_point.SomeVariants.empty())
        {
            text += "//   some variants only: (none)\n";
        }
        else
        {
            std::vector<std::string> pieces{ "some", "variants", "only:" };
            for (const std::string& name : entry_point.SomeVariants)
            {
                pieces.push_back(name + ",");
            }

            pieces.back().pop_back();
            AppendWrappedLine(text, "//   ", "//       ", pieces);
        }

        text += rule;
    }

    void AppendEntryPointSelectDeclarations(std::string& text, std::span<const ModuleFacts> modules)
    {
        bool wroteComment = false;
        for (const ModuleFacts& module : modules)
        {
            for (const EntryPointFacts& entryPoint : module.EntryPoints)
            {
                const bool hasTypedBinding = std::ranges::any_of(
                    entryPoint.Bindings, [](const auto& entry) { return HasSingleStruct(entry.second); });
                if (entryPoint.Params.empty() && !hasTypedBinding)
                {
                    continue;
                }

                if (!wroteComment)
                {
                    text += "// The selection of the types of an entry point by profile. The definitions are in the .inl file.\n";
                    wroteComment = true;
                }

                text += std::format("namespace {}::{}::detail\n{{\n\n", module.Identifier, MakeIdentifier(entryPoint.Name));
                if (!entryPoint.Params.empty())
                {
                    text += "template <CookProfile P>\nstruct ParamsSelect;\n\n";
                }

                for (const auto& [name, facts] : entryPoint.Bindings)
                {
                    if (HasSingleStruct(facts))
                    {
                        text += std::format("template <CookProfile P>\nstruct {}TypeSelect;\n\n", MakeIdentifier(name));
                    }
                }

                text += "} // namespace detail\n\n";
            }
        }
    }

    void AppendBindingConstant(std::string& text, std::string_view name)
    {
        text += std::format("inline constexpr std::string_view {}{{ \"{}\" }};\n", MakeIdentifier(name), name);
    }

    void AppendBindingUsing(std::string& text, std::string_view qualifier, std::string_view name)
    {
        text += std::format("using {}::{};\n", qualifier, MakeIdentifier(name));
    }

    void AppendBindingTypeAlias(std::string& text, std::string_view name)
    {
        const std::string identifier = MakeIdentifier(name);
        text += std::format("template <CookProfile P>\nusing {}Type = typename detail::{}TypeSelect<P>::Type;\n",
                            identifier,
                            identifier);
    }

    void AppendBindingArray(std::string& text,
                            std::string_view array_name,
                            std::string_view scope,
                            const std::set<std::string>& names)
    {
        if (names.empty())
        {
            text += std::format("inline constexpr std::array<std::string_view, 0> {}{{}};\n", array_name);

            return;
        }

        text += std::format("inline constexpr std::array<std::string_view, {}> {}{{ {{\n", names.size(), array_name);
        for (const std::string& name : names)
        {
            text += std::format("    {}{},\n", scope, MakeIdentifier(name));
        }

        text += "} };\n";
    }

    void AppendEntryPoint(std::string& text,
                          const EmitContext& context,
                          std::string_view module_identifier,
                          const EntryPointFacts& entry_point)
    {
        const CookedLibrary& library = context.Library;
        const std::string entryPointIdentifier = MakeIdentifier(entry_point.Name);
        const std::string qualifier = entryPointIdentifier == module_identifier
                                          ? std::string{ context.QualifiedPrefix }.append(module_identifier)
                                          : std::string{ module_identifier };

        AppendEntryPointComment(text, context, entry_point);
        text += std::format("namespace {}\n{{\n\n", entryPointIdentifier);

        const auto appendUsings = [&](const std::set<std::string>& names)
        {
            for (const std::string& name : names)
            {
                const std::vector<size_t>& counts = entry_point.Presence.VariantsWithBinding.at(name);
                const std::string missing = ListMissingProfiles(library, entry_point.Presence, counts);
                text += missing.empty() ? "" : std::format("// not a binding on profiles: {}\n", missing);
                AppendBindingUsing(text, qualifier, name);
            }
        };

        const auto appendTypeAliases = [&](const std::set<std::string>& names)
        {
            std::string aliases;
            for (const std::string& name : names)
            {
                if (HasSingleStruct(entry_point.Bindings.at(name)))
                {
                    aliases += aliases.empty() ? "" : "\n";
                    AppendBindingTypeAlias(aliases, name);
                }
            }

            text += aliases.empty() ? "" : "\n" + aliases;
        };

        appendUsings(entry_point.EveryVariant);
        appendTypeAliases(entry_point.EveryVariant);

        if (!entry_point.SomeVariants.empty())
        {
            text += "\nnamespace SomeVariants\n{\n\n";
            for (const std::string& name : entry_point.SomeVariants)
            {
                const auto changing = entry_point.Presence.AxesChangingBinding.find(name);
                if (changing == entry_point.Presence.AxesChangingBinding.end())
                {
                    text += "// present or absent with a combination of axes\n";
                }
                else
                {
                    text += std::format("// present or absent with the axes: {}\n", JoinSet(changing->second, ", "));
                }

                appendUsings({ name });
            }

            appendTypeAliases(entry_point.SomeVariants);
            text += "\n} // namespace SomeVariants\n";
        }

        text += "\n";
        AppendBindingArray(text, "Bindings", "", entry_point.EveryVariant);
        AppendBindingArray(text, "SomeVariantsBindings", "SomeVariants::", entry_point.SomeVariants);

        if (!entry_point.Params.empty())
        {
            text += "\n// The constants of the entry point: a push constant block or a uniform buffer, by profile.\n";
            text += "template <CookProfile P>\nusing Params = typename detail::ParamsSelect<P>::Type;\n";
        }

        text += std::format("\n}} // namespace {}\n\n", entryPointIdentifier);
    }

    void AppendModules(std::string& text, const EmitContext& context, std::span<const ModuleFacts> modules)
    {
        for (const ModuleFacts& module : modules)
        {
            text += std::format("// Module {}\n", module.Name);
            text += std::format("namespace {}\n{{\n\n", module.Identifier);
            for (const std::string& name : module.BindingNames)
            {
                AppendBindingConstant(text, name);
            }

            text += module.BindingNames.empty() ? "" : "\n";

            for (const EntryPointFacts& entryPoint : module.EntryPoints)
            {
                AppendEntryPoint(text, context, module.Identifier, entryPoint);
            }

            text += std::format("}} // namespace {}\n\n", module.Identifier);
        }
    }

    void AppendEntryPointSelections(std::string& text, const EmitContext& context, std::span<const ModuleFacts> modules)
    {
        const HostTypeCatalog& catalog = context.Catalog;
        for (const ModuleFacts& module : modules)
        {
            for (const EntryPointFacts& entryPoint : module.EntryPoints)
            {
                std::string body;
                for (const auto& [profileIndex, concreteIndex] : entryPoint.Params)
                {
                    body += std::format("template <>\nstruct ParamsSelect<CookProfile::{}>\n{{\n"
                                        "    using Type = {}detail::{};\n}};\n\n",
                                        catalog.ProfileEnumerators[profileIndex],
                                        context.QualifiedPrefix,
                                        catalog.Structs[concreteIndex].Identifier);
                }

                for (const auto& [name, facts] : entryPoint.Bindings)
                {
                    if (!HasSingleStruct(facts))
                    {
                        continue;
                    }

                    for (const auto& [profileIndex, concreteIndices] : facts.StructsByProfile)
                    {
                        body += std::format("template <>\nstruct {}TypeSelect<CookProfile::{}>\n{{\n"
                                            "    using Type = {}detail::{};\n}};\n\n",
                                            MakeIdentifier(name),
                                            catalog.ProfileEnumerators[profileIndex],
                                            context.QualifiedPrefix,
                                            catalog.Structs[*concreteIndices.begin()].Identifier);
                    }
                }

                if (!body.empty())
                {
                    text += std::format("namespace {}::{}::detail\n{{\n\n{}}} // namespace detail\n\n",
                                        module.Identifier,
                                        MakeIdentifier(entryPoint.Name),
                                        body);
                }
            }
        }
    }

} // namespace

} // namespace lodestone
