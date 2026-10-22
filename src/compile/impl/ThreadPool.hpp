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

/** @brief Spreads variant jobs over workers, each with its own session primed from the serialized modules. A mutex,
 * not lock-free tricks: the goal is faster variants, not maximum utilization. */
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
    [[nodiscard]] CookError Initialize(SlangCompilerCreateInfo create_info,
                                       std::vector<SerializedModule> serialized_modules);
    [[nodiscard]] SlangCompiler::CompileResultList Compile(const std::vector<VariantDescriptor>& variants,
                                                           DiagnosticSink& diagnostic_sink);
    /** @brief Read the blobs only: copying a `ComPtr` changes the reference count while workers use it. */
    [[nodiscard]] std::span<const SerializedModule> SerializedModules() const noexcept;

private:

    struct CompileBatch
    {
        std::span<const VariantDescriptor> Variants;
        std::span<CookResult<RawVariant>> Results;
        std::atomic<int32_t> NextJobIndex{ 0u };
        std::latch* DoneLatch{ nullptr };
        std::span<RecordingDiagnosticSink> DiagnosticSinks;
        std::span<RecordingDiagnosticSink> ThreadSinks;
        std::atomic<CookError> SetupError{ CookError::Success };
    };

    [[nodiscard]] CookError prepareContext(SlangModuleContext& module_context,
                                           CompileBatch& batch,
                                           DiagnosticSink& thread_sink);
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
