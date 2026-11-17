#include "ThreadPool.hpp"
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "SlangModuleContext.hpp"
#include "SlangReflector.hpp"
#include "SlangVariantCompiler.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationSpace.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <latch>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lodestone
{

ThreadPool::ThreadPool() noexcept = default;

ThreadPool::~ThreadPool()
{
    // each worker releases its global session on its own thread when it exits
    workers.clear();
}

void ThreadPool::Initialize(bool multithread_variant_build)
{
    // the calling thread helps, and hardware_concurrency can return 0
    const size_t hardwareThreads = std::max(std::thread::hardware_concurrency(), 1u);
    const size_t workerCount = multithread_variant_build ? hardwareThreads - 1u : 0u;

    workers.reserve(workerCount);
    for (size_t i = 0; i < workerCount; ++i)
    {
        auto workerLambda = [this, i](std::stop_token token)
        {
            workerFunction(token, i);
        };
        workers.emplace_back(workerLambda);
    }
}

CookResult<slang::IGlobalSession*> ThreadPool::AcquireCallerGlobalSession(CompileTimings& timings)
{
    ContextTimings creation;
    const CookError error = ensureGlobalSession(callerSession, creation);
    if (error != CookError::Success)
    {
        return std::unexpected(error);
    }

    if (creation.GlobalSessionCreated)
    {
        timings.BootstrapGlobalSessionMs += creation.GlobalSessionMs;
        timings.BootstrapGlobalSessionCount += 1u;
    }

    return callerSession.Session.get();
}

void ThreadPool::recordBatchTimings(std::span<const ContextTimings> thread_timings, CompileTimings& batch_timings)
{
    double slowestPrepareMs = 0.0;

    for (const ContextTimings& contextTimings : thread_timings)
    {
        if (contextTimings.GlobalSessionCreated)
        {
            batch_timings.WorkerGlobalSessionMs += contextTimings.GlobalSessionMs;
            batch_timings.WorkerGlobalSessionCount += 1u;
        }

        if (!contextTimings.Prepared)
        {
            continue;
        }

        batch_timings.WorkerSetupMs += contextTimings.SetupMs;
        batch_timings.WorkerSetupCount += 1u;
        batch_timings.WorkerPrepareMs += contextTimings.PrepareMs;
        batch_timings.IdleWorkerCount += contextTimings.JobsRun == 0u ? 1u : 0u;
        slowestPrepareMs = std::max(slowestPrepareMs, contextTimings.PrepareMs);
    }

    batch_timings.WorkerPrepareCriticalPathMs += slowestPrepareMs;
}

CookError ThreadPool::ensureGlobalSession(GlobalSessionSlot& slot, ContextTimings& context_timings)
{
    if (slot.Session != nullptr)
    {
        return CookError::Success;
    }

    const std::chrono::steady_clock::time_point creationStart = std::chrono::steady_clock::now();
    const CookError error = CreateGlobalSession(slot.Session);
    const std::chrono::steady_clock::time_point creationEnd = std::chrono::steady_clock::now();
    context_timings.GlobalSessionMs = std::chrono::duration<double, std::milli>{ creationEnd - creationStart }.count();
    context_timings.GlobalSessionCreated = error == CookError::Success;

    return error;
}

CookError ThreadPool::prepareContext(SlangModuleContext& module_context,
                                     GlobalSessionSlot& slot,
                                     CompileBatch& batch,
                                     DiagnosticSink& thread_sink,
                                     ContextTimings& context_timings)
{
    const CompileEnvironment& environment = *batch.Environment;
    const std::chrono::steady_clock::time_point prepareStart = std::chrono::steady_clock::now();

    CookError error = ensureGlobalSession(slot, context_timings);
    if (error == CookError::Success)
    {
        error = module_context.Initialize(environment.CreateInfo, slot.Session.get(), thread_sink);
    }

    std::chrono::steady_clock::time_point setupStart = prepareStart;
    if (error == CookError::Success)
    {
        setupStart = std::chrono::steady_clock::now();
        error = module_context.RunWorkerSetup(environment.SerializedModules);
    }

    if (error == CookError::Success)
    {
        error = module_context.LoadShimLinkModule(environment.ShimLinkSource);
    }

    if (error == CookError::Success)
    {
        error = module_context.LoadShimFormatModules(environment.ShimFormatSources);
    }

    if (error == CookError::Success)
    {
        error = module_context.LoadHeapLoweringModule();
    }

    const std::chrono::steady_clock::time_point prepareEnd = std::chrono::steady_clock::now();
    context_timings.SetupMs = std::chrono::duration<double, std::milli>{ prepareEnd - setupStart }.count();
    context_timings.PrepareMs = std::chrono::duration<double, std::milli>{ prepareEnd - prepareStart }.count();
    context_timings.Prepared = error == CookError::Success;

    if (error != CookError::Success)
    {
        // the usual cause is a module cache from another Slang version
        static_cast<void>(ReportError(thread_sink,
                                      error,
                                      std::format("a compile thread cannot prepare its Slang session ({})", ToString(error))));
        CookError expected = CookError::Success;
        batch.SetupError.compare_exchange_strong(expected, error);
    }

    return error;
}

void ThreadPool::serveBatch(GlobalSessionSlot& slot,
                            CompileBatch& batch,
                            DiagnosticSink& thread_sink,
                            ContextTimings& context_timings)
{
    // a thread that gets no job prepares nothing
    const int32_t firstJobIndex = batch.NextJobIndex.fetch_add(1, std::memory_order_relaxed);
    if (firstJobIndex >= std::ssize(batch.Variants))
    {
        return;
    }

    SlangModuleContext moduleContext;
    if (prepareContext(moduleContext, slot, batch, thread_sink, context_timings) != CookError::Success)
    {
        // this thread took the job, so no other thread runs it
        batch.Results[static_cast<size_t>(firstJobIndex)] = std::unexpected(CookError::WorkerSetupFailed);
        return;
    }

    context_timings.JobsRun = runJobs(moduleContext, batch, firstJobIndex);
}

uint32_t ThreadPool::runJobs(SlangModuleContext& module_context, CompileBatch& batch, int32_t first_job_index)
{
    uint32_t jobsRun = 0u;
    int32_t jobIndex = first_job_index;

    while (jobIndex < std::ssize(batch.Variants))
    {
        ++jobsRun;
        const auto jobIndexU = static_cast<size_t>(jobIndex);
        SlangVariantCompiler variantCompiler;
        CookResult<LinkedVariant> variantBuildResult =
            variantCompiler.CompileVariant(module_context, batch.Variants[jobIndexU], batch.DiagnosticSinks[jobIndexU]);
        SlangReflector variantReflector(module_context.EntryPointNames(),
                                        &batch.DiagnosticSinks[jobIndexU],
                                        module_context.GlobalSession(),
                                        module_context.BufferForm(),
                                        module_context.Language());

        batch.Results[jobIndexU] = variantBuildResult ? variantReflector.Reflect(*variantBuildResult, batch.Variants[jobIndexU])
                                                      : std::unexpected(variantBuildResult.error());

        jobIndex = batch.NextJobIndex.fetch_add(1, std::memory_order_relaxed);
    }

    return jobsRun;
}

SlangCompiler::CompileResultList ThreadPool::Compile(const CompileEnvironment& environment,
                                                     const std::vector<VariantDescriptor>& variants,
                                                     DiagnosticSink& diagnostic_sink,
                                                     CompileTimings& batch_timings)
{
    std::vector<RecordingDiagnosticSink> diagnosticSinks;
    diagnosticSinks.resize(variants.size());
    // the last sink is the calling thread's
    std::vector<RecordingDiagnosticSink> threadSinks;
    threadSinks.resize(workers.size() + 1u);
    std::vector<ContextTimings> threadTimings(workers.size() + 1u);

    SlangCompiler::CompileResultList results(variants.size(), std::unexpected(CookError::VariantNotCompiled));

    // the calling thread runs a job too, so a batch of one job wakes no worker
    const size_t jobsForWorkers = variants.empty() ? 0u : variants.size() - 1u;
    const auto workersNeeded = static_cast<int32_t>(std::min(workers.size(), jobsForWorkers));
    std::latch done{ workersNeeded };

    CompileBatch currBatch;
    currBatch.Environment = &environment;
    currBatch.Variants = variants;
    currBatch.Results = results;
    currBatch.WorkersNeeded = workersNeeded;
    currBatch.DoneLatch = &done;
    currBatch.DiagnosticSinks = diagnosticSinks;
    currBatch.ThreadSinks = threadSinks;
    currBatch.ThreadTimings = threadTimings;

    if (workersNeeded > 0)
    {
        {
            const std::scoped_lock<std::mutex> jobsMutex(mutex);
            activeBatch = &currBatch;
            ++activeBatchIndex;
        }

        // a worker with an index of workersNeeded or more goes back to sleep
        condition.notify_all();
    }

    // after waking the workers, do our solemn best to help out
    serveBatch(callerSession, currBatch, threadSinks.back(), threadTimings.back());

    done.wait();
    recordBatchTimings(threadTimings, batch_timings);

    if (workersNeeded > 0)
    {
        // the batch lives on this stack frame
        const std::scoped_lock<std::mutex> jobsMutex(mutex);
        activeBatch = nullptr;
    }

    // a job is left only when a thread that took it failed its setup
    if (currBatch.SetupError.load() != CookError::Success)
    {
        for (CookResult<RawVariant>& result : results)
        {
            if (!result && result.error() == CookError::VariantNotCompiled)
            {
                result = std::unexpected(CookError::WorkerSetupFailed);
            }
        }
    }

    // setup errors first, so the cause prints before its effects
    for (auto&& sink : threadSinks)
    {
        for (auto&& record : sink.Records())
        {
            diagnostic_sink.Report(std::move(record));
        }
    }

    for (auto&& sink : diagnosticSinks)
    {
        for (auto&& record : sink.Records())
        {
            diagnostic_sink.Report(std::move(record));
        }
    }

    return results;
}

void ThreadPool::workerFunction(const std::stop_token& stop_token, const size_t thread_idx)
{
    // lives as long as the thread, so the session is created and released on this thread
    GlobalSessionSlot slot;
    int32_t servedIndex = 0;

    while (!stop_token.stop_requested())
    {
        CompileBatch* batch = nullptr;
        {
            std::unique_lock<std::mutex> queueLock(mutex);
            const bool haveWork = condition.wait(queueLock,
                                                 stop_token,
                                                 [&servedIndex, this]
                                                 {
                                                     return servedIndex != activeBatchIndex;
                                                 });

            if (!haveWork)
            {
                return;
            }

            servedIndex = activeBatchIndex;

            // a finished batch has no pointer. the lowest worker indices serve each batch, so the same
            // workers keep their global sessions from one batch to the next
            if (activeBatch != nullptr && std::cmp_less(thread_idx, activeBatch->WorkersNeeded))
            {
                batch = activeBatch;
            }
        }

        if (batch == nullptr)
        {
            continue;
        }

        serveBatch(slot, *batch, batch->ThreadSinks[thread_idx], batch->ThreadTimings[thread_idx]);
        batch->DoneLatch->count_down();
    }
}

} // namespace lodestone
