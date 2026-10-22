#pragma once
#ifndef LODESTONE_SLANG_COMPILER_HPP
#define LODESTONE_SLANG_COMPILER_HPP
#include "CookerErrors.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SymbolTable.hpp"
#include "permute/PermutationSpace.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/** The Slang wall: this header names no Slang type. */
namespace lodestone
{

class DiagnosticSink;
struct SlangReflector;
class SlangModuleContext;

struct SlangCompilerCreateInfo
{
    std::filesystem::path ModulePath;
    std::filesystem::path ModuleCacheDirectory;
    uint32_t OptimizationLevel{ 0u };
    bool MultithreadVariantBuild{ true };
    /** @brief Sessions are expensive, so a small batch on every thread oversubscribes. */
    size_t ExpectedBatchSize{ 0u };
    std::optional<SymbolTable> InheritedSymbolTable;
    TargetLanguage Language{ TargetLanguage::Invalid };
    std::string_view SlangProfileName;
    PlacementKind AccessModel;
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

    CookError Initialize(SlangCompilerCreateInfo create_info, DiagnosticSink& sink);
    CookResult<RawModule> PrepareRawModule(const PermutationSpace& space);
    using CompileResultList = std::vector<CookResult<RawVariant>>;
    [[nodiscard]] CompileResultList Compile(const std::vector<VariantDescriptor>& variants,
                                            DiagnosticSink& sink) const;
    [[nodiscard]] std::string_view ModuleName() const noexcept;
    [[nodiscard]] size_t EntryPointCount() const noexcept;
    [[nodiscard]] const std::vector<std::string>& EntryPointNames() const noexcept;
    [[nodiscard]] const std::vector<std::string>& ModuleSourceStrings() const noexcept;
    [[nodiscard]] std::vector<std::string_view> ModuleSourceStringViews() const noexcept;
    /** @brief The file paths for the depfile, excluding embedded builtins. */
    [[nodiscard]] const std::vector<std::string>& DependencyFilePaths() const noexcept;
    [[nodiscard]] std::vector<RawAxisDeclaration> BuildAxisDeclarations() const noexcept;
    [[nodiscard]] const SymbolTable& GetSymbolTable() const noexcept;

private:
    DiagnosticSink* diagnosticSink{ nullptr };
    SymbolTable symbolTable;
    std::unique_ptr<ThreadPool> compilePool{ nullptr };
    std::unique_ptr<SlangModuleContext> bootstrapContext{ nullptr };
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_COMPILER_HPP
