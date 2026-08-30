#pragma once
#ifndef LODESTONE_DEDUPE_REPORT_HPP
#define LODESTONE_DEDUPE_REPORT_HPP
#include "model/CookedLibrary.hpp"
#include <string>
#include <vector>

/**
 * What the dedup pass did, written as a build artifact rather than a log line that scrolls past.
 *
 * The influence matrix is the part worth reading. For each entry point and each axis, it groups the
 * variants by every other axis, then checks whether the emitted text stays the same as that axis
 * changes. An axis that never changes the output is inert, and it multiplies the variant count for
 * nothing. The result turns combinatorial growth into a number you read before the space gets large,
 * instead of a build time you find afterwards.
 *
 * Every measurement here reads the content, never an interner index. `--no-dedupe` is the A/B control
 * arm, and it gives each artifact its own index, so a measurement that reads an index reports a
 * different answer in each arm. The two arms must agree, because that is the only thing the control
 * arm is for.
 */
namespace lodestone
{

enum class AxisInfluence : uint8_t
{
    Invalid = 0,
    /** The axis never changes this entry point's output. */
    Inert,
    /** The axis changes this entry point's output. */
    Active,
    /** Too few variants to decide. One axis value cannot show a difference. */
    Undetermined,
};

struct EntryPointInfluence
{
    std::string EntryPointName;
    /** Runs parallel to the module's permutation space. */
    std::vector<AxisInfluence> Axes;
};

struct ModuleInfluence
{
    std::string ModuleName;
    std::vector<EntryPointInfluence> EntryPoints;
};

ModuleInfluence ComputeAxisInfluence(const CookedModule& module);

/** True when every variant of the module produced the same binding layout. The graph can then hold
 * one layout for each module instead of one for each variant. */
bool AllVariantsShareOneLayout(const CookedModule& module);

/** Compares the measured influence against what the module declared, and checks the variant budget.
 * A mismatch fails the cook and names the entry point and the axis. */
CookError EnforceModulePolicy(const CookedModule& module, const ModuleInfluence& influence);

std::string GenerateDedupeReport(const CookedLibrary& library);

std::string_view ToString(AxisInfluence influence) noexcept;

} // namespace lodestone

#endif // !LODESTONE_DEDUPE_REPORT_HPP
