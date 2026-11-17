#include "driver/ParallelFor.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace lodestone
{

void ParallelFor(int32_t task_count, bool multithread, const std::function<void(int32_t)>& task)
{
    // the calling thread works too, and hardware_concurrency can return 0
    const auto hardwareThreads = static_cast<int32_t>(std::max(std::thread::hardware_concurrency(), 1u));
    const int32_t helperCount = multithread ? std::min(hardwareThreads - 1, task_count - 1) : 0;

    std::atomic<int32_t> nextTask{ 0 };
    auto runTasks = [&nextTask, task_count, &task]
    {
        for (int32_t index = nextTask.fetch_add(1, std::memory_order_relaxed); index < task_count;
             index = nextTask.fetch_add(1, std::memory_order_relaxed))
        {
            task(index);
        }
    };

    std::vector<std::jthread> helpers;
    helpers.reserve(static_cast<size_t>(std::max(helperCount, 0)));
    for (int32_t helper = 0; helper < helperCount; ++helper)
    {
        helpers.emplace_back(runTasks);
    }

    runTasks();

    // each jthread joins when `helpers` ends, which is after every task has returned
    helpers.clear();
}

} // namespace lodestone
