#pragma once
#ifndef LODESTONE_DRIVER_HPP
#define LODESTONE_DRIVER_HPP
#include "CookerErrors.hpp"
#include "CookerSteps.hpp"
#include "CookerOptions.hpp"
#include "emit/OutputSink.hpp"

/** The execution loop, separated from `main` so the cooker can also be driven in-process by a watcher
 * or by the engine itself. */
namespace lodestone
{


CookResult<CookStatistics> RunCook(const CookerOptions& options, OutputSink& sink);

} // namespace lodestone

#endif // !LODESTONE_DRIVER_HPP
