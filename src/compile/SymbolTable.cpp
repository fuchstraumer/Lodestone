#include "compile/SymbolTable.hpp"
#include "EnumClassUtils.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{
    namespace
    {

        /** @brief Flags, so the keywords combine in any order, as in C. */
        enum class DeclarationKeyword : uint8_t
        {
            None = 0,
            Extern = 1u << 0u,
            Static = 1u << 1u,
            Const = 1u << 2u,
            Struct = 1u << 3u,
        };
        MAKE_ENUM_CLASS_FLAGS(DeclarationKeyword)

        /** @brief Returns the part of `line` that is code. A `//` comment ends the line, a block comment that opens the line is
         * dropped, a block comment inside a declaration stays, and a block comment that does not close on the line starts a
         * comment that `in_block_comment` carries to the next lines. Views keep pointing into the source. */
        std::string_view StripComments(std::string_view line, bool& in_block_comment) noexcept;

        /** @brief Matches whole tokens, so a type named `staticconst_buffer` sets no flag. */
        DeclarationKeyword ScanDeclarationKeywords(std::string_view declaration) noexcept;

        ExternConstantDeclaration ExtractExternConst(std::string_view line, size_t assignment_index);

        /** @brief Reads `static const <Type> <Name> = <Expression>;` on one line. No value for arrays. */
        std::optional<DerivedConstantDeclaration> ExtractDerivedConstant(std::string_view line,
                                                                          size_t assignment_index,
                                                                          std::string_view source_path,
                                                                          uint32_t source_line);

        /** @brief Reads `extern struct <Name> : <Interface><<Arguments>> [= <Default>];`. The declaration ends at the first
         * `=`, `;`, or `{`, and a line comment is ignored. */
        std::optional<ExternStructDeclaration> ExtractExternStruct(std::string_view line,
                                                                   std::string_view source_path,
                                                                   uint32_t source_line);

        /** @brief Reads `extern struct <Name> : <Interface> = <Default>;`. The name follows `struct`. */
        std::optional<ExternConstantDeclaration> ExtractExternTypeDefault(std::string_view line, size_t assignment_index);

        constexpr bool IsIdentifierStartCharacter(char character) noexcept
        {
            return (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   character == '_';
        }

        constexpr bool IsIdentifierCharacter(char character) noexcept
        {
            return (character >= '0' && character <= '9') ||
                   (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   character == '_';
        }

        constexpr bool IsValidIdentifier(std::string_view identifier) noexcept
        {
            if (identifier.empty() || !IsIdentifierStartCharacter(identifier.front()))
            {
                return false;
            }

            return std::ranges::all_of(identifier.substr(1), IsIdentifierCharacter);
        }

        constexpr std::string_view TrimWhitespace(std::string_view str) noexcept
        {
            constexpr std::string_view k_WhiteSpaceChars = " \t\n\r\f";
            const size_t start = str.find_first_not_of(k_WhiteSpaceChars);
            if (start == std::string_view::npos)
            {
                return {};
            }
            const size_t end = str.find_last_not_of(k_WhiteSpaceChars);
            return str.substr(start, end - start + 1);
        }

        constexpr bool ChunkSeparator(char lhs, char rhs) noexcept
        {
            return IsIdentifierCharacter(lhs) == IsIdentifierCharacter(rhs);
        }

        /** @brief Returns the identifier before the `=` at `end_index`. */
        constexpr std::string_view ExtractIdentifier(std::string_view line, size_t end_index) noexcept
        {
            constexpr std::string_view k_ValidIdChars =
                "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
            // time to admit I just realized find_last_of runs in reverse (from the end_index backwards)
            const size_t idEndIdx = line.find_last_of(k_ValidIdChars, end_index - 1);
            if (idEndIdx == std::string_view::npos)
            {
                return {};
            }

            size_t idStartIdx = line.find_last_not_of(k_ValidIdChars, idEndIdx);
            assert(idStartIdx != std::string_view::npos);
            ++idStartIdx;
            return line.substr(idStartIdx, idEndIdx - idStartIdx + 1);
        }

        /** @brief Returns the trimmed text between `=` and `;`. */
        constexpr std::string_view ExtractValueAssignment(std::string_view line, size_t assignment_index) noexcept
        {
            const size_t valueStartIdx = assignment_index + 1;
            const size_t valueEndIdx = line.find(';', valueStartIdx);
            std::string_view valueStr = valueEndIdx == std::string_view::npos ?
                                        line.substr(valueStartIdx) :
                                        line.substr(valueStartIdx, valueEndIdx - valueStartIdx);
            return TrimWhitespace(valueStr);
        }
    }

    // sorted for binary_search. Reserved words and type names only: an author may shadow an intrinsic with an axis
    static constexpr std::string_view k_ReservedKeywords[]
    {
        "AppendStructuredBuffer", "Buffer", "ByteAddressBuffer", "ConstantBuffer", "ConsumeStructuredBuffer",
        "ParameterBlock", "RWBuffer", "RWByteAddressBuffer", "RWStructuredBuffer", "RWTexture1D",
        "RWTexture1DArray", "RWTexture2D", "RWTexture2DArray", "RWTexture3D", "RaytracingAccelerationStructure",
        "SV_DispatchThreadID", "SV_GroupID", "SV_GroupIndex", "SV_GroupThreadID", "SV_InstanceID",
        "SV_Position", "SV_Target", "SV_VertexID", "SamplerComparisonState", "SamplerState",
        "StructuredBuffer", "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture2DMS",
        "Texture2DMSArray", "Texture3D", "TextureCube", "TextureCubeArray", "__exported", "__generic",
        "__init", "__subscript", "associatedtype", "bool", "bool2", "bool3", "bool4", "break", "case",
        "cbuffer", "centroid", "class", "column_major", "const", "continue", "default", "discard", "do",
        "double", "double2", "double3", "double4", "else", "enum", "export", "extension", "extern", "false",
        "float", "float16_t", "float2", "float2x2", "float2x3", "float2x4", "float3", "float32_t", "float3x2",
        "float3x3", "float3x4", "float4", "float4x2", "float4x3", "float4x4", "float64_t", "for",
        "groupshared", "half", "half2", "half3", "half4", "if", "import", "in", "inline", "inout", "int",
        "int16_t", "int2", "int3", "int32_t", "int4", "int64_t", "int8_t", "interface", "internal", "linear",
        "min10float", "min12int", "min16float", "min16int", "min16uint", "namespace", "nointerpolation",
        "noperspective", "null", "out", "packoffset", "precise", "private", "property", "public", "register",
        "return", "row_major", "sample", "sampler", "shared", "static", "struct", "switch", "tbuffer", "this",
        "true", "typedef", "typename", "uint", "uint16_t", "uint2", "uint3", "uint32_t", "uint4", "uint64_t",
        "uint8_t", "uniform", "void", "volatile", "where", "while"
    };

    // constexpr is_sorted is SO COOL
    static_assert(std::ranges::is_sorted(k_ReservedKeywords),
                  "k_ReservedKeywords must stay sorted by byte value, or binary_search below is wrong");

    static constexpr bool IsReservedKeyword(std::string_view keyword) noexcept
    {
        return std::ranges::binary_search(k_ReservedKeywords, keyword);
    }

    void SymbolTable::AddSource(std::string_view module_name,
                                std::string_view source_path,
                                std::string_view source_code) noexcept
    {
        // `__include` gives one module several sources
        TokenSet& tokens = tokenMap[module_name];
        ExternConstSet& externConsts = externConstMap[module_name];
        ExternConstSet& externTypeDefaults = externTypeDefaultMap[module_name];
        std::vector<DerivedConstantDeclaration>& derivedConstants = derivedConstantMap[module_name];
        std::vector<ExternStructDeclaration>& externStructs = externStructMap[module_name];
        size_t lineStart = 0u;
        uint32_t lineNumber = 0u;
        bool inBlockComment = false;

        while (std::cmp_less(lineStart, source_code.size()))
        {
            size_t lineEnd = source_code.find('\n', lineStart);
            if (lineEnd == std::string_view::npos)
            {
                lineEnd = source_code.size();
            }

            // a comment declares nothing: the doc comment of a builtin can show `extern static const` as an example
            std::string_view line = StripComments(source_code.substr(lineStart, lineEnd - lineStart), inBlockComment);
            lineStart = lineEnd + 1;
            ++lineNumber;

            // a declaration with no default has no `=`, so the extractor reads the header up to the first `=`, `;`, or `{`
            if (line.contains("extern"))
            {
                if (const std::optional<ExternStructDeclaration> externStruct =
                        ExtractExternStruct(line, source_path, lineNumber))
                {
                    externStructs.push_back(*externStruct);
                }
            }

            const size_t assignmentIndex = line.find('=');
            const DeclarationKeyword keywords = assignmentIndex == std::string_view::npos
                                                    ? DeclarationKeyword::None
                                                    : ScanDeclarationKeywords(line.substr(0, assignmentIndex));

            const bool isExtern = HasAllFlags(keywords, DeclarationKeyword::Extern);
            const bool isStaticConst = HasAllFlags(keywords, DeclarationKeyword::Static | DeclarationKeyword::Const);

            // no `continue`: the expression names axes, which count as tokens
            if (isStaticConst && !isExtern)
            {
                if (const std::optional<DerivedConstantDeclaration> derived =
                        ExtractDerivedConstant(line, assignmentIndex, source_path, lineNumber))
                {
                    derivedConstants.push_back(*derived);
                }
            }

            // no `continue`: the struct name counts as a token
            if (isExtern && HasAllFlags(keywords, DeclarationKeyword::Struct))
            {
                if (const std::optional<ExternConstantDeclaration> typeDefault =
                        ExtractExternTypeDefault(line, assignmentIndex))
                {
                    externTypeDefaults.emplace(*typeDefault);
                }
            }

            if (isExtern && isStaticConst)
            {
                externConsts.emplace(ExtractExternConst(line, assignmentIndex));
                continue;
            }

            auto chunkedLineView = line | std::views::chunk_by(ChunkSeparator);

            for (auto chunk : chunkedLineView)
            {
                std::string_view token(std::ranges::data(chunk), std::ranges::size(chunk));
                if (!IsValidIdentifier(token) || IsReservedKeyword(token))
                {
                    continue;
                }

                tokens.emplace(token);
            }
        }
    }

    std::vector<std::string_view> SymbolTable::MissingTokens(std::span<const std::string> module_names,
                                                             std::span<const std::string_view> tokens) const
    {
        std::vector<std::string_view> missing;
        for (auto token : tokens)
        {
            bool found = false;
            for (const std::string& moduleName : module_names)
            {
                auto iter = tokenMap.find(moduleName);
                if (iter != tokenMap.end() && iter->second.contains(token))
                {
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                missing.emplace_back(token);
            }
        }
        
        return missing;
    }

    std::span<const DerivedConstantDeclaration> SymbolTable::DerivedConstantsForModule(std::string_view module_name) const
    {
        const auto iter = derivedConstantMap.find(module_name);
        if (iter != derivedConstantMap.end())
        {
            return iter->second;
        }
        
        return {};
    }

    std::span<const ExternStructDeclaration> SymbolTable::ExternStructsForModule(std::string_view module_name) const
    {
        const auto iter = externStructMap.find(module_name);
        if (iter != externStructMap.end())
        {
            return iter->second;
        }

        return {};
    }

    std::vector<ExternConstantDeclaration> SymbolTable::ExternTypeDefaultsForModule(std::string_view module_name) const
    {
        const auto iter = externTypeDefaultMap.find(module_name);
        if (iter != externTypeDefaultMap.end())
        {
            return std::vector<ExternConstantDeclaration>(iter->second.begin(), iter->second.end());
        }

        return {};
    }

    std::vector<ExternConstantDeclaration> SymbolTable::ExternConstantsForModule(std::string_view module_name) const
    {
        const auto iter = externConstMap.find(module_name);
        if (iter != externConstMap.end())
        {
            const ExternConstSet& externConsts = iter->second;
            return std::vector<ExternConstantDeclaration>(externConsts.begin(), externConsts.end());
        }

        return {};
    }

    namespace
    {
        std::string_view StripComments(std::string_view line, bool& in_block_comment) noexcept
        {
            if (in_block_comment)
            {
                const size_t close = line.find("*/");
                if (close == std::string_view::npos)
                {
                    return {};
                }
                in_block_comment = false;
                line = line.substr(close + 2u);
            }

            bool inString = false;
            for (size_t i = 0u; i < line.size(); ++i)
            {

                const char character = line[i];
                if (character == '"')
                {
                    inString = !inString;
                }
                else if (!inString && character == '/' && i + 1u < line.size())
                {
                    if (line[i + 1u] == '/')
                    {
                        return line.substr(0u, i);
                    }

                    if (line[i + 1u] == '*')
                    {
                        const size_t close = line.find("*/", i + 2u);
                        if (close == std::string_view::npos)
                        {
                            in_block_comment = true;
                            return line.substr(0u, i);
                        }

                        // a comment that opens the line is dropped, and code may follow it
                        const std::string_view beforeComment = line.substr(0u, i);
                        const size_t beforeCommentNonWhitespace = beforeComment.find_first_not_of(" \t\r");
                        const bool opensLine = beforeCommentNonWhitespace == std::string_view::npos;
                        if (opensLine)
                        {
                            return StripComments(line.substr(close + 2u), in_block_comment);
                        }
                        
                        i = close + 1u;
                    }
                }

            }

            return line;
        }

        DeclarationKeyword ScanDeclarationKeywords(std::string_view declaration) noexcept
        {
            DeclarationKeyword keywords = DeclarationKeyword::None;
            for (auto chunk : declaration | std::views::chunk_by(ChunkSeparator))
            {
                const std::string_view token(std::ranges::data(chunk), std::ranges::size(chunk));
                if (token == "extern")
                {
                    keywords |= DeclarationKeyword::Extern;
                }
                else if (token == "static")
                {
                    keywords |= DeclarationKeyword::Static;
                }
                else if (token == "const")
                {
                    keywords |= DeclarationKeyword::Const;
                }
                else if (token == "struct")
                {
                    keywords |= DeclarationKeyword::Struct;
                }
            }
            return keywords;
        }

        ExternConstantDeclaration ExtractExternConst(std::string_view line, size_t assignment_index)
        {
            const std::string_view identifier = ExtractIdentifier(line, assignment_index);
            const std::string_view value = ExtractValueAssignment(line, assignment_index);
            return ExternConstantDeclaration{ .Name=identifier, .Value=value };
        }

        std::optional<DerivedConstantDeclaration> ExtractDerivedConstant(std::string_view line,
                                                                          size_t assignment_index,
                                                                          std::string_view source_path,
                                                                          uint32_t source_line)
        {
            const std::string_view name = ExtractIdentifier(line, assignment_index);
            const std::string_view expression = ExtractValueAssignment(line, assignment_index);
            if (!IsValidIdentifier(name) || expression.empty())
            {
                return std::nullopt;
            }
            return DerivedConstantDeclaration{ .Name = name,
                                               .Expression = expression,
                                               .SourcePath = source_path,
                                               .SourceLine = source_line };
        }

        std::optional<ExternStructDeclaration> ExtractExternStruct(std::string_view line,
                                                                   std::string_view source_path,
                                                                   uint32_t source_line)
        {
            const size_t commentIndex = line.find("//");
            const std::string_view code = commentIndex == std::string_view::npos ? line : line.substr(0, commentIndex);
            const size_t endIndex = code.find_first_of("=;{");
            const std::string_view header = endIndex == std::string_view::npos ? code : code.substr(0, endIndex);
            if (!HasAllFlags(ScanDeclarationKeywords(header), DeclarationKeyword::Extern | DeclarationKeyword::Struct))
            {
                return std::nullopt;
            }

            const size_t colonIndex = header.find(':');
            if (colonIndex == std::string_view::npos)
            {
                return std::nullopt;
            }

            std::string_view name;
            bool sawStruct = false;
            for (auto chunk : header.substr(0, colonIndex) | std::views::chunk_by(ChunkSeparator))
            {
                const std::string_view token(std::ranges::data(chunk), std::ranges::size(chunk));
                if (!IsValidIdentifier(token))
                {
                    continue;
                }

                if (sawStruct)
                {
                    name = token;
                    break;
                }

                sawStruct = token == "struct";
            }

            if (name.empty())
            {
                return std::nullopt;
            }

            const std::string_view conformance = TrimWhitespace(header.substr(colonIndex + 1u));
            const size_t openIndex = conformance.find('<');
            
            const std::string_view interfaceName = TrimWhitespace(conformance.substr(0, openIndex));
            if (!IsValidIdentifier(interfaceName))
            {
                return std::nullopt;
            }

            std::string_view typeArguments;
            if (openIndex != std::string_view::npos)
            {
                const size_t closeIndex = conformance.rfind('>');
                if (closeIndex == std::string_view::npos || closeIndex < openIndex)
                {
                    return std::nullopt;
                }
                typeArguments = TrimWhitespace(conformance.substr(openIndex + 1u, closeIndex - openIndex - 1u));
            }

            return ExternStructDeclaration{ .Name = name,
                                            .Interface = interfaceName,
                                            .TypeArguments = typeArguments,
                                            .SourcePath = source_path,
                                            .SourceLine = source_line };
        }

        std::optional<ExternConstantDeclaration> ExtractExternTypeDefault(std::string_view line, size_t assignment_index)
        {
            bool sawStruct = false;
            for (auto chunk : line.substr(0, assignment_index) | std::views::chunk_by(ChunkSeparator))
            {
                const std::string_view token(std::ranges::data(chunk), std::ranges::size(chunk));
                if (!IsValidIdentifier(token))
                {
                    continue;
                }
                if (sawStruct)
                {
                    return ExternConstantDeclaration{ .Name = token,
                                                      .Value = ExtractValueAssignment(line, assignment_index) };
                }
                sawStruct = token == "struct";
            }
            return std::nullopt;
        }
    }
}
