#pragma once
#ifndef LODESTONE_DRIVER_PARALLEL_FOR_HPP
#define LODESTONE_DRIVER_PARALLEL_FOR_HPP
#include <cstdint>
#include <functional>

namespace lodestone
{

/** @brief Calls `task` once for each index in [0, task_count), spread over the calling thread and up to
 * `hardware_concurrency - 1` helper threads, and returns when every call has returned. Tasks start in index order, so
 * put the longest tasks first. Without `multithread`, the calling thread runs every task in index order. A task must
 * write only its own output slot. */
void ParallelFor(int32_t task_count, bool multithread, const std::function<void(int32_t)>& task);

} // namespace lodestone

#endif // !LODESTONE_DRIVER_PARALLEL_FOR_HPP
