#pragma once
#ifndef LODESTONE_DEPFILE_HPP
#define LODESTONE_DEPFILE_HPP
#include <span>
#include <string>
#include <string_view>

/**
 * The depfile a build system reads to know when to cook again.
 *
 * A cook reads more files than its command line names: every `import` and every `__include` of every
 * module. A build step that lists only the root modules misses an edit to any of them. Slang reports each
 * file it read, so the cook writes them all as one Makefile-form rule. Ninja, Make, and the CMake `DEPFILE`
 * option read this form.
 */
namespace lodestone
{

inline constexpr std::string_view k_DepfileName = "ShaderLibrary.d";

/** One rule: `target` depends on each prerequisite. The caller gives absolute paths with `/` separators,
 * sorted, with no duplicates, so two cooks of one input write the same bytes. A space, a `#`, and a `$` in a
 * path are escaped. */
[[nodiscard]] std::string GenerateDepfile(std::string_view target, std::span<const std::string> prerequisites);

} // namespace lodestone

#endif // !LODESTONE_DEPFILE_HPP
