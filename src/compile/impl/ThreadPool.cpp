#include "ThreadPool.hpp"
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "SlangModuleContext.hpp"
#include "SlangReflector.hpp"
#include "SlangVariantCompiler.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationSpace.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <latch>
#include <mutex>
#include <stdexcept>
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
    // dump module binaries to disk
}

CookError ThreadPool::Initialize(SlangCompilerCreateInfo create_info, std::vector<SerializedModule> serialized_modules)
{
    createInfo = std::move(create_info);
    serializedModules = std::move(serialized_modules);

    numThreads = createInfo.MultithreadVariantBuild ? std::thread::hardware_concurrency() : 1u;
    if (numThreads > 1u)
    {
        // take off one thread for the main thread, since it works as a helper after dispatching
        --numThreads;
    }

    // todo-ship: hook up the actual stop_token to allow graceful shutdown of worker threads
    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        // need to inject stop_token since jthread invocable check doesn't work
        // with member functions taking stop_token? i think
        auto workerLambda = [this, i](std::stop_token token)
        {
            workerFunction(token, i);
        };
        workers.emplace_back(workerLambda);
    }

    return CookError::Success;
}

SlangCompiler::CompileResultList ThreadPool::Compile(const std::vector<VariantDescriptor>& variants,
                                                     DiagnosticSink& diagnostic_sink)
{
    // we create recording diagnostic sinks for each variant to capture their diagnostics separately,
    // and we'll merge them into the main sink (no matter it's type) after the work completes.
    std::vector<RecordingDiagnosticSink> diagnosticSinks;
    diagnosticSinks.resize(variants.size());
    std::vector<RecordingDiagnosticSink> threadSinks;
    threadSinks.resize(workers.size());
    
    SlangCompiler::CompileResultList results(variants.size(), std::unexpected(CookError::VariantNotCompiled));
    std::latch done{ std::ssize(workers) };
    
    CompileBatch currBatch
    {
        .Variants = variants,
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
        // only enter here if multithreading already enabled: if not, let the singular worker toil away
        SlangModuleContext moduleContext;
        CookError contextError = moduleContext.Initialize(createInfo, diagnostic_sink);
        if (contextError != CookError::Success)
        {
            //currBatch.ThreadSinks[thread_idx].Report(CookError::ModuleContextInitializationFailed);
            throw std::runtime_error("Module context initialization failed");
        }

        contextError = moduleContext.RunWorkerSetup(serializedModules);
        if (contextError != CookError::Success)
        {
            throw std::runtime_error("Module context setup failed");
        }
        
        while (true)
        {
            // all we need is a unique index, not a strictly ordered one
            const int32_t jobIndex = currBatch.NextJobIndex.fetch_add(1, std::memory_order_relaxed);

            if (jobIndex >= std::ssize(currBatch.Variants))
            {
                // all done;
                break;
            }

            // rolling eyes because i have to convert jobindex to unsigned to avoid goofy signed/unsigned comparison warnings
            const auto jobIndexU = static_cast<size_t>(jobIndex);
            SlangVariantCompiler variantCompiler;
            // todo: standardize how these two take diagnostic sinks, instead of one being pointer and one being reference
            CookResult<LinkedVariant> variantBuildResult = variantCompiler.CompileVariant(moduleContext, currBatch.Variants[jobIndexU], currBatch.DiagnosticSinks[jobIndexU]);
            SlangReflector variantReflector(moduleContext.EntryPointNames(), &currBatch.DiagnosticSinks[jobIndexU], moduleContext.GlobalSession(), moduleContext.PlacementKindForTarget());

            currBatch.Results[jobIndexU] = variantBuildResult ? variantReflector.Reflect(*variantBuildResult, currBatch.Variants[jobIndexU]) : std::unexpected(CookError::VariantModuleCreationFailed);
        }
    }

    done.wait();

    // merge per variant diagnostics into the main diagnostic sink
    for (auto&& sink : diagnosticSinks)
    {
        for (auto&& record : sink.Records())
        {
            diagnostic_sink.Report(std::move(record));
        }
    }

    for (auto&& sink : threadSinks)
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

        SlangModuleContext moduleContext;
        CookError contextError = moduleContext.Initialize(createInfo, batch->ThreadSinks[thread_idx]);
        if (contextError != CookError::Success)
        {
            //batch->ThreadSinks[thread_idx].Report(CookError::ModuleContextInitializationFailed);
            batch->DoneLatch->count_down();
            return;
        }

        contextError = moduleContext.RunWorkerSetup(serializedModules);
        if (contextError != CookError::Success)
        {
            //batch->ThreadSinks[thread_idx].Report(CookError::ModuleContextInitializationFailed);
            batch->DoneLatch->count_down();
            return;
        }

        while (true)
        {
            // all we need is a unique index, not a strictly ordered one
            const int32_t jobIndex = batch->NextJobIndex.fetch_add(1, std::memory_order_relaxed);

            if (jobIndex >= std::ssize(batch->Variants))
            {
                // all done;
                break;
            }

            // rolling eyes because i have to convert jobindex to unsigned to avoid goofy signed/unsigned comparison warnings
            const auto jobIndexU = static_cast<size_t>(jobIndex);
            SlangVariantCompiler variantCompiler;
            // todo: standardize how these two take diagnostic sinks, instead of one being pointer and one being reference
            CookResult<LinkedVariant> variantBuildResult = variantCompiler.CompileVariant(moduleContext, batch->Variants[jobIndexU], batch->DiagnosticSinks[jobIndexU]);
            SlangReflector variantReflector(moduleContext.EntryPointNames(), &batch->DiagnosticSinks[jobIndexU], moduleContext.GlobalSession(), moduleContext.PlacementKindForTarget());

            batch->Results[jobIndexU] = variantBuildResult ? variantReflector.Reflect(*variantBuildResult, batch->Variants[jobIndexU]) : std::unexpected(CookError::VariantModuleCreationFailed);
        }

        const CookError writeCacheError = moduleContext.WriteModuleCache();
        if (writeCacheError != CookError::Success)
        {
            //batch->ThreadSinks[thread_idx].Report(CookError::ModuleContextInitializationFailed);
        }
        batch->DoneLatch->count_down();
    }
}

} // namespace lodestone
