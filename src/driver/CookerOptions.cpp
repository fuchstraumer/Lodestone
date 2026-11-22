#include "driver/CookerOptions.hpp"
#include "CookerErrors.hpp"
#include "emit/ArtifactNames.hpp"
#include "emit/HostHeaderEmitter.hpp"
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
        "Usage: lodestone --output <directory> --target=<name> [--O<level>] [--no-validate] [--quiet]\n"
        "                 [--cache-dir <path>] [--include-dir <path>]... [--single-threaded] [--no-dedupe]\n"
        "                 [--root-from-source]\n"
        "                 [--verify-deterministic] [--dump-stage=<name>]\n"
        "                 [--policy-file <path>]... [--depfile] <module.slang>...\n"
        "  --output, -o    output directory (required)\n"
        "  --target=<name> output target profile (required). Repeat it for more than one. Names: wgsl, spirv, spirv-modern-mutable, spirv-modern-typed\n"
        "  --O<level>      slang optimization level: 0-3, defaults to 0\n"
        "  --g<level>      slang debug level: 0-3, defaults to 0\n"
        "  --fp-mode=<mode> floating point mode of Slang: fast, default, or precise. Defaults to fast. Precise\n"
        "                  stops the compiler from fusing or reordering float arithmetic\n"
        "  --output-name=<stem> file name stem of the artifacts. Defaults to ShaderLibrary. A cook removes only\n"
        "                  the artifacts of its own stem, so cooks with other stems can share a folder\n"
        "  --no-validate   skip cross-checking reflection against the emitted text\n"
        "  --quiet         suppress the per-variant reflection report\n"
        "  --cache-dir     directory for precompiled slang modules\n"
        "  --include-dir <path> directory to search for imported modules, after the folder of each module\n"
        "                  and its parent. Repeat it for more than one\n"
        "  --single-threaded disable multi-threaded entry point codegen\n"
        "  --no-dedupe     disable content deduplication\n"
        "  --root-from-source worker sessions parse the root module from source, and do not load its serialized IR\n"
        "  --verify-deterministic cook twice and compare all artifacts\n"
        "  --dump-stage=<name> write one stage of the pipeline as JSON, beside the other artifacts.\n"
        "                  Repeat the flag for more than one stage. Names: space, variants, raw,\n"
        "                  resolved, interned, cooked, all.\n"
        "  --dump-sources  write the shader sources to the dump files\n"
        "  --policy-file <path> policy file to use. Repeat it to merge more than one file. A key that two files\n"
        "                  both set is an error\n"
        "  --depfile       write <stem>.d, a depfile that names every source file the cook read\n"
        "  --host-types=<include> write <stem>.host.hpp, the C++ mirrors of the shader structs. The header\n"
        "                  includes <include>, which must define float2 to float4x4, int2 to int4, uint2 to uint4,\n"
        "                  and half to half4. The cooker does not read that file\n"
        "  --host-namespace=<name> the C++ namespace of the host header. Required with --host-types\n";

    constexpr std::string_view k_OptimizationPrefix = "--O";
    constexpr std::string_view k_DebugPrefix = "--g";
    constexpr std::string_view k_TargetPrefix = "--target=";
    constexpr std::string_view k_StageDumpPrefix = "--dump-stage=";
    constexpr std::string_view k_HostTypesPrefix = "--host-types=";
    constexpr std::string_view k_HostNamespacePrefix = "--host-namespace=";
    constexpr std::string_view k_FloatingPointModePrefix = "--fp-mode=";
    constexpr std::string_view k_OutputNamePrefix = "--output-name=";
    constexpr std::string_view k_AllStageDumpsName = "all";

    /** One table for both the flag spelling and the artifact name, so the two cannot drift. */
    struct StageDumpName
    {
        StageDumpKind Kind;
        std::string_view Name;
    };

    constexpr std::array<StageDumpName, 6u> k_StageDumpNames
    {
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

// from_chars with string_view is safe, and the warning is a little paranoid (as it should be)
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
    // used to be optimization level only: now used for debug level as well
    CookResult<uint32_t> ParseOptionLevel(std::string_view level_text)
    {
        if (level_text.empty())
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        uint32_t level = 0u;
        const std::from_chars_result result =
            std::from_chars(level_text.data(), level_text.data() + level_text.size(), level);
        // a level must be the whole value: from_chars stops at the first non-digit, so `--O2x` would read as 2
        if (result.ec != std::errc{} || result.ptr != level_text.data() + level_text.size() || level > 3u)
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        return level;
    }
#ifdef __clang__
#pragma clang diagnostic pop
#endif

    struct SwitchFlag
    {
        std::string_view Name;
        void (*Apply)(CookerOptions& options) noexcept;
    };

    void DisableDedupe(CookerOptions& options) noexcept
    {
        options.DedupeEnabled = false;
    }

    void EnableRootFromSource(CookerOptions& options) noexcept
    {
        options.RootFromSource = true;
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

    void EnableDumpSources(CookerOptions& options) noexcept
    {
        options.DumpSources = true;
    }

    void EnableWriteDepfile(CookerOptions& options) noexcept
    {
        options.WriteDepfile = true;
    }

    constexpr std::array<SwitchFlag, 8u> k_SwitchFlags
    {
        SwitchFlag{ .Name = "--no-dedupe", .Apply = &DisableDedupe },
        SwitchFlag{ .Name = "--root-from-source", .Apply = &EnableRootFromSource },
        SwitchFlag{ .Name = "--verify-deterministic", .Apply = &EnableVerifyDeterminism },
        SwitchFlag{ .Name = "--no-validate", .Apply = &DisableValidateAgainstEmittedText },
        SwitchFlag{ .Name = "--quiet", .Apply = &DisableReflectionReports },
        SwitchFlag{ .Name = "--single-threaded", .Apply = &DisableMultithreadedCompile },
        SwitchFlag{ .Name = "--dump-sources", .Apply = &EnableDumpSources },
        SwitchFlag{ .Name = "--depfile", .Apply = &EnableWriteDepfile }
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

    /** A flag with its value attached, such as `--target=wgsl`. A new value flag costs a row, not a branch. */
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
        if (!FindTargetProfile(value)) [[unlikely]]
        {
            std::string validTargetNames;
            for (const std::string_view name : GetTargetProfileNames())
            {
                validTargetNames += std::string(" ") + std::string(name);
            }
            std::println(stderr,
                         "[lodestone][cooker_options] No target profile named {}. Valid options: {}",
                         value,
                         validTargetNames);
            return CookError::UnknownTargetProfile;
        }
        else
        {
            options.TargetNames.emplace_back(value);
            return CookError::Success;
        }
    }

    CookError ApplyDesiredOptimizationLevel(CookerOptions& options, std::string_view value)
    {
        const CookResult<uint32_t> level = ParseOptionLevel(value);
        if (!level)
        {
            return level.error();
        }
        options.OptimizationLevel = level.value();
        return CookError::Success;
    }

    CookError ApplyDebugLevel(CookerOptions& options, std::string_view value)
    {
        const CookResult<uint32_t> level = ParseOptionLevel(value);
        if (!level)
        {
            return level.error();
        }
        options.DebugLevel = level.value();
        return CookError::Success;
    }

    CookError ApplyHostTypesOption(CookerOptions& options, std::string_view value)
    {
        // the include text goes inside quotes in the header, so a quote or a line break would end it early
        const bool valid = !value.empty() && value.find_first_of("\"\r\n") == std::string_view::npos;
        if (!valid)
        {
            return CookError::MalformedArgument;
        }

        options.HostTypesInclude = std::string{ value };
        return CookError::Success;
    }

    CookError ApplyHostNamespaceOption(CookerOptions& options, std::string_view value)
    {
        if (!IsValidHostNamespace(value))
        {
            return CookError::HostNamespaceInvalid;
        }

        options.HostNamespace = std::string{ value };
        return CookError::Success;
    }

    struct FloatingPointModeName
    {
        manifest::FloatingPointMode Mode;
        std::string_view Name;
    };

    constexpr std::array<FloatingPointModeName, 3u> k_FloatingPointModeNames
    {
        FloatingPointModeName{ .Mode = manifest::FloatingPointMode::Fast, .Name = "fast" },
        FloatingPointModeName{ .Mode = manifest::FloatingPointMode::Default, .Name = "default" },
        FloatingPointModeName{ .Mode = manifest::FloatingPointMode::Precise, .Name = "precise" }
    };

    CookError ApplyFloatingPointMode(CookerOptions& options, std::string_view value)
    {
        const auto modeName = std::ranges::find(k_FloatingPointModeNames, value, &FloatingPointModeName::Name);
        if (modeName == std::end(k_FloatingPointModeNames))
        {
            std::println(stderr,
                         "[lodestone][cooker_options] No floating point mode named '{}'. Valid options: fast default precise",
                         value);
            return CookError::FloatingPointModeInvalid;
        }

        options.FloatingPointMode = modeName->Mode;
        return CookError::Success;
    }

    CookError ApplyOutputName(CookerOptions& options, std::string_view value)
    {
        if (!IsValidOutputName(value))
        {
            std::println(stderr,
                         "[lodestone][cooker_options] The output name '{}' is not valid. It must not be empty or '.' or '..', "
                         "and must not contain '/', '\\', or ':'",
                         value);
            return CookError::OutputNameInvalid;
        }

        options.OutputName = std::string{ value };
        return CookError::Success;
    }

    const std::array<ValueFlag, 8u> k_ValueFlags
    {
        ValueFlag{ .Prefix = k_StageDumpPrefix, .Apply = &ApplyDumpStageArgument },
        // rejected here, so every name in CookerOptions is one FindTargetProfile accepts
        ValueFlag{ .Prefix = k_TargetPrefix, .Apply = &ApplyTargetOption },
        ValueFlag{ .Prefix = k_OptimizationPrefix, .Apply = &ApplyDesiredOptimizationLevel },
        ValueFlag{ .Prefix = k_DebugPrefix, .Apply = &ApplyDebugLevel },
        ValueFlag{ .Prefix = k_HostTypesPrefix, .Apply = &ApplyHostTypesOption },
        ValueFlag{ .Prefix = k_HostNamespacePrefix, .Apply = &ApplyHostNamespaceOption },
        ValueFlag{ .Prefix = k_FloatingPointModePrefix, .Apply = &ApplyFloatingPointMode },
        ValueFlag{ .Prefix = k_OutputNamePrefix, .Apply = &ApplyOutputName }
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

    enum class PathMustExist : uint8_t
    {
        No = 0,
        Yes = 1
    };

    /** Not in a table, because it moves the loop index. Parsing touches no file: the output sink creates the output
     * folder, and `PrepareCookStep` creates the module cache folder. */
    CookResult<std::filesystem::path> ReadPathArgument(std::span<const std::string_view> arguments,
                                                       size_t& index,
                                                       PathMustExist must_exist)
    {
        if (index + 1u >= arguments.size())
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        ++index;

        std::filesystem::path result{ arguments[index] };
        if (must_exist == PathMustExist::Yes && !std::filesystem::exists(result))
        {
            return std::unexpected(CookError::DirectoryDoesNotExist);
        }
        return result;
    }

    CookResult<std::filesystem::path> ReadFileArgument(std::span<const std::string_view> arguments, size_t& index)
    {
        if (index + 1u >= arguments.size())
        {
            return std::unexpected(CookError::MalformedArgument);
        }

        ++index;
        std::filesystem::path result{ arguments[index] };

        if (result.has_filename() && !std::filesystem::exists(result))
        {
            return std::unexpected(CookError::FileNotFound);
        }
        return result;
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

std::filesystem::path DefaultModuleCacheDirectory() noexcept
{
    return std::filesystem::temp_directory_path() / "LodestoneShaderCooker";
}

CookResult<CookerOptions> ParseCommandLine(std::span<const std::string_view> arguments)
{
    CookerOptions options;
    options.ModuleCacheDirectory = DefaultModuleCacheDirectory();
    for (size_t i = 0; i < arguments.size(); ++i)
    {
        const std::string_view argument = arguments[i];

        if (argument == "--output" || argument == "-o")
        {
            const CookResult<std::filesystem::path> outputPath = ReadPathArgument(arguments, i, PathMustExist::No);
            if (!outputPath)
            {
                return std::unexpected(outputPath.error());
            }
            options.OutputPath = outputPath.value();
        }
        else if (argument == "--cache-dir")
        {
            const CookResult<std::filesystem::path> cacheDirectory = ReadPathArgument(arguments, i, PathMustExist::No);
            if (!cacheDirectory)
            {
                return std::unexpected(cacheDirectory.error());
            }
            options.ModuleCacheDirectory = cacheDirectory.value();
        }
        else if (argument == "--include-dir" || argument == "-I")
        {
            CookResult<std::filesystem::path> includeDirectory = ReadPathArgument(arguments, i, PathMustExist::Yes);
            if (!includeDirectory)
            {
                return std::unexpected(includeDirectory.error());
            }
            options.IncludeDirectories.push_back(std::move(includeDirectory.value()));
        }
        else if (argument == "--policy-file")
        {
            // a policy file that the author named and that does not exist is an error. Ignoring it would cook every
            // variant with no budget, and nothing would say why
            CookResult<std::filesystem::path> policyFile = ReadFileArgument(arguments, i);
            if (!policyFile)
            {
                return std::unexpected(policyFile.error());
            }

            options.PolicyFiles.push_back(std::move(*policyFile));
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

    if (options.TargetNames.empty())
    {
        return std::unexpected(CookError::NoTargetSpecified);
    }

    // each flag is useless without the other: the header needs a namespace, and a namespace needs a header
    if (options.HostTypesInclude.empty() != options.HostNamespace.empty())
    {
        return std::unexpected(CookError::HostNamespaceMissing);
    }

    return options;
}

std::string_view GetUsageText() noexcept
{
    return k_UsageText;
}

} // namespace lodestone
