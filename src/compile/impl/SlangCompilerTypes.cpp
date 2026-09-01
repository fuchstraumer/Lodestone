#include "SlangCompilerTypes.hpp"
#include "Diagnostics.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "slang.h"
#include <cstdint>
#include <ranges>
#include <string_view>
#include <vector>

namespace lodestone
{

slang::CompilerOptionEntry ToOptionEntry(const CompilerOptionRow& row) noexcept
{
    slang::CompilerOptionEntry entry{};
    entry.name = row.Name;
    entry.value.kind = row.Kind;
    entry.value.intValue0 = row.IntValue;
    entry.value.stringValue0 = row.StringValue;
    return entry;
}

std::vector<slang::CompilerOptionEntry> MakeCompilerOptions(uint32_t optimization_level)
{
    std::vector<slang::CompilerOptionEntry> options =
        k_CompilerOptionRows | std::views::transform(ToOptionEntry) | std::ranges::to<std::vector>();

    options.emplace_back(ToOptionEntry(
        CompilerOptionRow{ .Name = slang::CompilerOptionName::Optimization,
                           .Kind = slang::CompilerOptionValueKind::Int,
                           .IntValue = static_cast<int32_t>(ToSlangOptimizationLevel(optimization_level)) }));

    return options;
}

std::string BlobToString(slang::IBlob* blob)
{
    // Slang leaves the blob null when a call has nothing to say, and a clean codegen is the common
    // case. `GenerateOneEntryPoint` reads its diagnostic blob without asking, so the check is here
    // and not only in `ReportDiagnostics`.
    if (blob == nullptr)
    {
        return {};
    }

    return std::string{ static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize() };
}

void ReportDiagnostics(DiagnosticSink& sink, std::string_view context, slang::IBlob* blob)
{
    // this check is the point of this function: only report diagnostics if blob has content,
    // but otherwise make it trivial to call inline in case we want to report diagnositcs from
    // slang calls
    if (blob == nullptr || blob->getBufferSize() == 0u)
    {
        return;
    }

    ParseSlangDiagnostics(BlobToString(blob), context, sink);
}

} // namespace lodestone
