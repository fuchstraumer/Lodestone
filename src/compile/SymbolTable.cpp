#include "compile/SymbolTable.hpp"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{
    namespace
    {

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

        /**@brief Pulls out a variable identifier from 'line', by using 'end_index' - which should be the
         * location of an `=` character. */
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
            // run backwards to find id_start now
            size_t idStartIdx = line.find_last_not_of(k_ValidIdChars, idEndIdx);
            // asserting on npos because it should be nearly impossible for that to happen: we only enter
            // this code if the line has `extern const static` etc in it, and that precedes the identifier
            assert(idStartIdx != std::string_view::npos);
            // increment idStartIdx to point to the actual start of the identifier
            ++idStartIdx;
            return line.substr(idStartIdx, idEndIdx - idStartIdx + 1);
        }

        /**@brief Pulls out a value assignment from line, by starting *after* the `=` character we find for
         * the identifier extraction above. Much wider space of permissible characters though, so we just
         * trim whitespace and pull out the whole chunk between `=` and `;`. */
        constexpr std::string_view ExtractValueAssignment(std::string_view line, size_t assignment_index) noexcept
        {
            const size_t valueStartIdx = assignment_index + 1;
            const size_t valueEndIdx = line.find(';', valueStartIdx);
            // if no semicolon, just take the rest of the line
            std::string_view valueStr = valueEndIdx == std::string_view::npos ?
                                        line.substr(valueStartIdx) :
                                        line.substr(valueStartIdx, valueEndIdx - valueStartIdx);
            return TrimWhitespace(valueStr);
        }

        ExternConstantDeclaration ExtractExternConst(std::string_view line)
        {
            const size_t assignmentIndex = line.find('=');
            const std::string_view identifier = ExtractIdentifier(line, assignmentIndex);
            const std::string_view value = ExtractValueAssignment(line, assignmentIndex);
            return ExternConstantDeclaration{ .Name=identifier, .Value=value };
        }
    }

    // Reserved words and built-in type names, so a token that can never be a user identifier does not
    // enter the symbol table. Sorted by byte value, which is the order `string_view::operator<` uses,
    // so `binary_search` below is correct. The list holds only genuinely reserved words and built-in
    // type names -- never intrinsics like `dot` or `mul`, because an author may shadow an intrinsic
    // name with a real identifier, and filtering it would drop a token that could be an axis.
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

    void SymbolTable::AddSource(std::string_view module_name, std::string_view source_code) noexcept
    {
        // Accumulate tokens for this module - one module may receive tokens from
        // several sources, due to how slang handles `__include` directives.
        TokenSet& tokens = tokenMap[module_name];
        ExternConstSet& externConsts = externConstMap[module_name];
        size_t lineStart = 0u;

        while (std::cmp_less(lineStart, source_code.size()))
        {
            size_t lineEnd = source_code.find('\n', lineStart);
            if (lineEnd == std::string_view::npos)
            {
                lineEnd = source_code.size();
            }

            std::string_view line = source_code.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;

            // We check for these individually to avoid ordering dependencies:
            // `extern static const` passes and so does `static extern const` etc
            if (line.contains("extern") &&
                line.contains("static") &&
                line.contains("const") &&
                line.contains("="))
            {
                ExternConstantDeclaration decl = ExtractExternConst(line);
                externConsts.emplace(decl);
                continue;
            }

            // chunk_by works great here, since this is single pass: it's just a subview,
            // not even doing any copying (and it's lazily evaluated)
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

    std::vector<std::string_view> SymbolTable::MissingTokens(std::span<std::string_view> module_names,
                                                             std::span<std::string_view> tokens) const
    {
        std::vector<std::string_view> missing;
        for (auto token : tokens)
        {
            bool found = false;
            for (auto moduleName : module_names)
            {
                auto iter = tokenMap.find(moduleName);
                if (iter != tokenMap.end() && iter->second.contains(token))
                {
                    // break because finding token in any of the modules is good enough for us
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
}
