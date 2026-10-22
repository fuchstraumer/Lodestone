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

/** What the dedup pass did, as a build artifact. Its influence matrix shows the axes that multiply the variant count
 * for nothing. Every measurement reads content, so that both `--no-dedupe` arms agree. */
namespace lodestone
{

struct ModulePolicyEntry;
class DiagnosticSink;

CookError EnforceModulePolicy(const CookedModule& module, const ModulePolicyEntry& policy, DiagnosticSink& diagnostics) noexcept;

std::string GenerateDedupeReport(const CookedLibrary& library);

} // namespace lodestone

#endif // !LODESTONE_DEDUPE_REPORT_HPP
