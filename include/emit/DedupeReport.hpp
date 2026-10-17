#pragma once
#ifndef LODESTONE_DEDUPE_REPORT_HPP
#define LODESTONE_DEDUPE_REPORT_HPP
#include "CookerErrors.hpp"
#include "model/AxisInfluence.hpp"
#include "model/CookedLibrary.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * What the dedup pass did, written as a build artifact rather than a log line that scrolls past.
 *
 * The influence matrix is the part worth reading. For each entry point and each axis, it groups the
 * variants by every other axis, then checks whether the entry point instance (code, layout, raster
 * state, and workgroup size) stays the same as that axis changes. `model/AxisInfluence` measures it. An axis that never changes the output is inert, and it multiplies the variant count for
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

struct ModulePolicyEntry;
class DiagnosticSink;

CookError EnforceModulePolicy(const CookedModule& module, const ModulePolicyEntry& policy, DiagnosticSink& diagnostics) noexcept;

std::string GenerateDedupeReport(const CookedLibrary& library);

} // namespace lodestone

#endif // !LODESTONE_DEDUPE_REPORT_HPP
