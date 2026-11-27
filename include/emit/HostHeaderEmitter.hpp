#pragma once
#ifndef LODESTONE_HOST_HEADER_EMITTER_HPP
#define LODESTONE_HOST_HEADER_EMITTER_HPP
#include "CookerErrors.hpp"
#include "model/CookedLibrary.hpp"
#include <string>
#include <string_view>

/** Writes the host mirrors of the shader structs as two C++ files. The header is for people: the binding names, the type
 * aliases, and a comment block for each entry point. The inline file is for the compiler: one struct for each distinct
 * layout and the selection of a type by profile and layout rule. */
namespace lodestone
{

class DiagnosticSink;

/** @brief The text of the two files. The header includes the inline file at its end. */
struct HostHeaderFiles
{
    std::string Header;
    std::string Inline;
};

/** @brief Returns true when the text is a C++ identifier, or a `::` path of them, and no segment is a keyword. */
[[nodiscard]] bool IsValidHostNamespace(std::string_view text) noexcept;

/** @brief Returns the text of the host header and of the inline file. `host_types_include` is the file that defines
 * `float2` to `float4x4`, `int2` to `int4`, `uint2` to `uint4`, and `half` to `half4`. `inline_name` is the file name that
 * the header includes. Fails when two names map to one identifier, or when a member has no host type. */
[[nodiscard]] CookResult<HostHeaderFiles> EmitHostHeader(const CookedLibrary& library,
                                                         std::string_view host_types_include,
                                                         std::string_view host_namespace,
                                                         std::string_view inline_name,
                                                         DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_HOST_HEADER_EMITTER_HPP
