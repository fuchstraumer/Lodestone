#pragma once
#ifndef LODESTONE_DEPFILE_HPP
#define LODESTONE_DEPFILE_HPP
#include <span>
#include <string>
#include <string_view>

/** The Makefile-form depfile that lists every file a cook read, including imports and `__include`s. */
namespace lodestone
{

inline constexpr std::string_view k_DepfileName = "ShaderLibrary.d";

/** The caller gives sorted, unique, absolute paths with `/`, so that the bytes are deterministic. */
[[nodiscard]] std::string GenerateDepfile(std::string_view target, std::span<const std::string> prerequisites);

} // namespace lodestone

#endif // !LODESTONE_DEPFILE_HPP
