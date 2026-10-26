#include "driver/CookerSteps.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lodestone
{

    CookStatistics& CookStatistics::operator+=(const CookStatistics& other)
    {
        std::atomic_ref<uint32_t> atomicModulesCooked{ ModulesCooked };
        std::atomic_ref<uint32_t> atomicVariantsCompiled{ VariantsCompiled };
        std::atomic_ref<uint32_t> atomicEntryPointsCompiled{ EntryPointsCompiled };
        std::atomic_ref<uint32_t> atomicReflectionMismatches{ ReflectionMismatches };
        std::atomic_ref<double> atomicElapsedMilliseconds{ ElapsedMilliseconds };
        std::atomic_ref<size_t> atomicTotalSourceBytes{ TotalSourceBytes };
        // do *not* need strict ordering here since we're just incrementing cookwide stats
        atomicModulesCooked.fetch_add(other.ModulesCooked, std::memory_order_relaxed);
        atomicVariantsCompiled.fetch_add(other.VariantsCompiled, std::memory_order_relaxed);
        atomicEntryPointsCompiled.fetch_add(other.EntryPointsCompiled, std::memory_order_relaxed);
        atomicReflectionMismatches.fetch_add(other.ReflectionMismatches, std::memory_order_relaxed);
        atomicElapsedMilliseconds.fetch_add(other.ElapsedMilliseconds, std::memory_order_relaxed);
        atomicTotalSourceBytes.fetch_add(other.TotalSourceBytes, std::memory_order_relaxed);
        return *this;
    }

}
