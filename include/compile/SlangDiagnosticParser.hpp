#pragma once
#ifndef LODESTONE_SLANG_DIAGNOSTIC_PARSER_HPP
#define LODESTONE_SLANG_DIAGNOSTIC_PARSER_HPP
#include <cstdint>
#include <string_view>

/** Turns Slang's machine-readable diagnostic text into records.
 *
 * Slang renders a diagnostic two ways. The one we use is not the default - it's one record per line,
 * tab separated, turned on with `EnableMachineReadableDiagnostics`. This parser reads that form,
 * and SlangCompiler.cpp turns it on for us.
 * 
 * Slang documents its field order in `slang-rich-diagnostics-render.cpp`:
 *
 *     E<code>\t<severity>\t<file>\t<start line>\t<start column>\t<end line>\t<end column>\t<message>
 **/
namespace lodestone
{

class DiagnosticSink;

/** Reports one record for each primary diagnostic in `text`. `context` names the call that produced the text,
  * such as `loadModule`, and reaches every record.
  *
  * Returns the count of reported records whose severity is a failure. Slang can report an error and still
  * return a success code, so a caller that must fail on an error reads this count, not the return code alone.
 **/
int32_t ParseSlangDiagnostics(std::string_view text, std::string_view context, DiagnosticSink& sink);

} // namespace lodestone

#endif // !LODESTONE_SLANG_DIAGNOSTIC_PARSER_HPP
