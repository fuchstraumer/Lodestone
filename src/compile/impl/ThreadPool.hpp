#pragma once
#ifndef LODESTONE_SHADER_COMPILER_THREAD_POOL_HPP
#define LODESTONE_SHADER_COMPILER_THREAD_POOL_HPP
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationSpace.hpp"


#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <latch>
#include <mutex>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace lodestone
{

class SlangModuleContext;

/**@brief The SlangCompiler will initialize this threadpool, and pass it a global session to inherit
 * from. For each child worker, it'll pass the inherited session to it - which it will take as a copy,
 * since Slang global sessions are not thread safe. It'll persist this session throughout it's workloop,
 * since variants don't mutate the session: it'll pop new variant jobs from a queue, using the inherited
 * global session to save on initialization workload.
 *
 * Since this is made just to mostly make variant jobs faster, but not absolutely max utilization,
 * it still uses a mutex instead of lockless structures or any really fancy threading tricks.
 * We just want to make variant job execution more efficient without overcomplicating the threading model.
 */
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
    // Results of initialize can be reused for multiple batches, but likely won't be
    [[nodiscard]] CookError Initialize(SlangCompilerCreateInfo create_info,
                                       std::vector<SerializedModule> serialized_modules);
    [[nodiscard]] SlangCompiler::CompileResultList Compile(const std::vector<VariantDescriptor>& variants,
                                                           DiagnosticSink& diagnostic_sink);
    /**@brief Returns the serialized modules that the workers prime their sessions from. Callers only read the
     * blob bytes and never copy the `ComPtr` values, because a copy changes the blob's reference count while
     * the workers are using it. */
    [[nodiscard]] std::span<const SerializedModule> SerializedModules() const noexcept;

private:

    /**@brief Represents a batch of compile jobs that are currently being processed by the thread pool.
      *Most helpful element is the scoped DoneLatch, which makes sure parent thread wakes when all jobs
       are done. NextJobIndex is the index of the next job to be processed within this batch. */
    struct CompileBatch
    {
        std::span<const VariantDescriptor> Variants;
        std::span<CookResult<RawVariant>> Results;
        std::atomic<int32_t> NextJobIndex{ 0u };
        std::latch* DoneLatch{ nullptr };
        std::span<RecordingDiagnosticSink> DiagnosticSinks;
        std::span<RecordingDiagnosticSink> ThreadSinks;
        // the first setup error of any thread. a failed thread takes no job, so the other threads compile the batch
        std::atomic<CookError> SetupError{ CookError::Success };
    };

    /**@brief Creates a session for one thread and primes it from the serialized modules. A failure is reported to
     * the sink and stored in the batch. */
    [[nodiscard]] CookError prepareContext(SlangModuleContext& module_context,
                                           CompileBatch& batch,
                                           DiagnosticSink& thread_sink);
    /**@brief Takes jobs from the batch until no job is left. */
    static void runJobs(SlangModuleContext& module_context, CompileBatch& batch);

    CompileBatch* activeBatch{ nullptr };

    SlangCompilerCreateInfo createInfo;
    void workerFunction(const std::stop_token& stop_token,
                        size_t thread_idx);
    std::mutex mutex;
    std::condition_variable_any condition;
    std::vector<std::jthread> workers;
    std::vector<SerializedModule> serializedModules;
    size_t numThreads{ 1u };
    int32_t activeBatchIndex{ 0 };
};
} // namespace lodestone

#endif // LODESTONE_SHADER_COMPILER_THREAD_POOL_HPP
