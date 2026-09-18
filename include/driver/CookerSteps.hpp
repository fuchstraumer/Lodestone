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
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
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
    std::filesystem::path CacheDirectory;
    PolicyDocument Policy;
    std::vector<std::string_view> AllModuleNames;
    std::unique_ptr<class OutputSink> OutputSink;
    // Resolve policies per target upfront, read later
    // string_views are views into Options vector of strings, should be fine
    std::unordered_map<std::string_view, TargetCookPolicy> TargetPolicies;
};

// Writes SharedState, building the profile and policy document
// used by all later steps. Does not yet populate permutation space.
struct PreparedCook
{
    SharedCookState SharedState;
};

// Step 1: Bootstrap compiler *per module*, then coalesce back to build the 
// permutation space for the whole cook. After that, we can proceed to 
// build the RawModule per-module
// todo-ship: this also needs to be keyed/varied on target, since that can
// change the binding model and generally affects results greatly
struct PreparedCompiler
{
    std::unique_ptr<class SlangCompiler> Compiler;
};

// Step 2: Build the permutation space, using all of the modules (and thus
// compiler instances) from the previous step. This also generates
// the variant set for a single module.
// todo-ship: Currently it's still per-module, but that doesn't break
// anything behavior-wise. It's just a missing improvement.
struct PreparedPermutationSpace
{
    PermutationSpace Space;
    std::optional<std::string> SpaceDump;
    VariantSet Variants;
    std::optional<std::string> VariantDump;
};

// Stage 3: Combine the built space and the compiler to build the prepared
// module. Previous seam used to be on RawModule, but this builds that 
// internally and writes to RawModuleDump as the only artifact of that work
// which exits this step
// todo-ship: Thread this step, and use that to scale the shader compiler
// to the right amount of threads to be about 1.25-1.5x hardware thread counts
// any further will just choke out the OS, but mild oversubscription is fine
struct BuiltModule
{
    InternedModule Module;
    std::vector<CompiledVariant> CompiledVariants;
    std::optional<std::string> RawModuleDump;
    std::optional<std::string> ResolvedModuleDump;
};

struct FinalizedModule
{
    CookedModule Module;
    std::optional<std::string> Dump;
};

};

#endif // LODESTONE_COOKER_STEPS_HPP
