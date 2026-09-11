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
    };
}

#endif // !LODESTONE_SHADER_SYMBOL_TABLE_HPP
