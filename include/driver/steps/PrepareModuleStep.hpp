#pragma once
#ifndef LODESTONE_DRIVER_PREPARE_MODULE_STEP_HPP
#define LODESTONE_DRIVER_PREPARE_MODULE_STEP_HPP
#include "driver/CookerSteps.hpp"
#include "CookerErrors.hpp"
#include <filesystem>
#include <string_view>

namespace lodestone
{

struct PrepareModuleStep
{
    CookResult<PreparedCompiler> operator()(const SharedCookState& state,
                                            std::filesystem::path module_path,
                                            std::string_view target_name) const;
};

}

#endif // !LODESTONE_DRIVER_PREPARE_MODULE_STEP_HPP
