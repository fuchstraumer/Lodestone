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
    workers.clear();
}

CookError ThreadPool::Initialize(SlangCompilerCreateInfo create_info, std::vector<SerializedModule> serialized_modules)
{
    createInfo = std::move(create_info);
    serializedModules = std::move(serialized_modules);

    // hardware_concurrency can return 0
    numThreads = createInfo.MultithreadVariantBuild ? std::max(std::thread::hardware_concurrency(), 1u) : 1u;
    if (numThreads > 1u)
    {
        // the calling thread helps after dispatch
        --numThreads;
    }

    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        auto workerLambda = [this, i](std::stop_token token)
        {
            workerFunction(token, i);
        };
        workers.emplace_back(workerLambda);
    }

    return CookError::Success;
}

std::span<const SerializedModule> ThreadPool::SerializedModules() const noexcept
{
    return serializedModules;
}

CookError ThreadPool::prepareContext(SlangModuleContext& module_context,
                                     CompileBatch& batch,
                                     DiagnosticSink& thread_sink)
{
    CookError error = module_context.Initialize(createInfo, thread_sink);
    if (error == CookError::Success)
    {
        error = module_context.RunWorkerSetup(serializedModules);
    }
    
    if (error == CookError::Success)
    {
        error = module_context.LoadShimLinkModule(batch.ShimLinkSource);
    }

    if (error == CookError::Success)
    {
        error = module_context.LoadShimFormatModules(batch.ShimFormatSources);
    }

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

void ThreadPool::runJobs(SlangModuleContext& module_context, CompileBatch& batch)
{
    while (true)
    {
        const int32_t jobIndex = batch.NextJobIndex.fetch_add(1, std::memory_order_relaxed);
        if (jobIndex >= std::ssize(batch.Variants))
        {
            return;
        }

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
    }
}

SlangCompiler::CompileResultList ThreadPool::Compile(const std::vector<VariantDescriptor>& variants,
                                                     std::string_view shim_link_source,
                                                     std::span<const ShimFormatLinkSource> shim_format_sources,
                                                     DiagnosticSink& diagnostic_sink)
{
    std::vector<RecordingDiagnosticSink> diagnosticSinks;
    diagnosticSinks.resize(variants.size());
    // the last sink is the calling thread's
    std::vector<RecordingDiagnosticSink> threadSinks;
    threadSinks.resize(workers.size() + 1u);

    SlangCompiler::CompileResultList results(variants.size(), std::unexpected(CookError::VariantNotCompiled));
    std::latch done{ std::ssize(workers) };

    CompileBatch currBatch
    {
        .Variants = variants,
        .ShimLinkSource = shim_link_source,
        .ShimFormatSources = shim_format_sources,
        .Results = results,
        .NextJobIndex = 0,
        .DoneLatch = &done,
        .DiagnosticSinks = diagnosticSinks,
        .ThreadSinks = threadSinks
    };

    {
        const std::scoped_lock<std::mutex> jobsMutex(mutex);
        activeBatch = &currBatch;
        ++activeBatchIndex;
    }

    condition.notify_all();

    // after waking everyone, do our solemn best to help out
    if (createInfo.MultithreadVariantBuild)
    {
        SlangModuleContext moduleContext;
        if (prepareContext(moduleContext, currBatch, threadSinks.back()) == CookError::Success)
        {
            runJobs(moduleContext, currBatch);
        }
    }

    done.wait();

    {
        // the batch lives on this stack frame
        const std::scoped_lock<std::mutex> jobsMutex(mutex);
        activeBatch = nullptr;
    }

    // a job is left only when every thread failed its setup
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

void ThreadPool::workerFunction(const std::stop_token& stop_token,
                                const size_t thread_idx)
{
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
            batch = activeBatch;
        }

        // the thread must stay: each later batch's latch counts it
        SlangModuleContext moduleContext;
        if (prepareContext(moduleContext, *batch, batch->ThreadSinks[thread_idx]) == CookError::Success)
        {
            runJobs(moduleContext, *batch);
        }

        batch->DoneLatch->count_down();
    }
}

} // namespace lodestone
