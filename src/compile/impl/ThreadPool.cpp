#include "ThreadPool.hpp"
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
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

CookError ThreadPool::Initialize(SlangCompilerCreateInfo create_info, size_t num_threads_override)
{
    createInfo = std::move(create_info);
    numThreads = (num_threads_override == 0u) ? std::thread::hardware_concurrency() : num_threads_override;
    if (numThreads > 1u)
    {
        // take off one thread for the main thread, since it works as a helper after dispatching
        --numThreads;
    }

    // todo-ship: hook up the actual stop_token to allow graceful shutdown of worker threads
    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        workers.emplace_back(&ThreadPool::workerFunction, this, globalSessions[i], sessions[i], i);
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

    SlangCompiler::CompileResultList results(variants.size());
    std::latch done{ std::ssize(workers) };

    CompileBatch currBatch
    {
        .Variants = variants,
        .Results = results,
        .NextJobIndex = 0,
        .DoneLatch = &done,
        .DiagnosticSinks = diagnosticSinks
    };

    {
        const std::scoped_lock<std::mutex> jobsMutex(mutex);
        activeBatch = &currBatch;
        ++activeBatchIndex;
    }

    // todo-like-crazy-soon: have the main thread just join the work. since we're using atomics and the latch,
    // this should be trivial. join work after the condition_variable, and still wait on done: we could complete
    // before one of our pending child threads, and we want to wait on them just the same

    condition.notify_all();
    done.wait();

    // merge per variant diagnostics into the main diagnostic sink
    for (auto& sink : diagnosticSinks)
    {
        for (auto&& record : sink.Records())
        {
            diagnostic_sink.Report(std::move(record));
        }
    }

    return results;
}

void ThreadPool::workerFunction(const std::stop_token& stop_token,
                                const Slang::ComPtr<slang::IGlobalSession>& global_session,
                                const Slang::ComPtr<slang::ISession>& session,
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
            
        }

        batch->DoneLatch->count_down();
    }
}

} // namespace lodestone
