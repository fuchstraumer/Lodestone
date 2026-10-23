#include "compile/SlangCompiler.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/ShimLinkSource.hpp"
#include "compile/SlangDiagnosticParser.hpp"
#include "compile/SymbolTable.hpp"
#include "impl/SlangCompilerTypes.hpp"
#include "impl/SlangModuleContext.hpp"
#include "impl/ThreadPool.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "target/ShimTable.hpp"


#include <algorithm>
#include <cstddef>
#include <expected>
#include <format>
#include <optional>
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
    targetName = std::string{ create_info.TargetName };
    shimAccessModel = AccessModelFromPlacementKind(create_info.AccessModel);
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

    // the symbol table reads the text and reflection confirms the type exists, so each is a second opinion for the other
    const std::vector<std::string> moduleScopeStructs = bootstrapContext->BuildModuleScopeStructNames();
    // reflection only: the attribute is usually on the line before the extern
    const std::vector<StructStringAttribute> formatAttributes = bootstrapContext->BuildStructStringAttributes("ls_storage_format");
    for (const ExternStructDeclaration& declaration : symbolTable.ExternStructsForModule(ModuleName()))
    {
        const ShaderLocation where{ .File = declaration.SourcePath, .Line = declaration.SourceLine };
        const auto formatAttribute = std::ranges::find(formatAttributes, declaration.Name, &StructStringAttribute::StructName);
        const ShimInterface* shimInterface = FindShimInterface(declaration.Interface);
        if (shimInterface == nullptr)
        {
            if (formatAttribute != formatAttributes.end())
            {
                return std::unexpected(ReportError(*diagnosticSink,
                                                   CookError::ShimExternInvalid,
                                                   where,
                                                   std::format("extern struct '{}' has [ls_storage_format], and '{}' is not "
                                                               "a shim interface",
                                                               declaration.Name,
                                                               declaration.Interface)));
            }
            continue;
        }

        if (formatAttribute != formatAttributes.end() && formatAttribute->Argument.empty())
        {
            return std::unexpected(ReportError(*diagnosticSink,
                                               CookError::ShimStorageFormatAxisUnknown,
                                               where,
                                               std::format("[ls_storage_format] on extern '{}' names no axis", declaration.Name)));
        }

        if (!std::ranges::binary_search(moduleScopeStructs, declaration.Name))
        {
            return std::unexpected(ReportError(*diagnosticSink,
                                               CookError::ShimExternNotReflected,
                                               where,
                                               std::format("extern struct '{}' names the shim interface '{}', but "
                                                           "reflection finds no module scope struct of that name",
                                                           declaration.Name,
                                                           declaration.Interface)));
        }

        RawShimExtern shimExtern{ .Name = std::string{ declaration.Name },
                                  .Interface = std::string{ declaration.Interface },
                                  .TypeArguments = std::string{ declaration.TypeArguments },
                                  .ArgumentModules = {},
                                  .Class = shimInterface->Class,
                                  .FormatAxis = formatAttribute != formatAttributes.end() ? formatAttribute->Argument
                                                                                          : std::string{},
                                  .SourceFile = std::string{ declaration.SourcePath },
                                  .SourceLine = declaration.SourceLine };

        // a count that differs from the interface is reported with the link source, which knows the table
        const std::vector<std::string_view> arguments = SplitShimTypeArguments(declaration.TypeArguments);
        if (arguments.size() == shimInterface->TypeParameterCount)
        {
            for (const std::string_view argument : arguments)
            {
                const std::optional<std::string> declaringModule = bootstrapContext->FindTypeDeclaringModule(argument);
                if (!declaringModule.has_value())
                {
                    return std::unexpected(ReportError(*diagnosticSink,
                                                       CookError::ShimExternInvalid,
                                                       where,
                                                       std::format("type argument '{}' of extern '{}' is not a type that "
                                                                   "the module can see",
                                                                   argument,
                                                                   declaration.Name)));
                }
                shimExtern.ArgumentModules.push_back(*declaringModule);
            }
        }

        module.ShimExterns.push_back(std::move(shimExtern));
    }

    CookResult<std::string> linkSource =
        MakeShimLinkSource(module.ShimExterns, shimAccessModel, module.Name, targetName, *diagnosticSink);
    if (!linkSource)
    {
        return std::unexpected(linkSource.error());
    }
    
    CookResult<std::vector<ShimFormatLinkSource>> formatSources =
        MakeShimFormatLinkSources(module.ShimExterns, space, shimAccessModel, module.Name, *diagnosticSink);
    if (!formatSources)
    {
        return std::unexpected(formatSources.error());
    }

    shimLinkSource = std::move(linkSource.value());
    shimFormatSources = std::move(formatSources.value());
    return module;
}

SlangCompiler::CompileResultList SlangCompiler::Compile(const std::vector<VariantDescriptor>& variants, DiagnosticSink& sink) const
{
    return compilePool->Compile(variants, shimLinkSource, shimFormatSources, sink);
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

CookResult<std::vector<RawAxisDeclaration>> SlangCompiler::BuildAxisDeclarations() const
{
    return bootstrapContext->BuildDeclaredAxes();
}

const SymbolTable& SlangCompiler::GetSymbolTable() const noexcept
{
    return symbolTable;
}

} // namespace lodestone
