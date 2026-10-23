#pragma once
#ifndef LODESTONE_DRIVER_PREPARE_COOK_STEP_HPP
#define LODESTONE_DRIVER_PREPARE_COOK_STEP_HPP
#include "CookerErrors.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"

namespace lodestone
{


class DiagnosticSink;

struct PrepareCookStep
{
    CookResult<PreparedCook> operator()(CookerOptions&& input, DiagnosticSink& diagnostics) const;
};

};

#endif // LODESTONE_DRIVER_PREPARE_COOK_STEP_HPP