#pragma once
#ifndef LODESTONE_SHADER_SYMBOL_TABLE_HPP
#define LODESTONE_SHADER_SYMBOL_TABLE_HPP
#include "TransparentHash.hpp"
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lodestone
{
    /** @brief One `extern static const <Var> = <Value>;` declaration. Views into the source. */
    struct ExternConstantDeclaration
    {
        std::string_view Name;
        std::string_view Value;
        constexpr bool operator==(const ExternConstantDeclaration& other) const noexcept
        {
            return Name == other.Name && Value == other.Value;
        }
    };

    /** @brief A non-`extern` `static const`, kept as written so it can be evaluated per variant. */
    struct DerivedConstantDeclaration
    {
        std::string_view Name;
        std::string_view Expression;
        std::string_view SourcePath;
        uint32_t SourceLine{ 0u };
    };

    struct ExternConstHash
    {
        std::size_t operator()(const ExternConstantDeclaration& decl) const noexcept
        {
            std::size_t hash1 = std::hash<std::string_view>{}(decl.Name);
            std::size_t hash2 = std::hash<std::string_view>{}(decl.Value);
            constexpr static std::size_t k_GoldenRatioFract = 0x9e3779b9;
            return hash1 ^ (hash2 + k_GoldenRatioFract + (hash1 << 6) + (hash1 >> 2));
        }
    };

    /** @brief The unique tokens of the shader sources, used to prune axes no source uses. Views into the sources. */
    struct SymbolTable
    {
        SymbolTable() = default;
        void AddSource(std::string_view module_name, std::string_view source_path, std::string_view source_code) noexcept;
        [[nodiscard]] std::vector<std::string_view> MissingTokens(std::span<const std::string> module_names,
                                                                  std::span<const std::string_view> tokens) const;
        [[nodiscard]] std::vector<ExternConstantDeclaration> ExternConstantsForModule(std::string_view module_name) const;
        /** @brief Kept apart from the extern constants: a type default is never an expression. */
        [[nodiscard]] std::vector<ExternConstantDeclaration> ExternTypeDefaultsForModule(std::string_view module_name) const;
        /** @brief In source order. A name can repeat, and the caller decides which are module scope. */
        [[nodiscard]] std::span<const DerivedConstantDeclaration> DerivedConstantsForModule(std::string_view module_name) const;
    private:
        using TokenSet = std::unordered_set<std::string_view>;
        std::unordered_map<std::string_view, TokenSet, TransparentStringHash, std::equal_to<>> tokenMap;
        using ExternConstSet = std::unordered_set<ExternConstantDeclaration, ExternConstHash>;
        std::unordered_map<std::string_view, ExternConstSet> externConstMap;
        std::unordered_map<std::string_view, ExternConstSet> externTypeDefaultMap;
        std::unordered_map<std::string_view, std::vector<DerivedConstantDeclaration>> derivedConstantMap;
    };
}

#endif // !LODESTONE_SHADER_SYMBOL_TABLE_HPP
