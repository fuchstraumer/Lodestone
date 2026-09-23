#pragma once
#ifndef LODESTONE_MANIFEST_EMITTER_HPP
#define LODESTONE_MANIFEST_EMITTER_HPP
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include <string>
#include <string_view>

/**
 * Writes one CookedLibrary as the binary manifest bundle that `client/include/ShaderManifest.hpp` reads.
 *
 * One cook writes one bundle. The header region holds the whole-cook tables and one header for each
 * module. The file then holds one extent for each (profile, module) environment, ordered by profile and
 * then by module, so a renderer reads the modules of one profile in one contiguous read.
 *
 * Every section starts on an 8-byte boundary, because the binding records, the variant keys, and the axis
 * masks hold 64-bit fields. The reader maps the bytes in place and does not copy them.
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
 * The emit fails when a module axis holds more than 32 values, when two profiles of one module disagree on
 * its entry points or its axes, or when two variants of one environment share a key. */
[[nodiscard]] CookResult<std::string> EmitShaderManifest(const CookedLibrary& library);

/** Reads the bundle back and compares every environment against the module it came from. For each entry
 * point of each variant it checks the source bytes, the workgroup size, each binding field, and the raster
 * state. For each variant it also decodes the key through the module axes and compares the result with
 * the canonical assignment, and compares the axis-active mask with the active assignment.
 *
 * This runs on every cook. The check is not optional. */
[[nodiscard]] CookError VerifyManifestRoundTrip(const CookedLibrary& library, const std::string& manifest_bytes);

} // namespace lodestone

#endif // !LODESTONE_MANIFEST_EMITTER_HPP
