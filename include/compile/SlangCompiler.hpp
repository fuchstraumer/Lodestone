#pragma once
#ifndef LODESTONE_SLANG_COMPILER_HPP
#define LODESTONE_SLANG_COMPILER_HPP
#include "CookerErrors.hpp"
#include "RawLibrary.hpp"
#include "Diagnostics.hpp"
#include "permute/PermutationSpace.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** Owns every interaction with Slang. Nothing in this header names a Slang type, so the rest of the
 * cooker links against the data schema rather than against the compiler. */
namespace lodestone
{

struct SlangCompilerImpl;
struct SlangReflector;

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
    using CompileResultList = std::vector<CookResult<RawVariant>>;
    [[nodiscard]] CompileResultList Compile(const std::vector<VariantDescriptor>& variants) const;

    [[nodiscard]] std::string_view GetModuleName() const noexcept;
    [[nodiscard]] std::span<const std::string> GetEntryPointNames() const noexcept;
    /** Every source file the module pulled in, transitively, in Slang's dependency order. */
    [[nodiscard]] std::span<const std::string> GetModuleSourceTexts() const noexcept;

private:
    std::unique_ptr<ThreadPool> compilePool;
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_COMPILER_HPP
