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

    std::vector<Slang::ComPtr<slang::IGlobalSession>> globalSessions(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        if (SLANG_FAILED(slang::createGlobalSession(globalSessions[i].writeRef())) ||
            globalSessions[i].get() == nullptr)
        {
            return CookError::CompilerGlobalSessionCreationFailed;
        }
    }

    // todo-ship: hook up the actual stop_token to allow graceful shutdown of worker threads
    workers.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i)
    {
        workers.emplace_back(&ThreadPool::workerFunction, this, std::stop_token{}, globalSessions[i], i);
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
                                const size_t thread_idx)
{
    int32_t servedIndex = 0;

    const std::vector<slang::CompilerOptionEntry> compileOptions =
        MakeCompilerOptions(createInfo.OptimizationLevel);

    // todo-asap: I am inserting the tests/assets/ rootdir here for the attributes file. This needs to be
    // optionalized and standardized
    const std::filesystem::path attributesPath = std::filesystem::canonical("D:/ShaderTools/tests/assets/");
    const std::string attributesPathStr = attributesPath.string();
    const std::filesystem::path canonicalModulePath = std::filesystem::canonical(createInfo.ModulePath);
    const std::string sourceDirectory = canonicalModulePath.parent_path().string();
    // The shared modules a shader imports -- VeloxAttributes among them -- sit one level above the
    // per-stage directory, so the asset root resolves without a command-line switch.
    const std::string sharedDirectory = canonicalModulePath.parent_path().parent_path().string();
    const std::string cacheDirectory = createInfo.ModuleCacheDirectory.string();
    const std::array<const char*, 4> searchPaths
    {
        sourceDirectory.c_str(),
        sharedDirectory.c_str(),
        cacheDirectory.c_str(),
        attributesPathStr.c_str()
    };

    slang::TargetDesc target{};
    // todo-ship: target output format needs to from compile options, and should be 
    // able to be made into multiple targets. this will require changes to reflection
    // though, so it's a larger job than just the profile opt below
    target.format = SLANG_WGSL;
    // todo-ship: profile should also be a selectable option
    target.profile = global_session->findProfile("spirv_1_4");
    // each job will create their own session: but, global session will be shared
    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
    sessionDesc.compilerOptionEntries = compileOptions.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(compileOptions.size());

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

        // second step on waking: create a new session for this batch of work. this should persist throughout
        // the whole session, as we can reuse it for variants without a problem (its what we did pre-thread-pool)
        // first step: make the session for this job.
        Slang::ComPtr<slang::ISession> session;
        if (SLANG_FAILED(global_session->createSession(sessionDesc, session.writeRef())) || session == nullptr)
        {
            // this thread is good as useless now
            batch->DiagnosticSinks[thread_idx].Report(Diagnostic{
                .Severity = DiagnosticSeverity::Fatal,
                .Code = {},
                .File = {},
                .Range = {},
                .Message = "Failed to create session for variant job.",
                .Context = {},
                .Related = {}
            });
            batch->DoneLatch->count_down();
            // if we can't make a session now, it won't work the next time: return and exit this thread.
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
            // and now get the actual output index, since that can be different
            const int32_t outputIndex = batch->Variants[jobIndexU].Index;
            // and it's signed too, lmao, so back to unsigned! wheeee
            const auto outputIndexU = static_cast<size_t>(outputIndex);
            
        }

        batch->DoneLatch->count_down();
    }
}

} // namespace lodestone
