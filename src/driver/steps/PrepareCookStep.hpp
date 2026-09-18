#pragma once
#ifndef LODESTONE_COOKER_PREPARE_COOK_STEP_HPP
#define LODESTONE_COOKER_PREPARE_COOK_STEP_HPP
#include "CookerErrors.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"

namespace lodestone
{

struct PrepareCookStep
{
    CookResult<PrepareCookState> operator()(CookerOptions&& input) const;
};

};

#endif // LODESTONE_COOKER_PREPARE_COOK_STEP_HPP