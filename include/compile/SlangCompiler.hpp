#pragma once
#ifndef LODESTONE_SLANG_COMPILER_HPP
#define LODESTONE_SLANG_COMPILER_HPP
#include "CookerErrors.hpp"
#include "compile/CompileTimings.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/ShimLinkSource.hpp"
#include "compile/SymbolTable.hpp"
#include "permute/PermutationSpace.hpp"
#include "ShaderManifestFormat.hpp"
#include "target/TargetProfile.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The Slang wall: this header names no Slang type. */
namespace lodestone
{

class DiagnosticSink;
class SlangReflector;
class SlangModuleContext;

struct SlangCompilerCreateInfo
{
    std::filesystem::path ModulePath;
    std::filesystem::path ModuleCacheDirectory;
    std::vector<std::filesystem::path> IncludeDirectories;
    uint32_t OptimizationLevel{ 0u };
    uint32_t DebugLevel{ 0u };
    /** @brief Sessions are expensive, so a small batch on every thread oversubscribes. */
    size_t ExpectedBatchSize{ 0u };
    std::optional<SymbolTable> InheritedSymbolTable;
    TargetLanguage Language{ TargetLanguage::Invalid };
    std::string_view SlangProfileName;
    ShimForms Forms;
    /** @brief Given to Slang as the bindless space index. */
    std::optional<uint32_t> DescriptorHeapSet;
    /** @brief `PerDescriptorType` makes each worker load the link module that overrides Slang's default heap lowering. */
    manifest::DescriptorHeapLowering HeapLowering{ manifest::DescriptorHeapLowering::None };
    std::string_view TargetName;
    /** @brief The control arm: each worker parses the root module from source, instead of loading its IR. */
    bool WorkersLoadRootFromSource{ false };
};

class CompilePool;
struct CompileEnvironment;

class SlangCompiler
{
public:
    SlangCompiler() noexcept;
    ~SlangCompiler();
    SlangCompiler(const SlangCompiler&) = delete;
    SlangCompiler& operator=(const SlangCompiler&) = delete;
    SlangCompiler(SlangCompiler&&) = delete;
    SlangCompiler& operator=(SlangCompiler&&) = delete;

    /** @brief Runs the bootstrap compile on the calling thread, using the global session of the calling thread
     * from `pool`. The pool must outlive this compiler. */
    CookError Initialize(CompilePool& pool, SlangCompilerCreateInfo create_info, DiagnosticSink& sink);
    CookResult<RawModule> PrepareRawModule(const PermutationSpace& space);
    using CompileResultList = std::vector<CookResult<RawVariant>>;
    [[nodiscard]] CompileResultList Compile(const std::vector<VariantDescriptor>& variants,
                                            DiagnosticSink& sink) const;
    [[nodiscard]] std::string_view ModuleName() const noexcept;
    [[nodiscard]] size_t EntryPointCount() const noexcept;
    [[nodiscard]] const std::vector<std::string>& EntryPointNames() const noexcept;
    [[nodiscard]] const std::vector<std::string>& ModuleSourceStrings() const noexcept;
    [[nodiscard]] std::vector<std::string_view> ModuleSourceStringViews() const;
    /** @brief The file paths for the depfile, excluding embedded builtins. */
    [[nodiscard]] const std::vector<std::string>& DependencyFilePaths() const noexcept;
    /** @brief Fails when a declaration is invalid. The error is also reported, and the cook must not go on without the axes. */
    [[nodiscard]] CookResult<std::vector<RawAxisDeclaration>> BuildAxisDeclarations() const;
    [[nodiscard]] const SymbolTable& GetSymbolTable() const noexcept;
    /** @brief Returns the setup timings of the bootstrap and of every batch of this compiler so far. */
    [[nodiscard]] CompileTimings Timings() const noexcept;

private:
    mutable CompileTimings setupTimings;
    DiagnosticSink* diagnosticSink{ nullptr };
    SymbolTable symbolTable;
    std::string targetName;
    ShimForms shimForms;
    CompilePool* pool{ nullptr };
    std::unique_ptr<CompileEnvironment> environment;
    std::unique_ptr<SlangModuleContext> bootstrapContext{ nullptr };
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_COMPILER_HPP
