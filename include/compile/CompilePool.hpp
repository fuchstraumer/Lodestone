#pragma once
#ifndef LODESTONE_COMPILE_POOL_HPP
#define LODESTONE_COMPILE_POOL_HPP
#include <memory>

/** The Slang wall: this header names no Slang type. */
namespace lodestone
{

class ThreadPool;

/** @brief The compile threads of one cook. Each thread creates one Slang global session the first time it needs one
 * and keeps it until the pool ends, so a compiler for each environment does not load the core module again. Create
 * one for the whole cook, and keep it alive longer than every `SlangCompiler` that uses it. The thread that calls
 * `SlangCompiler::Initialize` and `SlangCompiler::Compile` must be the same for the whole life of the pool. */
class CompilePool
{
public:
    /** @brief Without `multithread_variant_build`, the calling thread runs every job. */
    explicit CompilePool(bool multithread_variant_build);
    ~CompilePool();
    CompilePool(const CompilePool&) = delete;
    CompilePool& operator=(const CompilePool&) = delete;
    CompilePool(CompilePool&&) = delete;
    CompilePool& operator=(CompilePool&&) = delete;

    [[nodiscard]] ThreadPool& Threads() const noexcept;

private:
    std::unique_ptr<ThreadPool> threads;
};

} // namespace lodestone

#endif // !LODESTONE_COMPILE_POOL_HPP
