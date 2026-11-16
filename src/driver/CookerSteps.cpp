#include "driver/CookerSteps.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>

namespace lodestone
{

namespace
{
    PhaseTimings g_ProcessPhaseTimings{};
}

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

PhaseTimings& PhaseTimings::operator+=(const PhaseTimings& other) noexcept
{
    PrepareCookMs += other.PrepareCookMs;
    Stage1_DeclareMs += other.Stage1_DeclareMs;
    Stage2_EnumerateMs += other.Stage2_EnumerateMs;
    Stage3_CompileMs += other.Stage3_CompileMs;
    Stage4_ResolveMs += other.Stage4_ResolveMs;
    Validate_EmittedCodeMs += other.Validate_EmittedCodeMs;
    Stage6_InternMs += other.Stage6_InternMs;
    Stage7_FreezeMs += other.Stage7_FreezeMs;
    Validate_ModuleRoundTripsMs += other.Validate_ModuleRoundTripsMs;
    Validate_CrossProfileMs += other.Validate_CrossProfileMs;
    Stage8_EmitMs += other.Stage8_EmitMs;
    CooksRun += other.CooksRun;
    return *this;
}

void AddPhaseTimings(const PhaseTimings& timings) noexcept
{
    std::atomic_ref<double> prep{ g_ProcessPhaseTimings.PrepareCookMs };
    std::atomic_ref<double> s1{ g_ProcessPhaseTimings.Stage1_DeclareMs };
    std::atomic_ref<double> s2{ g_ProcessPhaseTimings.Stage2_EnumerateMs };
    std::atomic_ref<double> s3{ g_ProcessPhaseTimings.Stage3_CompileMs };
    std::atomic_ref<double> s4{ g_ProcessPhaseTimings.Stage4_ResolveMs };
    std::atomic_ref<double> vEmitted{ g_ProcessPhaseTimings.Validate_EmittedCodeMs };
    std::atomic_ref<double> s6{ g_ProcessPhaseTimings.Stage6_InternMs };
    std::atomic_ref<double> s7{ g_ProcessPhaseTimings.Stage7_FreezeMs };
    std::atomic_ref<double> vRoundTrip{ g_ProcessPhaseTimings.Validate_ModuleRoundTripsMs };
    std::atomic_ref<double> vCross{ g_ProcessPhaseTimings.Validate_CrossProfileMs };
    std::atomic_ref<double> s8{ g_ProcessPhaseTimings.Stage8_EmitMs };
    std::atomic_ref<uint32_t> cooks{ g_ProcessPhaseTimings.CooksRun };

    prep.fetch_add(timings.PrepareCookMs, std::memory_order_relaxed);
    s1.fetch_add(timings.Stage1_DeclareMs, std::memory_order_relaxed);
    s2.fetch_add(timings.Stage2_EnumerateMs, std::memory_order_relaxed);
    s3.fetch_add(timings.Stage3_CompileMs, std::memory_order_relaxed);
    s4.fetch_add(timings.Stage4_ResolveMs, std::memory_order_relaxed);
    vEmitted.fetch_add(timings.Validate_EmittedCodeMs, std::memory_order_relaxed);
    s6.fetch_add(timings.Stage6_InternMs, std::memory_order_relaxed);
    s7.fetch_add(timings.Stage7_FreezeMs, std::memory_order_relaxed);
    vRoundTrip.fetch_add(timings.Validate_ModuleRoundTripsMs, std::memory_order_relaxed);
    vCross.fetch_add(timings.Validate_CrossProfileMs, std::memory_order_relaxed);
    s8.fetch_add(timings.Stage8_EmitMs, std::memory_order_relaxed);
    cooks.fetch_add(timings.CooksRun, std::memory_order_relaxed);
}

PhaseTimings GetProcessPhaseTimings() noexcept
{
    std::atomic_ref<double> prep{ g_ProcessPhaseTimings.PrepareCookMs };
    std::atomic_ref<double> s1{ g_ProcessPhaseTimings.Stage1_DeclareMs };
    std::atomic_ref<double> s2{ g_ProcessPhaseTimings.Stage2_EnumerateMs };
    std::atomic_ref<double> s3{ g_ProcessPhaseTimings.Stage3_CompileMs };
    std::atomic_ref<double> s4{ g_ProcessPhaseTimings.Stage4_ResolveMs };
    std::atomic_ref<double> vEmitted{ g_ProcessPhaseTimings.Validate_EmittedCodeMs };
    std::atomic_ref<double> s6{ g_ProcessPhaseTimings.Stage6_InternMs };
    std::atomic_ref<double> s7{ g_ProcessPhaseTimings.Stage7_FreezeMs };
    std::atomic_ref<double> vRoundTrip{ g_ProcessPhaseTimings.Validate_ModuleRoundTripsMs };
    std::atomic_ref<double> vCross{ g_ProcessPhaseTimings.Validate_CrossProfileMs };
    std::atomic_ref<double> s8{ g_ProcessPhaseTimings.Stage8_EmitMs };
    std::atomic_ref<uint32_t> cooks{ g_ProcessPhaseTimings.CooksRun };

    return PhaseTimings{
        .PrepareCookMs = prep.load(std::memory_order_relaxed),
        .Stage1_DeclareMs = s1.load(std::memory_order_relaxed),
        .Stage2_EnumerateMs = s2.load(std::memory_order_relaxed),
        .Stage3_CompileMs = s3.load(std::memory_order_relaxed),
        .Stage4_ResolveMs = s4.load(std::memory_order_relaxed),
        .Validate_EmittedCodeMs = vEmitted.load(std::memory_order_relaxed),
        .Stage6_InternMs = s6.load(std::memory_order_relaxed),
        .Stage7_FreezeMs = s7.load(std::memory_order_relaxed),
        .Validate_ModuleRoundTripsMs = vRoundTrip.load(std::memory_order_relaxed),
        .Validate_CrossProfileMs = vCross.load(std::memory_order_relaxed),
        .Stage8_EmitMs = s8.load(std::memory_order_relaxed),
        .CooksRun = cooks.load(std::memory_order_relaxed)
    };
}

void WriteTestTimingCsvRow(std::string_view test_name, double total_test_ms)
{
    if (test_name == "inner" || test_name == "silent")
    {
        return;
    }

    const char* csvEnv = std::getenv("LODESTONE_TIMING_CSV");
    if (csvEnv == nullptr || *csvEnv == '\0')
    {
        return;
    }

    const char* compilerEnv = std::getenv("LODESTONE_TIMING_COMPILER");
    const std::string compiler = (compilerEnv != nullptr && *compilerEnv != '\0') ? compilerEnv : "unknown";

    const char* configEnv = std::getenv("LODESTONE_TIMING_CONFIG");
    const std::string buildConfig = (configEnv != nullptr && *configEnv != '\0') ? configEnv : "unknown";

    const char* testEnv = std::getenv("LODESTONE_TIMING_TEST");
    const std::string testName = (testEnv != nullptr && *testEnv != '\0') ? testEnv : std::string{ test_name };

    const PhaseTimings timings = GetProcessPhaseTimings();

    const double sumPhases = timings.PrepareCookMs +
                             timings.Stage1_DeclareMs +
                             timings.Stage2_EnumerateMs +
                             timings.Stage3_CompileMs +
                             timings.Stage4_ResolveMs +
                             timings.Validate_EmittedCodeMs +
                             timings.Stage6_InternMs +
                             timings.Stage7_FreezeMs +
                             timings.Validate_ModuleRoundTripsMs +
                             timings.Validate_CrossProfileMs +
                             timings.Stage8_EmitMs;

    const double otherMs = std::max(0.0, total_test_ms - sumPhases);

    const std::filesystem::path csvPath{ csvEnv };
    const bool writeHeader = !std::filesystem::exists(csvPath) || std::filesystem::file_size(csvPath) == 0;

    std::ofstream file(csvPath, std::ios::app);
    if (!file.is_open())
    {
        return;
    }

    if (writeHeader)
    {
        file << "compiler,build_config,test_name,cooks_run,"
                "prepare_cook_ms,stage1_declare_ms,stage2_enumerate_ms,stage3_compile_ms,"
                "stage4_resolve_ms,validate_emitted_code_ms,stage6_intern_ms,stage7_freeze_ms,"
                "validate_module_roundtrips_ms,validate_cross_profile_ms,stage8_emit_ms,"
                "other_test_ms,total_test_ms\n";
    }

    file << std::format("{},{},{},{},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f}\n",
                        compiler,
                        buildConfig,
                        testName,
                        timings.CooksRun,
                        timings.PrepareCookMs,
                        timings.Stage1_DeclareMs,
                        timings.Stage2_EnumerateMs,
                        timings.Stage3_CompileMs,
                        timings.Stage4_ResolveMs,
                        timings.Validate_EmittedCodeMs,
                        timings.Stage6_InternMs,
                        timings.Stage7_FreezeMs,
                        timings.Validate_ModuleRoundTripsMs,
                        timings.Validate_CrossProfileMs,
                        timings.Stage8_EmitMs,
                        otherMs,
                        total_test_ms);
}

}
