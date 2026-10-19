#pragma once
#ifndef LODESTONE_MANIFEST_EMITTER_HPP
#define LODESTONE_MANIFEST_EMITTER_HPP
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include <cstddef>
#include <string_view>
#include <vector>

/**
 * Writes one CookedLibrary as the binary manifest bundle that `client/include/ShaderManifest.hpp` reads.
 *
 * Each cook writes one bundle. The header region contains the whole-cook tables and one header for each
 * module. After it, the file contains one extent for each (profile, module) environment, ordered by profile
 * and then by module, so that renderers can read all modules of one profile in one contiguous read.
 *
 * Every section starts on an 8-byte boundary, because the binding records, the variant keys, and the axis
 * masks contain 64-bit fields. The reader maps the bytes in place and does not copy them.
 */
namespace lodestone
{

/** The name of the bundle inside the output directory. */
inline constexpr std::string_view k_ManifestFileName = "ShaderLibrary.ldmanifest";

/** The returned bytes must start on an 8-byte boundary before a reader opens them.
 * `BundleView::Open` rejects a span that does not, because it maps 64-bit fields in place. A
 * heap allocated `std::string` satisfies this today, but the type does not promise it. Copy the bytes
 * into an aligned buffer if you ever move them somewhere the alignment is not certain.
 *
 * The emit fails when a module axis has more than 32 values, when two profiles of one module disagree on
 * its entry points or its axes, or when two variants of one environment share a key. */
[[nodiscard]] CookResult<std::vector<std::byte>> EmitShaderManifest(const CookedLibrary& library);

} // namespace lodestone

#endif // !LODESTONE_MANIFEST_EMITTER_HPP
