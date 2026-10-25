#include "SlangCompilerTypes.hpp"
#include "Diagnostics.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "slang.h"
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string>
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

std::vector<slang::CompilerOptionEntry> MakeCompilerOptions(uint32_t optimization_level, uint32_t debug_level)
{
    std::vector<slang::CompilerOptionEntry> options = k_CompilerOptionRows |
                                                      std::views::transform(ToOptionEntry) |
                                                      std::ranges::to<std::vector>();
    
    const CompilerOptionRow optimizationOption
    {
        .Name = slang::CompilerOptionName::Optimization,
        .Kind = slang::CompilerOptionValueKind::Int,
        .IntValue = static_cast<int32_t>(ToSlangOptimizationLevel(optimization_level))
    };

    const CompilerOptionRow debugOption
    {
        .Name = slang::CompilerOptionName::DebugInformation,
        .Kind = slang::CompilerOptionValueKind::Int,
        .IntValue = static_cast<int32_t>(debug_level)
    };

    options.emplace_back(ToOptionEntry(optimizationOption));
    options.emplace_back(ToOptionEntry(debugOption));

    return options;
}

std::vector<std::byte> BlobToBytes(slang::IBlob* blob)
{
    if (blob == nullptr || blob->getBufferSize() == 0u)
    {
        return {};
    }

    const size_t size = blob->getBufferSize();
    const std::byte* ptr = static_cast<const std::byte*>(blob->getBufferPointer());
    return std::vector<std::byte>{ ptr, ptr + size };
}

std::string BlobToString(slang::IBlob* blob)
{
    // Slang leaves the blob null when it has nothing to say
    if (blob == nullptr)
    {
        return {};
    }

    return std::string{ static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize() };
}

void ReportDiagnostics(DiagnosticSink& sink, std::string_view context, slang::IBlob* blob)
{
    if (blob == nullptr || blob->getBufferSize() == 0u)
    {
        return;
    }

    ParseSlangDiagnostics(BlobToString(blob), context, sink);
}

} // namespace lodestone
