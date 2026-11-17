#include "compile/CompilePool.hpp"
#include "impl/ThreadPool.hpp"
#include <memory>

namespace lodestone
{

CompilePool::CompilePool(bool multithread_variant_build) :
  threads{ std::make_unique<ThreadPool>() }
{
    threads->Initialize(multithread_variant_build);
}

CompilePool::~CompilePool() = default;

ThreadPool& CompilePool::Threads() const noexcept
{
    return *threads;
}

} // namespace lodestone
