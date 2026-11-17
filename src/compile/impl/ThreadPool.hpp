#pragma once
#ifndef LODESTONE_SHADER_COMPILER_THREAD_POOL_HPP
#define LODESTONE_SHADER_COMPILER_THREAD_POOL_HPP
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "Diagnostics.hpp"
#include "compile/CompileTimings.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/ShimLinkSource.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationSpace.hpp"
#include "slang.h"
#include "slang-com-ptr.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lodestone
{

class SlangModuleContext;

/** @brief What a batch needs from one environment, which is a module cooked for one profile. */
struct CompileEnvironment
{
    SlangCompilerCreateInfo CreateInfo;
    /** The modules of the bootstrap. Read the blobs only: copying a `ComPtr` changes the reference count while
     * workers use it. */
    std::vector<SerializedModule> SerializedModules;
    /** Every context loads it once. Empty without shim externs. */
    std::string ShimLinkSource;
    std::vector<ShimFormatLinkSource> ShimFormatSources;
};

/** @brief Spreads variant jobs over threads for the whole cook. Each thread creates one global session when it first
 * needs one, and keeps it. A global session must never be used by two threads at once. A thread that takes a job
 * creates a session of the environment from its global session, primed from the serialized modules. A mutex, not
 * lock-free tricks: the goal is faster variants, not maximum utilization. */
class ThreadPool
{
public:
    using JobFunction = std::function<CookResult<RawVariant>(const VariantDescriptor&)>;

    ThreadPool() noexcept;
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /** @brief Starts the worker threads. Without multithreading the calling thread runs every job. */
    void Initialize(bool multithread_variant_build);

    /** @brief Returns the global session of the calling thread, and creates it the first time. The calling thread is
     * the one that calls `Compile`. Adds the time of a creation to the bootstrap timings. */
    [[nodiscard]] CookResult<slang::IGlobalSession*> AcquireCallerGlobalSession(CompileTimings& timings);

    /** @brief Adds the worker timings of the batch to `batch_timings`. */
    [[nodiscard]] SlangCompiler::CompileResultList Compile(const CompileEnvironment& environment,
                                                           const std::vector<VariantDescriptor>& variants,
                                                           DiagnosticSink& diagnostic_sink,
                                                           CompileTimings& batch_timings);

private:

    /** The global session of one thread, and the thread that created it is the thread that uses it. */
    struct GlobalSessionSlot
    {
        Slang::ComPtr<slang::IGlobalSession> Session;
    };

    /** Written by the thread that owns the context only, and read after the batch ends. */
    struct ContextTimings
    {
        double GlobalSessionMs{ 0.0 };
        double SetupMs{ 0.0 };
        double PrepareMs{ 0.0 };
        uint32_t JobsRun{ 0u };
        bool GlobalSessionCreated{ false };
        bool Prepared{ false };
    };

    struct CompileBatch
    {
        const CompileEnvironment* Environment{ nullptr };
        std::span<const VariantDescriptor> Variants;
        std::span<CookResult<RawVariant>> Results;
        std::atomic<int32_t> NextJobIndex{ 0 };
        /** Workers 0 up to this count join the batch, and the done latch counts them. */
        int32_t WorkersNeeded{ 0 };
        std::latch* DoneLatch{ nullptr };
        std::span<RecordingDiagnosticSink> DiagnosticSinks;
        std::span<RecordingDiagnosticSink> ThreadSinks;
        /** One for each worker, and the last is the calling thread's. */
        std::span<ContextTimings> ThreadTimings;
        std::atomic<CookError> SetupError{ CookError::Success };
    };

    [[nodiscard]] static CookError ensureGlobalSession(GlobalSessionSlot& slot, ContextTimings& context_timings);
    [[nodiscard]] static CookError prepareContext(SlangModuleContext& module_context,
                                                  GlobalSessionSlot& slot,
                                                  CompileBatch& batch,
                                                  DiagnosticSink& thread_sink,
                                                  ContextTimings& context_timings);
    /** @brief Takes a job first, and prepares a context only when a job exists. */
    static void serveBatch(GlobalSessionSlot& slot,
                           CompileBatch& batch,
                           DiagnosticSink& thread_sink,
                           ContextTimings& context_timings);
    /** @brief Returns the number of jobs that this context ran, starting with `first_job_index`. */
    [[nodiscard]] static uint32_t runJobs(SlangModuleContext& module_context,
                                          CompileBatch& batch,
                                          int32_t first_job_index);
    static void recordBatchTimings(std::span<const ContextTimings> thread_timings, CompileTimings& batch_timings);
    void workerFunction(const std::stop_token& stop_token, size_t thread_idx);

    CompileBatch* activeBatch{ nullptr };
    std::mutex mutex;
    std::condition_variable_any condition;
    GlobalSessionSlot callerSession;
    std::vector<std::jthread> workers;
    int32_t activeBatchIndex{ 0 };
};
} // namespace lodestone

#endif // LODESTONE_SHADER_COMPILER_THREAD_POOL_HPP
