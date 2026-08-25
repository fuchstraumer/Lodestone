#pragma once
#ifndef LODESTONE_MANIFEST_EMITTER_HPP
#define LODESTONE_MANIFEST_EMITTER_HPP
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include <string>

/**
 * Writes one CookedModule as the binary manifest that `include/shader/ShaderManifest.hpp` reads.
 *
 * The manifest and the generated C++ carry the same tables from the same frozen model. The C++ form
 * compiles into the program. The manifest form arrives as bytes, so a live cooker can replace it while
 * the program runs.
 *
 * Every section starts on an 8-byte boundary, because the binding records and the axis values hold
 * 64-bit fields. The reader maps the bytes in place and does not copy them.
 */
namespace lodestone
{

/** The returned bytes must start on an 8-byte boundary before a reader opens them.
 * `ShaderManifestView::Open` rejects a span that does not, because it maps 64-bit fields in place. A
 * heap allocated `std::string` satisfies this today, but the type does not promise it. Copy the bytes
 * into an aligned buffer if you ever move them somewhere the alignment is not certain. */
[[nodiscard]] std::string EmitShaderManifest(const CookedModule& module);

[[nodiscard]] std::string MakeManifestFileName(std::string_view module_name);

/** Reads the manifest back and compares every entry point of every variant against the module it came
 * from. It checks the source bytes, the workgroup size, and each binding field.
 *
 * This runs on every cook. A manifest that says something different from the generated C++ is the one
 * failure this format could hide, so the check is not optional. */
[[nodiscard]] CookError VerifyManifestRoundTrip(const CookedModule& module, const std::string& manifest_bytes);

} // namespace lodestone

#endif // !LODESTONE_MANIFEST_EMITTER_HPP
