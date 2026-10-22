#pragma once
#ifndef LODESTONE_MANIFEST_EMITTER_HPP
#define LODESTONE_MANIFEST_EMITTER_HPP
#include "model/CookedLibrary.hpp"
#include "CookerErrors.hpp"
#include <cstddef>
#include <string_view>
#include <vector>

/** Writes a CookedLibrary as the binary bundle that `client/include/ShaderManifest.hpp` reads. */
namespace lodestone
{

inline constexpr std::string_view k_ManifestFileName = "ShaderLibrary.ldmanifest";

/** The reader maps 64-bit fields in place, so keep the bytes 8-byte aligned if you move them. */
[[nodiscard]] CookResult<std::vector<std::byte>> EmitShaderManifest(const CookedLibrary& library);

} // namespace lodestone

#endif // !LODESTONE_MANIFEST_EMITTER_HPP
