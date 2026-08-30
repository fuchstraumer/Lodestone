#pragma once
#ifndef LODESTONE_DRIVER_HPP
#define LODESTONE_DRIVER_HPP
#include "CookerErrors.hpp"
#include "CookerOptions.hpp"
#include "emit/OutputSink.hpp"
#include <cstdint>

/** The execution loop, separated from `main` so the cooker can also be driven in-process by a watcher
 * or by the engine itself. */
namespace lodestone
{

struct CookStatistics
{
    uint32_t ModulesCooked{ 0u };
    uint32_t VariantsCompiled{ 0u };
    uint32_t EntryPointsCompiled{ 0u };
    uint32_t ReflectionMismatches{ 0u };
    double ElapsedMilliseconds{ 0.0 };
    size_t TotalSourceBytes{ 0u };
};

CookResult<CookStatistics> RunCook(const CookerOptions& options, OutputSink& sink);

} // namespace lodestone

#endif // !LODESTONE_DRIVER_HPP
