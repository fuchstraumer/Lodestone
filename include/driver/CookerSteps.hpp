#pragma once
#ifndef LODESTONE_COOKER_STEPS_HPP
#define LODESTONE_COOKER_STEPS_HPP
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "driver/CookerOptions.hpp"
#include "permute/PolicyDocument.hpp"
#include "target/TargetProfile.hpp"
#include "model/ShaderDataSchema.hpp"
#include "model/CookedLibrary.hpp"
#include "permute/PermutationSpace.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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

// fow now: listing steps using an enum class to spell them out, then going to build structs
// per step
struct SharedCookState
{
    std::chrono::steady_clock::time_point StartTime;
    CookStatistics Statistics;
    CookerOptions Options;
    std::unique_ptr<DiagnosticSink> Diagnostics;
    PolicyDocument Policy;
    TargetProfile Profile;
    // Each module shares this, but it's initialization
    // is a bit deferred: so, wrapped in a unique_ptr
    std::unique_ptr<PermutationSpace> Space;
};

// Writes SharedState, building the profile and policy document
// used by all later steps. Does not yet populate permutation space.
struct PrepareCookState
{
    SharedCookState SharedState;
};

// Stage 1: Declare and enumerate the module + space
struct PreparedModule
{
    RawModule Module;
};

// Evaluates the module for the permutation space, building the initial
// set of variants
struct ExpandedModule
{
    RawModule Module;
    VariantSet Variants;
};

// Compiles and resolves the module into concrete compiled shader variants
struct BuiltModule
{
    std::vector<CompiledVariant> Variants;
};

// Freezes the module, interning content and preparing it for dump to disk
struct FrozenModule
{
    CookedModule Module;
};

};

#endif // LODESTONE_COOKER_STEPS_HPP
