#pragma once
#ifndef LODESTONE_OPTIONS_HPP
#define LODESTONE_OPTIONS_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{

/** A point between two stages that a cook can dump. Dumps are diagnostics with no version: only byte comparisons
 * read them. */
enum class StageDumpKind : uint8_t
{
    Invalid = 0,
    Space,
    Variants,
    Raw,
    Resolved,
    Interned,
    Cooked,
};

std::string_view ToString(StageDumpKind kind) noexcept;

StageDumpKind ParseStageDumpKind(std::string_view name) noexcept;

uint32_t StageDumpBit(StageDumpKind kind) noexcept;

uint32_t AllStageDumpBits() noexcept;

struct CookerOptions
{
    std::filesystem::path OutputPath;
    std::filesystem::path ModuleCacheDirectory;
    std::vector<std::filesystem::path> IncludeDirectories;
    std::vector<std::filesystem::path> ModulePaths;
    uint32_t OptimizationLevel{ 0u };
    std::vector<std::string> TargetNames;
    bool ValidateAgainstEmittedText{ true };
    bool ReportReflection{ false };
    bool MultithreadEntryPointCodegen{ true };
    /** The control arm: every artifact takes its own index. */
    bool DedupeEnabled{ true };
    /** Cooks twice into memory and compares, to catch unordered containers reaching the output. */
    bool VerifyDeterministic{ false };
    uint32_t DumpStageMask{ 0u };
    std::optional<std::filesystem::path> PolicyFile;
    bool DumpSources{ false };
    bool WriteDepfile{ false };
};

bool IsStageDumpRequested(const CookerOptions& options, StageDumpKind kind) noexcept;
std::filesystem::path DefaultModuleCacheDirectory() noexcept;
CookResult<CookerOptions> ParseCommandLine(std::span<const std::string_view> arguments);
std::string_view GetUsageText() noexcept;

} // namespace lodestone

#endif // !LODESTONE_OPTIONS_HPP
