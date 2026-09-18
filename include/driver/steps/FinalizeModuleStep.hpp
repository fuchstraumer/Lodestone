#pragma once
#ifndef LODESTONE_DRIVER_FINALIZE_MODULE_STEP_HPP
#define LODESTONE_DRIVER_FINALIZE_MODULE_STEP_HPP
#include "driver/CookerSteps.hpp"
#include "CookerErrors.hpp"
#include "model/CookedLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include <span>

namespace lodestone
{

struct InternedModule;

struct FinalizeModuleStep
{
    CookResult<FinalizedModule> operator()(const SharedCookState& shared_state,
                                           InternedModule&& interned_module,
                                           std::span<const CompiledVariant> module_variants) const;
};

}

#endif // LODESTONE_DRIVER_FINALIZE_MODULE_STEP_HPP
