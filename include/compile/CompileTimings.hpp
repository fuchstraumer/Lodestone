#pragma once
#ifndef LODESTONE_COMPILE_TIMINGS_HPP
#define LODESTONE_COMPILE_TIMINGS_HPP
#include <cstdint>

namespace lodestone
{

/** @brief Measurements of the Slang setup work. A `*Ms` sum over worker threads is CPU time across threads,
 * not wall time. Each count is the number of times the work ran. */
struct CompileTimings
{
    double BootstrapGlobalSessionMs{ 0.0 };
    uint32_t BootstrapGlobalSessionCount{ 0u };
    double WorkerGlobalSessionMs{ 0.0 };
    uint32_t WorkerGlobalSessionCount{ 0u };
    /** Worker session priming and the shim, format, and heap link modules. Excludes the global session. */
    double WorkerSetupMs{ 0.0 };
    uint32_t WorkerSetupCount{ 0u };
    /** The part of `WorkerSetupMs` that loads the root module. */
    double WorkerRootLoadMs{ 0.0 };
    /** The whole `Initialize` and setup of each worker context, including the global session. */
    double WorkerPrepareMs{ 0.0 };
    /** For each batch, the largest prepare time of one context. The batch waits for the slowest context. */
    double WorkerPrepareCriticalPathMs{ 0.0 };
    /** Contexts that finished their setup and ran no job. */
    uint32_t IdleWorkerCount{ 0u };
    double BootstrapCompileMs{ 0.0 };
    double SerializeModulesMs{ 0.0 };
    double WriteModuleCacheMs{ 0.0 };
    double SymbolTableMs{ 0.0 };

    CompileTimings& operator+=(const CompileTimings& other) noexcept;
};

} // namespace lodestone

#endif // !LODESTONE_COMPILE_TIMINGS_HPP
