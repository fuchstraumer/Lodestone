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

/** One point between two stages that a cook can write out as JSON.
 *
 * A dump is a diagnostic, not a shipped format. It has no version field and nothing reads it back.
 * Every check that uses one is a byte comparison: against a golden file, or against the second cook
 * that `--verify-deterministic` runs.*/
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

/** Maps one `--dump-stage` name onto a kind. Returns `Invalid` for a name the cooker does not know. */
StageDumpKind ParseStageDumpKind(std::string_view name) noexcept;

uint32_t StageDumpBit(StageDumpKind kind) noexcept;

/** Every kind at once, which is what `--dump-stage=all` sets. */
uint32_t AllStageDumpBits() noexcept;

struct CookerOptions
{
    std::filesystem::path OutputPath;
    std::filesystem::path ModuleCacheDirectory;
    std::vector<std::filesystem::path> ModulePaths;
    uint32_t OptimizationLevel{ 0u };
    /** Which `TargetProfile` this cook emits for. `--target` sets it */
    std::string TargetName{ "wgsl" };
    /** Runs the target's validator, making sure that emitted data matches target binding schema and access
     * model */
    bool ValidateAgainstEmittedText{ true };
    bool ReportReflection{ false };
    bool MultithreadEntryPointCodegen{ true };
    /**Turns off content dedup. Output stays correct, and every artifact takes its own index */
    bool DedupeEnabled{ true };
    /** Cooks twice into memory and compares. Catches an unordered container's iteration order when
     * it reaches the emitted output. */
    bool VerifyDeterministic{ false };
    /** One bit for each `StageDumpKind` the cook must write. `--dump-stage` sets them. */
    uint32_t DumpStageMask{ 0u };
    std::optional<std::filesystem::path> PolicyFile;
};

bool IsStageDumpRequested(const CookerOptions& options, StageDumpKind kind) noexcept;

CookResult<CookerOptions> ParseCommandLine(std::span<const std::string_view> arguments);
std::string_view GetUsageText() noexcept;

} // namespace lodestone

#endif // !LODESTONE_OPTIONS_HPP
