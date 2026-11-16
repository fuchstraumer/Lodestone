#pragma once
#ifndef LODESTONE_COOKER_STEPS_HPP
#define LODESTONE_COOKER_STEPS_HPP
#include "Diagnostics.hpp"
#include "driver/CookerOptions.hpp"
#include "permute/PolicyDocument.hpp"
#include "target/TargetProfile.hpp"
#include "model/ShaderDataSchema.hpp"
#include "model/CookedLibrary.hpp"
#include "permute/PermutationSpace.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <unordered_map>
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
    CookStatistics& operator+=(const CookStatistics& other);
};

struct SharedCookState
{
    CookerOptions Options;
    // the caller of RunCook owns the sink, so that an embedding host receives every record
    DiagnosticSink* Diagnostics{ nullptr };
    PolicyDocument Policy;
    std::vector<std::string> AllModuleNames;
    // views into the Options strings
    std::unordered_map<std::string_view, TargetProfile> TargetProfiles;
};

struct PreparedCook
{
    SharedCookState SharedState;
};

// todo-ship: key this on the target too, which changes the binding model
struct PreparedCompiler
{
    std::unique_ptr<class SlangCompiler> Compiler;
};

// todo-ship: build the space once for the whole cook, not per module
struct PreparedPermutationSpace
{
    PermutationSpace Space;
    std::optional<std::string> SpaceDump;
    VariantSet Variants;
    std::optional<std::string> VariantDump;
};

// todo-ship: thread this step, at about 1.25-1.5x the hardware threads
struct BuiltModule
{
    InternedModule Module;
    std::vector<CompiledVariant> CompiledVariants;
    std::optional<std::string> RawModuleDump;
    std::optional<std::string> ResolvedModuleDump;
    std::optional<std::string> InternedModuleDump;
    CookStatistics Statistics;
};

struct FinalizedModule
{
    CookedModule Module;
    std::optional<std::string> Dump;
};

struct PhaseTimings
{
    double PrepareCookMs{ 0.0 };
    double Stage1_DeclareMs{ 0.0 };
    double Stage2_EnumerateMs{ 0.0 };
    double Stage3_CompileMs{ 0.0 };
    double Stage4_ResolveMs{ 0.0 };
    double Validate_EmittedCodeMs{ 0.0 };
    double Stage6_InternMs{ 0.0 };
    double Stage7_FreezeMs{ 0.0 };
    double Validate_ModuleRoundTripsMs{ 0.0 };
    double Validate_CrossProfileMs{ 0.0 };
    double Stage8_EmitMs{ 0.0 };
    uint32_t CooksRun{ 0u };

    PhaseTimings& operator+=(const PhaseTimings& other) noexcept;
};

void AddPhaseTimings(const PhaseTimings& timings) noexcept;
PhaseTimings GetProcessPhaseTimings() noexcept;
void WriteTestTimingCsvRow(std::string_view test_name, double total_test_ms);

};

#endif // LODESTONE_COOKER_STEPS_HPP
