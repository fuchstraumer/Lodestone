#include "driver/CookerOptions.hpp"
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace lodestone
{

namespace
{

    constexpr std::string_view k_UsageText =
        "Usage: lodestone --output <header.hpp> [--O<level>] [--no-validate] [--quiet]\n"
        "                 [--cache-dir <path>] [--single-threaded] [--no-dedupe]\n"
        "                 [--target=<name>] [--verify-deterministic] [--dump-stage=<name>]\n"
        "                 [--policy-file <path>] <module.slang>...\n"
        "  --output, -o    destination header path (required)\n"
        "  --O<level>      slang optimization level: 0-3, defaults to 0\n"
        "  --target=<name> output target profile, defaults to wgsl. Names: wgsl\n"
        "  --no-validate   skip cross-checking reflection against the emitted text\n"
        "  --quiet         suppress the per-variant reflection report\n"
        "  --cache-dir     directory for precompiled slang modules\n"
        "  --single-threaded disable multi-threaded entry point codegen\n"
        "  --no-dedupe     disable content deduplication\n"
        "  --verify-deterministic cook twice and compare all artifacts\n"
        "  --dump-stage=<name> write one stage of the pipeline as JSON, beside the other artifacts.\n"
        "                  Repeat the flag for more than one stage. Names: space, variants, raw,\n"
        "                  resolved, interned, cooked, all.\n"
        "  --policy-file <path> specify the policy file to use\n";

    constexpr std::string_view k_OptimizationPrefix = "--O";
    constexpr std::string_view k_TargetPrefix = "--target=";
    constexpr std::string_view k_StageDumpPrefix = "--dump-stage=";
    constexpr std::string_view k_AllStageDumpsName = "all";

    /** The one table that decides both what `--dump-stage` accepts and what a dump artifact is
     * called. A separate spelling for either job would let the flag and the file name drift. */
    struct StageDumpName
    {
        StageDumpKind Kind;
        std::string_view Name;
    };

    constexpr std::array<StageDumpName, 6u> k_StageDumpNames{
        StageDumpName{ .Kind = StageDumpKind::Space, .Name = "space" },
        StageDumpName{ .Kind = StageDumpKind::Variants, .Name = "variants" },
        StageDumpName{ .Kind = StageDumpKind::Raw, .Name = "raw" },
        StageDumpName{ .Kind = StageDumpKind::Resolved, .Name = "resolved" },
        StageDumpName{ .Kind = StageDumpKind::Interned, .Name = "interned" },
        StageDumpName{ .Kind = StageDumpKind::Cooked, .Name = "cooked" }
    };

    CookResult<uint32_t> ParseStageDumpArgument(std::string_view name)
    {
        if (name == k_AllStageDumpsName)
        {
            return AllStageDumpBits();
        }

        const StageDumpKind kind = ParseStageDumpKind(name);
        if (kind == StageDumpKind::Invalid)
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        return StageDumpBit(kind);
    }

// ignore -Wunsafe-buffer-usage because from_chars with string_view is safe, and the warning is a little
// paranoid (as it should be)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
    CookResult<uint32_t> ParseOptimizationLevel(std::string_view level_text)
    {
        if (level_text.empty())
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        uint32_t level = 0u;
        const std::from_chars_result result =
            std::from_chars(level_text.data(), level_text.data() + level_text.size(), level);
        if (result.ec != std::errc{} || level > 3u)
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        return level;
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif

    std::filesystem::path DefaultModuleCacheDirectory()
    {
        return std::filesystem::temp_directory_path() / "LodestoneShaderCooker";
    }

    /** A flag that takes no argument. The table keeps the parser flat: one row for each switch, and
     * the loop below stays a lookup rather than a chain of comparisons. */
    struct SwitchFlag
    {
        std::string_view Name;
        void (*Apply)(CookerOptions& options) noexcept;
    };

    // moved these toggles out of lambdas, because the lambdas were ugly as sin
    void DisableDedupe(CookerOptions& options) noexcept
    {
        options.DedupeEnabled = false;
    }

    void EnableVerifyDeterminism(CookerOptions& options) noexcept
    {
        options.VerifyDeterministic = true;
    }

    void DisableValidateAgainstEmittedText(CookerOptions& options) noexcept
    {
        options.ValidateAgainstEmittedText = false;
    }

    void DisableReflectionReports(CookerOptions& options) noexcept
    {
        options.ReportReflection = false;
    }

    void DisableMultithreadedCompile(CookerOptions& options) noexcept
    {
        options.MultithreadEntryPointCodegen = false;
    }

    constexpr std::array<SwitchFlag, 5u> k_SwitchFlags{
        SwitchFlag{ .Name = "--no-dedupe", .Apply = &DisableDedupe },
        SwitchFlag{ .Name = "--verify-deterministic", .Apply = &EnableVerifyDeterminism },
        SwitchFlag{ .Name = "--no-validate", .Apply = &DisableValidateAgainstEmittedText },
        SwitchFlag{ .Name = "--quiet", .Apply = &DisableReflectionReports },
        SwitchFlag{ .Name = "--single-threaded", .Apply = &DisableMultithreadedCompile }
    };

    const SwitchFlag* FindSwitchFlag(std::string_view argument) noexcept
    {
        auto switchFlag = std::ranges::find_if(k_SwitchFlags,
                                               [&argument](const SwitchFlag& flag)
                                               {
                                                   return flag.Name == argument;
                                               });
        if (switchFlag != std::end(k_SwitchFlags))
        {
            return std::to_address(switchFlag);
        }
        return nullptr;
    }

    /** A flag written as one argument with its value attached, such as `--target=wgsl` or `--O2`.
     *
     * These three had the same shape as three separate branches of `ParseCommandLine`, and adding
     * `--target` put that function over the complexity threshold. One table instead, the way
     * `k_SwitchFlags` already does for switches, so a fourth value flag costs a row rather than a
     * branch. `Apply` gets only the tail, so no handler repeats the prefix length. */
    struct ValueFlag
    {
        std::string_view Prefix;
        CookError (*Apply)(CookerOptions& options, std::string_view value);
    };

    CookError ApplyDumpStageArgument(CookerOptions& options, std::string_view value)
    {
        const CookResult<uint32_t> bits = ParseStageDumpArgument(value);
        if (!bits)
        {
            return bits.error();
        }
        options.DumpStageMask |= bits.value();
        return CookError::Success;
    }

    CookError ApplyTargetOption(CookerOptions& options, std::string_view value)
    {
        if (FindTargetProfile(value) == nullptr) [[unlikely]]
        {
            std::string validTargetNames;
            for (const std::string_view name : GetTargetProfileNames())
            {
                validTargetNames += std::string(" ") + std::string(name);
            }
            std::println(stderr,
                         "[shader_cooker][cooker_options] No target profile named {}. Valid options: {}",
                         value,
                         validTargetNames);
            return CookError::UnknownTargetProfile;
        }
        else
        {
            options.TargetName = std::string{ value };
            return CookError::Success;
        }
    }

    CookError ApplyDesiredOptimizationLevel(CookerOptions& options, std::string_view value)
    {
        const CookResult<uint32_t> level = ParseOptimizationLevel(value);
        if (!level)
        {
            return level.error();
        }
        options.OptimizationLevel = level.value();
        return CookError::Success;
    }

    const std::array<ValueFlag, 3u> k_ValueFlags{
        ValueFlag{ .Prefix = k_StageDumpPrefix, .Apply = &ApplyDumpStageArgument },
        // Rejected here rather than in the driver. A name that reaches CookerOptions is a name
        // FindTargetProfile accepts, so no later stage has to ask again.
        ValueFlag{ .Prefix = k_TargetPrefix, .Apply = &ApplyTargetOption },
        ValueFlag{ .Prefix = k_OptimizationPrefix, .Apply = &ApplyDesiredOptimizationLevel }
    };

    const ValueFlag* FindValueFlag(std::string_view argument) noexcept
    {
        auto valueFlagIter = std::ranges::find_if(k_ValueFlags,
                                                  [&argument](const ValueFlag& flag)
                                                  {
                                                      return argument.starts_with(flag.Prefix);
                                                  });
        if (valueFlagIter != std::end(k_ValueFlags))
        {
            return std::to_address(valueFlagIter);
        }
        return nullptr;
    }

    /** A flag that consumes the next argument. It cannot join either table, because it moves the
     * loop index. */
    CookResult<std::filesystem::path> ReadPathArgument(std::span<const std::string_view> arguments,
                                                       size_t& index)
    {
        if (index + 1u >= arguments.size())
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        ++index;
        return std::filesystem::path{ arguments[index] };
    }

} // namespace

std::string_view ToString(StageDumpKind kind) noexcept
{
    for (const StageDumpName& entry : k_StageDumpNames)
    {
        if (entry.Kind == kind)
        {
            return entry.Name;
        }
    }

    return "invalid";
}

StageDumpKind ParseStageDumpKind(std::string_view name) noexcept
{
    for (const StageDumpName& entry : k_StageDumpNames)
    {
        if (entry.Name == name)
        {
            return entry.Kind;
        }
    }

    return StageDumpKind::Invalid;
}

uint32_t StageDumpBit(StageDumpKind kind) noexcept
{
    if (kind == StageDumpKind::Invalid)
    {
        return 0u;
    }

    return 1u << (static_cast<uint32_t>(kind) - 1u);
}

uint32_t AllStageDumpBits() noexcept
{
    uint32_t mask = 0u;
    for (const StageDumpName& entry : k_StageDumpNames)
    {
        mask |= StageDumpBit(entry.Kind);
    }

    return mask;
}

bool IsStageDumpRequested(const CookerOptions& options, StageDumpKind kind) noexcept
{
    return (options.DumpStageMask & StageDumpBit(kind)) != 0u;
}

CookResult<CookerOptions> ParseCommandLine(std::span<const std::string_view> arguments)
{
    CookerOptions options;
    options.ModuleCacheDirectory = DefaultModuleCacheDirectory();
    // good ol if/else config parsing, because the command line is at least simple for now
    // quick future upgrade would be a declarative lambda table, along with limits for safety
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string_view argument = arguments[i];

        if (argument == "--output" || argument == "-o")
        {
            const CookResult<std::filesystem::path> outputPath = ReadPathArgument(arguments, i);
            if (!outputPath)
            {
                return std::unexpected(outputPath.error());
            }
            options.OutputPath = outputPath.value();
        }
        else if (argument == "--cache-dir")
        {
            const CookResult<std::filesystem::path> cacheDirectory = ReadPathArgument(arguments, i);
            if (!cacheDirectory)
            {
                return std::unexpected(cacheDirectory.error());
            }
            options.ModuleCacheDirectory = cacheDirectory.value();
        }
        else if (argument == "--policy-file")
        {
            CookResult<std::filesystem::path> policyFile = ReadPathArgument(arguments, i);
            if (!policyFile)
            {
                // no policy file is fine, just continue without setting it
                options.PolicyFile = std::nullopt;
                continue;
            }

            options.PolicyFile = *policyFile;
        }
        else if (const SwitchFlag* flag = FindSwitchFlag(argument); flag != nullptr)
        {
            flag->Apply(options);
        }
        else if (const ValueFlag* valueFlag = FindValueFlag(argument); valueFlag != nullptr)
        {
            const CookError error = valueFlag->Apply(options, argument.substr(valueFlag->Prefix.size()));
            if (error != CookError::Success)
            {
                return std::unexpected(error);
            }
        }
        else if (argument.starts_with('-'))
        {
            return std::unexpected(CookError::UnknownArgument);
        }
        else
        {
            options.ModulePaths.emplace_back(argument);
        }
    }

    if (options.OutputPath.empty())
    {
        return std::unexpected(CookError::NoOutputSpecified);
    }

    if (options.ModulePaths.empty())
    {
        return std::unexpected(CookError::NoModulesSpecified);
    }

    return options;
}

std::string_view GetUsageText() noexcept
{
    return k_UsageText;
}

} // namespace lodestone
