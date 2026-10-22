#pragma once
#ifndef LODESTONE_SLANG_DIAGNOSTIC_PARSER_HPP
#define LODESTONE_SLANG_DIAGNOSTIC_PARSER_HPP
#include <cstdint>
#include <string_view>

/** Reads Slang's machine-readable diagnostics (`EnableMachineReadableDiagnostics`), one tab-separated record per line:
 *     E<code>\t<severity>\t<file>\t<start line>\t<start column>\t<end line>\t<end column>\t<message> */
namespace lodestone
{

class DiagnosticSink;

/** Returns the count of failure records. Slang can report an error with a success code, so callers read this. */
int32_t ParseSlangDiagnostics(std::string_view text, std::string_view context, DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_SLANG_DIAGNOSTIC_PARSER_HPP
