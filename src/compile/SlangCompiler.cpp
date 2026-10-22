#include "compile/SlangCompiler.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "compile/SymbolTable.hpp"
#include "impl/SlangCompilerTypes.hpp"
#include "impl/SlangModuleContext.hpp"
#include "impl/ThreadPool.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"


#include <algorithm>
#include <cstddef>
#include <expected>
#include <memory>
#include <ranges>
#include <slang.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace lodestone
{

SlangCompiler::SlangCompiler() noexcept
    : compilePool{ nullptr },
      bootstrapContext{ nullptr }
{
}

SlangCompiler::~SlangCompiler() = default;

CookError SlangCompiler::Initialize(SlangCompilerCreateInfo create_info, DiagnosticSink& sink)
{
    diagnosticSink = &sink;
    compilePool = std::make_unique<ThreadPool>();
    bootstrapContext = std::make_unique<SlangModuleContext>();
    if (create_info.InheritedSymbolTable)
    {
        symbolTable = *create_info.InheritedSymbolTable;
    }

    CookError initResult = bootstrapContext->Initialize(create_info, sink);
    if (initResult != CookError::Success)
    {
        return initResult;
    }

    initResult = bootstrapContext->RunBootstrap();
    if (initResult != CookError::Success)
    {
        return initResult;
    }

    std::vector<SerializedModule> serializedModules = bootstrapContext->SerializeModules();
    initResult = compilePool->Initialize(std::move(create_info), std::move(serializedModules));
    if (initResult != CookError::Success)
    {
        return initResult;
    }

    // one writer, after the workers start, so the write overlaps their setup. The workers used to write the cache
    // too, all at once, while other sessions read it
    static_cast<void>(WriteModuleCache(compilePool->SerializedModules(), sink));

    // todo-ship: share symbol tables between the compilers of a multi-module build
    const std::string_view moduleName = bootstrapContext->ModuleName();
    const std::vector<std::string>& sourcePaths = bootstrapContext->ModuleSourcePaths();
    for (const auto&& [sourceIndex, sourceStr] : std::views::enumerate(bootstrapContext->ModuleSourceStrings()))
    {
        symbolTable.AddSource(moduleName, sourcePaths[static_cast<size_t>(sourceIndex)], sourceStr);
    }
    
    return initResult;
}

CookResult<RawModule> SlangCompiler::PrepareRawModule(const PermutationSpace& space)
{
    if (compilePool == nullptr || bootstrapContext == nullptr)
    {
        return std::unexpected(CookError::CompilerNotInitialized);
    }
    const std::vector<std::string_view> sourceViews = bootstrapContext->ModuleSourceStringViews();
    CookResult<std::vector<ExternConstantDefault>> defaults =
        space.CollectUndrivenExternDefaults(ModuleName(), GetSymbolTable(), *diagnosticSink);
    if (!defaults)
    {
        return std::unexpected(defaults.error());
    }

    RawModule module;
    module.Name = bootstrapContext->ModuleName();
    module.EntryPointNames = bootstrapContext->EntryPointNames();
    module.ExternDefaults = std::move(defaults.value());

    // the symbol table gives the expressions, and reflection says which names are module scope
    const std::vector<std::string> moduleScopeNames = bootstrapContext->BuildModuleScopeVariableNames();
    for (const DerivedConstantDeclaration& declaration : symbolTable.DerivedConstantsForModule(ModuleName()))
    {
        if (!std::ranges::binary_search(moduleScopeNames, declaration.Name))
        {
            continue;
        }
        module.DerivedConstants.push_back(RawDerivedConstant{ .Name = std::string{ declaration.Name },
                                                              .Expression = std::string{ declaration.Expression },
                                                              .SourceFile = std::string{ declaration.SourcePath },
                                                              .SourceLine = declaration.SourceLine });
    }
    return module;
}

SlangCompiler::CompileResultList SlangCompiler::Compile(const std::vector<VariantDescriptor>& variants, DiagnosticSink& sink) const
{
    return compilePool->Compile(variants, sink);
}

std::string_view SlangCompiler::ModuleName() const noexcept
{
    return bootstrapContext->ModuleName();
}

size_t SlangCompiler::EntryPointCount() const noexcept
{
    return bootstrapContext->EntryPointNames().size();
}

const std::vector<std::string>& SlangCompiler::EntryPointNames() const noexcept
{
    return bootstrapContext->EntryPointNames();
}

const std::vector<std::string>& SlangCompiler::ModuleSourceStrings() const noexcept
{
    return bootstrapContext->ModuleSourceStrings();
}

const std::vector<std::string>& SlangCompiler::DependencyFilePaths() const noexcept
{
    return bootstrapContext->DependencyFilePaths();
}

std::vector<std::string_view> SlangCompiler::ModuleSourceStringViews() const noexcept
{
    return bootstrapContext->ModuleSourceStringViews();
}

std::vector<RawAxisDeclaration> SlangCompiler::BuildAxisDeclarations() const noexcept
{
    CookResult<std::vector<RawAxisDeclaration>> result = bootstrapContext->BuildDeclaredAxes();
    if (result)
    {
        return std::move(*result);
    }
    else
    {
        return std::vector<RawAxisDeclaration>{};
    }
}

const SymbolTable& SlangCompiler::GetSymbolTable() const noexcept
{
    return symbolTable;
}

} // namespace lodestone
