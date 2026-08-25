#pragma once
#ifndef LODESTONE_SLANG_COMPILER_HPP
#define LODESTONE_SLANG_COMPILER_HPP
#include "CookerErrors.hpp"
#include "RawLibrary.hpp"
#include "Diagnostics.hpp"
#include "permute/PermutationSpace.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

/** Owns every interaction with Slang. Nothing in this header names a Slang type, so the rest of the
 * cooker links against the data schema rather than against the compiler. */
namespace lodestone
{

struct SlangReflector;
class SlangModuleContext;

struct SlangCompilerCreateInfo
{
    std::filesystem::path ModulePath;
    std::filesystem::path ModuleCacheDirectory;
    uint32_t OptimizationLevel{ 0u };
    bool MultithreadEntryPointCodegen{ true };
};

class ThreadPool;

class SlangCompiler
{
public:
    SlangCompiler() noexcept;
    ~SlangCompiler();
    SlangCompiler(const SlangCompiler&) = delete;
    SlangCompiler& operator=(const SlangCompiler&) = delete;
    SlangCompiler(SlangCompiler&&) = delete;
    SlangCompiler& operator=(SlangCompiler&&) = delete;

    CookError Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink);
    CookResult<RawModule> PrepareRawModule(const PermutationSpace& space);
    using CompileResultList = std::vector<CookResult<RawVariant>>;
    [[nodiscard]] CompileResultList Compile(const std::vector<VariantDescriptor>& variants) const;

private:
    std::unique_ptr<ThreadPool> compilePool{ nullptr };
    std::unique_ptr<SlangModuleContext> bootstrapContext{ nullptr };
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_COMPILER_HPP
