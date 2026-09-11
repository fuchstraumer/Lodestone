#include "compile/SymbolTable.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{
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
        if (tokenMap.contains(module_name))
        {
            return;
        }

        auto isIdentifierChar = [](char c) -> bool
        {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
        };

        TokenSet tokens;
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
                line.contains("const"))
            {
                continue;
            }

            auto chunkOperator = [&](char lhs, char rhs) -> bool
            {
                return isIdentifierChar(lhs) == isIdentifierChar(rhs);
            };

            auto chunkedLineView = line | std::views::chunk_by(chunkOperator);

            for (auto chunk : chunkedLineView)
            {
                if ((std::isalpha(static_cast<unsigned char>(chunk.front())) == 0) &&
                    (chunk.front() != '_'))
                {
                    continue;
                }

                std::string_view token(std::ranges::data(chunk), std::ranges::size(chunk));
                if (IsReservedKeyword(token))
                {
                    continue;
                }

                tokens.emplace(token);
            }
        }

        tokenMap.emplace(module_name, std::move(tokens));
  
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
}
