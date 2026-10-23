#pragma once
#ifndef LODESTONE_DRIVER_HPP
#define LODESTONE_DRIVER_HPP
#include "CookerErrors.hpp"
#include "CookerSteps.hpp"
#include "CookerOptions.hpp"
#include "emit/OutputSink.hpp"

namespace lodestone
{

class DiagnosticSink;

/** @brief Runs one cook, and reports every diagnostic to stderr. */
CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink);

/** @brief Runs one cook, and reports every diagnostic to `diagnostics`. An embedding host or a test passes its own sink
 * here to read the records instead of the console text. */
CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink, DiagnosticSink& diagnostics);

} // namespace lodestone

#endif // !LODESTONE_DRIVER_HPP
