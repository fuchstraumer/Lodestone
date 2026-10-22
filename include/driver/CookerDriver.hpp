#pragma once
#ifndef LODESTONE_DRIVER_HPP
#define LODESTONE_DRIVER_HPP
#include "CookerErrors.hpp"
#include "CookerSteps.hpp"
#include "CookerOptions.hpp"
#include "emit/OutputSink.hpp"

namespace lodestone
{


CookResult<CookStatistics> RunCook(CookerOptions options, OutputSink& sink);

} // namespace lodestone

#endif // !LODESTONE_DRIVER_HPP
