#pragma once
#ifndef LODESTONE_MANIFEST_VERIFY_HPP
#define LODESTONE_MANIFEST_VERIFY_HPP
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include <cstddef>
#include <vector>

namespace lodestone
{

/** The manifest round trip: reads the bundle back through the client views and compares every environment with its
 * module. It runs on every cook. */
[[nodiscard]] CookError VerifyManifestRoundTrip(const CookedLibrary& library,
                                                const std::vector<std::byte>& manifest_bytes,
                                                DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_MANIFEST_VERIFY_HPP
