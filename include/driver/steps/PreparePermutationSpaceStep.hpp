#pragma once
#ifndef LODESTONE_DRIVER_PREPARE_PERMUTATION_SPACE_HPP
#define LODESTONE_DRIVER_PREPARE_PERMUTATION_SPACE_HPP
#include "driver/CookerSteps.hpp"
#include "CookerErrors.hpp"
#include "compile/SymbolTable.hpp"
#include "compile/RawLibrary.hpp"
#include "permute/PolicyDocument.hpp"
#include <span>
#include <string_view>
#include <vector>

namespace lodestone
{

struct PreparePermutationSpaceStep
{
    CookResult<PreparedPermutationSpace> operator()(const SharedCookState& shared_state,
                                                    std::string_view module_name,
                                                    std::string_view target_name,
                                                    const SymbolTable& symbol_table,
                                                    std::vector<RawAxisDeclaration> raw_axes) const;
};

}

#endif // LODESTONE_DRIVER_PREPARE_PERMUTATION_SPACE_HPP