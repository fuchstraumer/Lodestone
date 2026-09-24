#include "driver/CookerDriver.hpp"
#include "CookerErrors.hpp"
#include "driver/CookerOptions.hpp"
#include "emit/OutputSink.hpp"

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <print>
#include <string_view>
#include <vector>

int main(int argc, char** argv)
{
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<size_t>(argc));
    for (int i = 1; i < argc; ++i)
    {
        arguments.emplace_back(argv[i]);
    }

    lodestone::CookResult<lodestone::CookerOptions> optionsResult = lodestone::ParseCommandLine(arguments);
    if (!optionsResult)
    {
        std::println(stderr,
                     "[lodestone] {}\n{}",
                     lodestone::ToString(optionsResult.error()),
                     lodestone::GetUsageText());
        return 1;
    }

    lodestone::CookerOptions options{ std::move(*optionsResult) };
    lodestone::FileOutputSink sink{ options.OutputPath };
    const lodestone::CookResult<lodestone::CookStatistics> statistics =
        lodestone::RunCook(std::move(options), sink);

    if (!statistics)
    {
        std::println(stderr, "[lodestone] cook failed: {}", lodestone::ToString(statistics.error()));
        return 1;
    }

    std::println(stderr,
                 "[lodestone] cooked {} modules, {} variants, {} entrypoints, {} KiB of shader source code in {:.1f}ms "
                 "-> {}",
                 statistics.value().ModulesCooked,
                 statistics.value().VariantsCompiled,
                 statistics.value().EntryPointsCompiled,
                 statistics.value().TotalSourceBytes / 1024u,
                 statistics.value().ElapsedMilliseconds,
                 sink.Describe());

    return 0;
}
