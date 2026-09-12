#pragma once
#ifndef LODESTONE_SHADER_SYMBOL_TABLE_HPP
#define LODESTONE_SHADER_SYMBOL_TABLE_HPP
#include "TransparentHash.hpp"
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lodestone
{
    /**@brief For each `extern static const <Var> = <Value>;` decl in the shader source, we capture
     * the variable name and its value. during the symbol table build. This was originally done in it's
     * own pass over the source line-by-line, but with symbol table construction we can now share that
     * traversal work to get two datasets (tokens and extern constant declarations) simultaneously.
     * @note Like the original, it uses string_views since source strings are in memory all-session. */
    struct ExternConstantDeclaration
    {
        std::string_view Name;
        std::string_view Value;
    };

    struct ExternConstHash
    {
        std::size_t operator()(const ExternConstantDeclaration& decl) const noexcept
        {
            // use boost hash-combine method since it's more succinct than full fnv-1a
            // but not nearly as collision-prone as a simple xor of the two hashes
            std::size_t hash1 = std::hash<std::string_view>{}(decl.Name);
            std::size_t hash2 = std::hash<std::string_view>{}(decl.Value);
            constexpr static std::size_t goldenRatioFract = 0x9e3779b9;
            return hash1 ^ (hash2 + goldenRatioFract + (hash1 << 6) + (hash1 >> 2));
        }
    };

    /**@brief Given an input shader source string, constructs a symbol table of unique variable tokens.
      *Skips parsing `extern static const`, and removes reserved keywords to reduce the size of the search. Returns
      *an unordered set containing the unique variable tokens found in the input shader source string: used by 
      *PermutationSpace construction to see which axis names/variables are actually used in source code.
      *@note Uses string_view: the original source must persist */
    struct SymbolTable
    {
        SymbolTable() = default;
        /**@brief Adds given source code for `module_name` to the table - strips out reserved keywords
         * and extern static const decls. */
        void AddSource(std::string_view module_name, std::string_view source_code) noexcept;
        /** @brief Returns the set of tokens not found in the symbol tables for the given modules */
        [[nodiscard]] std::vector<std::string_view> MissingTokens(std::span<std::string_view> module_names,
                                                                  std::span<std::string_view> tokens) const;
    private:
        using TokenSet = std::unordered_set<std::string_view>;
        std::unordered_map<std::string_view, TokenSet, TransparentStringHash, std::equal_to<>> tokenMap;
        using ExternConstSet = std::unordered_set<ExternConstantDeclaration, ExternConstHash>;
        std::unordered_map<std::string_view, ExternConstSet> externConstMap;
    };
}

#endif // !LODESTONE_SHADER_SYMBOL_TABLE_HPP
