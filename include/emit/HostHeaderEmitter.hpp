#pragma once
#ifndef LODESTONE_HOST_HEADER_EMITTER_HPP
#define LODESTONE_HOST_HEADER_EMITTER_HPP
#include "CookerErrors.hpp"
#include "model/CookedLibrary.hpp"
#include <string>
#include <string_view>

/** Writes the host mirrors of the shader structs as one C++ header: one struct for each distinct layout, a type
 * selected by profile and layout rule, and the binding names of each entry point. */
namespace lodestone
{

class DiagnosticSink;

inline constexpr std::string_view k_HostHeaderFileName = "ShaderLibrary.host.hpp";

/** @brief Returns true when the text is a C++ identifier, or a `::` path of them, and no segment is a keyword. */
[[nodiscard]] bool IsValidHostNamespace(std::string_view text) noexcept;

/** @brief Returns the text of the host header. `host_types_include` is the file that defines `float2` to `float4x4`,
 * `int2` to `int4`, `uint2` to `uint4`, and `half` to `half4`. Fails when two names map to one identifier, or when a
 * member has no host type. */
[[nodiscard]] CookResult<std::string> EmitHostHeader(const CookedLibrary& library,
                                                     std::string_view host_types_include,
                                                     std::string_view host_namespace,
                                                     DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_HOST_HEADER_EMITTER_HPP
