#pragma once
#ifndef LODESTONE_ARTIFACT_NAMES_HPP
#define LODESTONE_ARTIFACT_NAMES_HPP
#include <string>
#include <string_view>

/** The file names of the artifacts of one cook. Each name is the output name (the stem) and a suffix. */
namespace lodestone
{

inline constexpr std::string_view k_DefaultOutputName = "ShaderLibrary";

inline constexpr std::string_view k_ManifestSuffix = ".ldmanifest";
inline constexpr std::string_view k_DedupeReportSuffix = ".dedupe.txt";
inline constexpr std::string_view k_ResourceReportSuffix = ".resources.txt";
inline constexpr std::string_view k_HostHeaderSuffix = ".host.hpp";
inline constexpr std::string_view k_DepfileSuffix = ".d";

/** @brief Returns true when the text is a file name stem: not empty, not `.` or `..`, and with no `/`, `\` or `:`. */
[[nodiscard]] bool IsValidOutputName(std::string_view output_name) noexcept;

/** @brief Returns the file name of one artifact. */
[[nodiscard]] std::string MakeArtifactName(std::string_view output_name, std::string_view suffix);

} // namespace lodestone

#endif // !LODESTONE_ARTIFACT_NAMES_HPP
