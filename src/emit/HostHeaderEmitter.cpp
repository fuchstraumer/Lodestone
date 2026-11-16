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

    struct BindingPresence
    {
        /** For each profile, the variant count of the module, or 0 when the module was not cooked for it. */
        std::vector<size_t> VariantCounts;
        /** For each binding name, the count of variants that use it, for each profile. */
        std::map<std::string, std::vector<size_t>> VariantsWithBinding;
        std::map<std::string, std::set<std::string>> AxesChangingBinding;
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

    uint32_t FindOrAddConcreteStruct(HostTypeCatalog& catalog,
                                     const HostStruct& host_struct,
                                     std::span<const uint32_t> remap);
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
    CookError AppendHostTypes(std::string& text, const HostTypeCatalog& catalog, DiagnosticSink& sink);
    void AppendTypeAliases(std::string& text, const HostTypeCatalog& catalog);

    const CookedModule* FindFirstEnvironment(const CookedLibrary& library, size_t module_index) noexcept;
    std::vector<std::string_view> UsedBindingNames(const CookedModule& module,
                                                   const LibraryVariant& variant,
                                                   size_t entry_point_index);
    void AddEnvironmentPresence(const CookedModule& module,
                                size_t entry_point_index,
                                size_t profile_index,
                                BindingPresence& presence);
    bool LacksInSomeVariant(const BindingPresence& presence, const std::vector<size_t>& counts);
    std::string DescribeMissingProfiles(const CookedLibrary& library,
                                        const BindingPresence& presence,
                                        const std::vector<size_t>& counts);
    BindingPresence CollectBindingPresence(const CookedLibrary& library,
                                           size_t module_index,
                                           size_t entry_point_index);
    std::map<size_t, uint32_t> CollectParamsSelection(const CookedLibrary& library,
                                                      const HostTypeCatalog& catalog,
                                                      size_t module_index,
                                                      size_t entry_point_index);
    void AppendBindingConstant(std::string& text, std::string_view name, std::string_view indent);
    void AppendParams(std::string& text,
                      const HostTypeCatalog& catalog,
                      const std::map<size_t, uint32_t>& params,
                      std::string_view qualified_prefix);
    CookError AppendEntryPoint(std::string& text,
                               const CookedLibrary& library,
                               const HostTypeCatalog& catalog,
                               std::string_view qualified_prefix,
                               size_t module_index,
                               size_t entry_point_index,
                               DiagnosticSink& sink);
    CookError AppendModules(std::string& text,
                            const CookedLibrary& library,
                            const HostTypeCatalog& catalog,
                            std::string_view qualified_prefix,
                            DiagnosticSink& sink);

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

CookResult<std::string> EmitHostHeader(const CookedLibrary& library,
                                       std::string_view host_types_include,
                                       std::string_view host_namespace,
                                       DiagnosticSink& sink)
{
    CookResult<HostTypeCatalog> catalogResult = BuildHostTypeCatalog(library, sink);
    if (!catalogResult)
    {
        return std::unexpected(catalogResult.error());
    }

    const HostTypeCatalog& catalog = *catalogResult;

    std::string profileList;
    for (const CookedProfile& profile : library.Profiles)
    {
        profileList += profileList.empty() ? "" : ", ";
        profileList += profile.TargetName;
    }

    std::string text;
    text += "// Generated by the lodestone cooker. Do not edit.\n";
    text += std::format("// Profiles: {}.\n", profileList);
    text += "#pragma once\n\n";
    text += "#include <cstddef>\n#include <cstdint>\n#include <string_view>\n\n";
    text += "#include \"ShaderLibraryTypes.hpp\"\n";
    text += std::format("#include \"{}\"\n\n", host_types_include);
    text += std::format("namespace {}\n{{\n\n", host_namespace);

    AppendProfileEnum(text, library);

    const CookError typesError = AppendHostTypes(text, catalog, sink);
    if (!typesError)
    {
        return std::unexpected(typesError);
    }

    AppendTypeAliases(text, catalog);

    const std::string qualifiedPrefix = MakeQualifiedPrefix(host_namespace);
    const CookError modulesError = AppendModules(text, library, catalog, qualifiedPrefix, sink);
    if (!modulesError)
    {
        return std::unexpected(modulesError);
    }

    text += std::format("}} // namespace {}\n", host_namespace);

    return text;
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

        uint32_t sameNameCount = 0u;
        for (auto&& [index, concrete] : std::views::enumerate(catalog.Structs))
        {
            if (concrete.SlangName != layout.Name)
            {
                continue;
            }

            if (concrete.Layout == layout)
            {
                return static_cast<uint32_t>(index);
            }

            ++sameNameCount;
        }

        const uint32_t newIndex = static_cast<uint32_t>(catalog.Structs.size());
        catalog.Structs.push_back(ConcreteStruct{ .SlangName = layout.Name,
                                                  .Identifier = std::format("{}_{}", MakeIdentifier(layout.Name), sameNameCount),
                                                  .Layout = std::move(layout) });

        return newIndex;
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
            text += std::format("template <CookProfile P, ::lodestone::LayoutRule R>\nstruct {}Select;\n\n", identifier);
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

    std::vector<std::string_view> UsedBindingNames(const CookedModule& module,
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
        std::vector<std::string_view> names;
        names.reserve(visible.size());
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
                names.emplace_back(binding.Name);
            }
        }

        std::ranges::sort(names);
        const auto [duplicatesBegin, duplicatesEnd] = std::ranges::unique(names);
        names.erase(duplicatesBegin, duplicatesEnd);

        return names;
    }

    void AddEnvironmentPresence(const CookedModule& module,
                                size_t entry_point_index,
                                size_t profile_index,
                                BindingPresence& presence)
    {
        std::vector<std::vector<std::string_view>> namesOfVariant;
        namesOfVariant.reserve(module.Variants.size());
        for (const LibraryVariant& variant : module.Variants)
        {
            std::vector<std::string_view>& names = namesOfVariant.emplace_back(UsedBindingNames(module, variant, entry_point_index));
            for (const std::string_view name : names)
            {
                std::vector<size_t>& counts = presence.VariantsWithBinding[std::string{ name }];
                counts.resize(presence.VariantCounts.size(), 0u);
                counts[profile_index] += 1u;
            }
        }

        presence.VariantCounts[profile_index] = module.Variants.size();

        // for each axis, the variants that agree on every other axis form one class, found with a key
        std::span<const PermutationAxis> axes = module.Space->Axes();
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

        for (auto&& [axisIndex, axis] : std::views::enumerate(axes))
        {
            std::unordered_map<std::string, std::vector<size_t>> classes;
            for (size_t variantIndex = 0u; variantIndex < valuesOfVariant.size(); ++variantIndex)
            {
                std::string key;
                for (auto&& [otherIndex, value] : std::views::enumerate(valuesOfVariant[variantIndex]))
                {
                    key += otherIndex == axisIndex ? std::string{ "*" } : value;
                    key += '|';
                }

                classes[key].push_back(variantIndex);
            }

            for (const auto& [key, members] : classes)
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
                        presence.AxesChangingBinding[std::string{ name }].insert(axis.Name);
                    }
                }
            }
        }
    }

    BindingPresence CollectBindingPresence(const CookedLibrary& library, size_t module_index, size_t entry_point_index)
    {
        BindingPresence presence;
        presence.VariantCounts.assign(library.Profiles.size(), 0u);
        const size_t moduleCount = library.ModuleNames.size();
        for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
        {
            const std::optional<CookedModule>& environment = library.Environments[(profileIndex * moduleCount) + module_index];
            if (environment.has_value())
            {
                AddEnvironmentPresence(*environment, entry_point_index, profileIndex, presence);
            }
        }

        return presence;
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

    /** Returns the comment for the cooked profiles that do not use the binding, or an empty string. */
    std::string DescribeMissingProfiles(const CookedLibrary& library,
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

        return names.empty() ? std::string{} : std::format("// not a binding on profiles: {}\n", names);
    }

    std::map<size_t, uint32_t> CollectParamsSelection(const CookedLibrary& library,
                                                      const HostTypeCatalog& catalog,
                                                      size_t module_index,
                                                      size_t entry_point_index)
    {
        std::map<size_t, uint32_t> params;
        const size_t moduleCount = library.ModuleNames.size();
        for (size_t profileIndex = 0u; profileIndex < library.Profiles.size(); ++profileIndex)
        {
            const size_t environmentIndex = (profileIndex * moduleCount) + module_index;
            const std::optional<CookedModule>& environment = library.Environments[environmentIndex];
            if (!environment.has_value())
            {
                continue;
            }

            const std::vector<uint32_t>& remap = catalog.EnvironmentRemaps[environmentIndex];
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

    void AppendBindingConstant(std::string& text, std::string_view name, std::string_view indent)
    {
        text += std::format("{}inline constexpr std::string_view {}{{ \"{}\" }};\n", indent, MakeIdentifier(name), name);
    }

    void AppendParams(std::string& text,
                      const HostTypeCatalog& catalog,
                      const std::map<size_t, uint32_t>& params,
                      std::string_view qualified_prefix)
    {
        text += "\nnamespace detail\n{\n\ntemplate <CookProfile P>\nstruct ParamsSelect;\n\n";
        for (const auto& [profileIndex, concreteIndex] : params)
        {
            text += std::format("template <>\nstruct ParamsSelect<CookProfile::{}>\n{{\n"
                                "    using Type = {}detail::{};\n}};\n\n",
                                catalog.ProfileEnumerators[profileIndex],
                                qualified_prefix,
                                catalog.Structs[concreteIndex].Identifier);
        }

        text += "} // namespace detail\n\n";
        text += "// The constants of the entry point: a push constant block or a uniform buffer, by profile.\n";
        text += "template <CookProfile P>\nusing Params = typename detail::ParamsSelect<P>::Type;\n";
    }

    CookError AppendEntryPoint(std::string& text,
                               const CookedLibrary& library,
                               const HostTypeCatalog& catalog,
                               std::string_view qualified_prefix,
                               size_t module_index,
                               size_t entry_point_index,
                               DiagnosticSink& sink)
    {
        const CookedModule& first = *FindFirstEnvironment(library, module_index);
        const std::string& entryPointName = first.EntryPoints[entry_point_index].Name;
        const BindingPresence presence = CollectBindingPresence(library, module_index, entry_point_index);
        const std::map<size_t, uint32_t> params = CollectParamsSelection(library, catalog, module_index, entry_point_index);

        std::set<std::string> everyVariant;
        std::set<std::string> someVariants;
        for (const auto& [name, counts] : presence.VariantsWithBinding)
        {
            (LacksInSomeVariant(presence, counts) ? someVariants : everyVariant).insert(name);
        }

        std::map<std::string, std::string> nameOfIdentifier{ { "Params", "Params" },
                                                             { "SomeVariants", "SomeVariants" },
                                                             { "detail", "detail" } };
        for (const auto& [name, counts] : presence.VariantsWithBinding)
        {
            const auto [position, inserted] = nameOfIdentifier.try_emplace(MakeIdentifier(name), name);
            if (!inserted)
            {
                return ReportError(sink,
                                   CookError::HostTypeNameCollision,
                                   std::format("in entry point '{}', the binding name '{}' and the name '{}' both map to "
                                               "the C++ identifier '{}'",
                                               entryPointName,
                                               name,
                                               position->second,
                                               position->first));
            }
        }

        text += std::format("namespace {}\n{{\n\n", MakeIdentifier(entryPointName));
        for (const std::string& name : everyVariant)
        {
            text += DescribeMissingProfiles(library, presence, presence.VariantsWithBinding.at(name));
            AppendBindingConstant(text, name, "");
        }

        if (!someVariants.empty())
        {
            text += "\nnamespace SomeVariants\n{\n\n";
            for (const std::string& name : someVariants)
            {
                const auto changing = presence.AxesChangingBinding.find(name);
                if (changing == presence.AxesChangingBinding.end())
                {
                    text += "// present or absent with a combination of axes\n";
                }
                else
                {
                    std::string axisList;
                    for (const std::string& axisName : changing->second)
                    {
                        axisList += axisList.empty() ? "" : ", ";
                        axisList += axisName;
                    }

                    text += std::format("// present or absent with the axes: {}\n", axisList);
                }

                text += DescribeMissingProfiles(library, presence, presence.VariantsWithBinding.at(name));
                AppendBindingConstant(text, name, "");
            }

            text += "\n} // namespace SomeVariants\n";
        }

        if (!params.empty())
        {
            AppendParams(text, catalog, params, qualified_prefix);
        }

        text += std::format("\n}} // namespace {}\n\n", MakeIdentifier(entryPointName));

        return CookError::Success;
    }

    CookError AppendModules(std::string& text,
                            const CookedLibrary& library,
                            const HostTypeCatalog& catalog,
                            std::string_view qualified_prefix,
                            DiagnosticSink& sink)
    {
        std::map<std::string, std::string> nameOfIdentifier{ { "CookProfile", "CookProfile" }, { "detail", "detail" } };
        for (const auto& [identifier, slangName] : catalog.NameOfIdentifier)
        {
            nameOfIdentifier.emplace(identifier, slangName);
        }

        for (size_t moduleIndex = 0u; moduleIndex < library.ModuleNames.size(); ++moduleIndex)
        {
            const CookedModule* first = FindFirstEnvironment(library, moduleIndex);
            if (first == nullptr)
            {
                continue;
            }

            const std::string& moduleName = library.ModuleNames[moduleIndex];
            const std::string moduleIdentifier = MakeIdentifier(moduleName);
            const auto [position, inserted] = nameOfIdentifier.try_emplace(moduleIdentifier, moduleName);
            if (!inserted)
            {
                return ReportError(sink,
                                   CookError::HostTypeNameCollision,
                                   std::format("the module name '{}' and the name '{}' both map to the C++ identifier '{}' "
                                               "in the host header",
                                               moduleName,
                                               position->second,
                                               moduleIdentifier));
            }

            text += std::format("namespace {}\n{{\n\n", moduleIdentifier);

            std::map<std::string, std::string> entryPointOfIdentifier;
            for (size_t entryPointIndex = 0u; entryPointIndex < first->EntryPoints.size(); ++entryPointIndex)
            {
                const std::string& entryPointName = first->EntryPoints[entryPointIndex].Name;
                const auto [entryPosition, entryInserted] =
                    entryPointOfIdentifier.try_emplace(MakeIdentifier(entryPointName), entryPointName);
                if (!entryInserted)
                {
                    return ReportError(sink,
                                       CookError::HostTypeNameCollision,
                                       std::format("in module '{}', the entry point names '{}' and '{}' both map to the "
                                                   "C++ identifier '{}'",
                                                   moduleName,
                                                   entryPointName,
                                                   entryPosition->second,
                                                   entryPosition->first));
                }

                const CookError entryPointError =
                    AppendEntryPoint(text, library, catalog, qualified_prefix, moduleIndex, entryPointIndex, sink);
                if (!entryPointError)
                {
                    return entryPointError;
                }
            }

            text += std::format("}} // namespace {}\n\n", moduleIdentifier);
        }

        return CookError::Success;
    }

} // namespace

} // namespace lodestone
