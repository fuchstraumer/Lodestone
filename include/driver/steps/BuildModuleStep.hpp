#pragma once
#ifndef LODESTONE_DRIVER_BUILD_MODULE_STEP_HPP
#define LODESTONE_DRIVER_BUILD_MODULE_STEP_HPP
#include "driver/CookerSteps.hpp"
#include "CookerErrors.hpp"
#include "permute/PermutationSpace.hpp"
#include <memory>
#include <string_view>

namespace lodestone
{

struct BuildModuleStep
{
    CookResult<BuiltModule> operator()(const SharedCookState& shared_state,
                                       const std::string_view& module_name,
                                       const std::string_view& target_name,
                                       std::unique_ptr<class SlangCompiler> compiler,
                                       const PermutationSpace& space,
                                       const VariantSet& variants) const;
};
}

#endif // !LODESTONE_DRIVER_BUILD_MODULE_STEP_HPP
