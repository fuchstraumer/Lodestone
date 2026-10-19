#pragma once
#ifndef LODESTONE_MANIFEST_VERIFY_HPP
#define LODESTONE_MANIFEST_VERIFY_HPP
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include <cstddef>
#include <vector>

/**
 * The manifest round trip: the validator that reads a written bundle back through the client views, the way a
 * consumer reads it, and compares it against the `CookedLibrary` it came from. `EmitShaderManifest` writes the
 * bundle, and this file checks it, so the writer and its second opinion live side by side in `emit/`.
 */
namespace lodestone
{

/** Reads the bundle back and compares every environment against the module it came from. For each entry
 * point of each variant it checks the source bytes, the workgroup size, each binding field, and the raster
 * state. For each variant it also decodes the key through the module axes and compares the result with
 * the canonical assignment, and compares the axis-active mask with the active assignment.
 *
 * This runs on every cook. The check is not optional. */
[[nodiscard]] CookError VerifyManifestRoundTrip(const CookedLibrary& library, const std::vector<std::byte>& manifest_bytes);

} // namespace lodestone

#endif // !LODESTONE_MANIFEST_VERIFY_HPP
