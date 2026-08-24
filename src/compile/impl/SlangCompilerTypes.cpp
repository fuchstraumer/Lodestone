#include "SlangCompilerTypes.hpp"

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

} // namespace lodestone
