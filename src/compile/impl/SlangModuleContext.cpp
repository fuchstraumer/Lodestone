#include "SlangModuleContext.hpp"
#include "SlangCompilerTypes.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{
    std::string BuildCachedModulePath(std::string_view cache_directory, std::string_view module_name)
    {
        std::string result{ cache_directory };
        result += "/";
        result += module_name;
        result += ".slang-module";
        return result;
    }

}

constexpr bool k_UseSlangWorkaround = true;

CookError SlangModuleContext::Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink)
{
    diagnosticSink = &sink;
    placementKind = create_info.AccessModel;
    if (k_UseSlangWorkaround)
    {
        const SlangResult created = slang_createGlobalSessionWithoutCoreModule(SLANG_API_VERSION, globalSession.writeRef());
        if (SLANG_FAILED(created))
        {
            return CookError::GlobalSessionCreationFailed;
        }

        if (ISlangBlob* coreModuleBlob = slang_getEmbeddedCoreModule())
        {
            const SlangResult loaded = globalSession->loadCoreModule(coreModuleBlob->getBufferPointer(), coreModuleBlob->getBufferSize());
            if (SLANG_FAILED(loaded))
            {
                return CookError::SlangCoreModuleLoadFailed;
            }
        }
        else
        {
            // attempt to build core module
            const SlangResult built = globalSession->compileCoreModule(0);
            if (SLANG_FAILED(built))
            {
                return CookError::SlangCoreModuleBuildFailed;
            }
        }
    }
    else
    {
        if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
        {   
            return CookError::GlobalSessionCreationFailed;
        }
    }

    const std::vector<slang::CompilerOptionEntry> compileOptions =
        MakeCompilerOptions(create_info.OptimizationLevel);

    // todo-asap: I am inserting the tests/assets/ rootdir here for the attributes file. This needs to be
    // optionalized and standardized
    const std::filesystem::path attributesPath =
        std::filesystem::canonical("C:/SoftwareDev/Lodestone/tests/assets/");
    const std::string attributesPathStr = attributesPath.string();
    const std::filesystem::path canonicalModulePath = std::filesystem::canonical(create_info.ModulePath);
    const std::string sourceDirectory = canonicalModulePath.parent_path().string();
    // The shared modules a shader imports -- VeloxAttributes among them -- sit one level above the
    // per-stage directory, so the asset root resolves without a command-line switch.
    const std::string sharedDirectory = canonicalModulePath.parent_path().parent_path().string();
    cacheDirectory = create_info.ModuleCacheDirectory.string();
    const std::array<const char*, 4> searchPaths{
        sourceDirectory.c_str(), sharedDirectory.c_str(), cacheDirectory.c_str(), attributesPathStr.c_str()
    };

    slang::TargetDesc target{};
    // todo-ship: target output format needs to from compile options, and should be
    // able to be made into multiple targets. this will require changes to reflection
    // though, so it's a larger job than just the profile opt below
    target.format = SLANG_WGSL;
    // todo-ship: profile should also be a selectable option
    target.profile = globalSession->findProfile("spirv_1_4");
    // each job will create their own session: but, global session will be shared
    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
    sessionDesc.compilerOptionEntries = compileOptions.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(compileOptions.size());
    
    // create session attached to each threads global session
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        return CookError::SessionCreationFailed;
    }

    // extract module name from module path
    moduleName = canonicalModulePath.stem().string();

    return CookError::Success;
}

CookError SlangModuleContext::RunBootstrap()
{
    CookError bootstrapResult = loadRootModule();
    if (bootstrapResult != CookError::Success)
    {
        return bootstrapResult;
    }

    bootstrapResult = readDependencySourceStrings();
    if (bootstrapResult != CookError::Success)
    {
        return bootstrapResult;
    }

    bootstrapResult = enumerateEntryPoints();
    if (bootstrapResult != CookError::Success)
    {
        return bootstrapResult;
    }

    bootstrapResult = buildSlangComponents();
    if (bootstrapResult != CookError::Success)
    {
        return bootstrapResult;
    }

    return bootstrapResult;
}

CookResult<std::span<const RawAxisDeclaration>> SlangModuleContext::ReadDeclaredAxes()
{
    if (!axisDeclarations.empty())
    {
        return axisDeclarations;
    }

    const int64_t loadedModuleCount = static_cast<int64_t>(session->getLoadedModuleCount());
    for (int64_t i = 0; i < loadedModuleCount; ++i)
    {
        slang::IModule* module = session->getLoadedModule(i);
        slang::DeclReflection* moduleReflection = module->getModuleReflection();
        if (moduleReflection != nullptr)
        {
            const int64_t childCount = static_cast<int64_t>(moduleReflection->getChildrenCount());
            for (int64_t j = 0; j < childCount; ++j)
            {
                slang::DeclReflection* child = moduleReflection->getChild(static_cast<unsigned int>(j));
                if ((child != nullptr) && child->getKind() == slang::DeclReflection::Kind::Variable)
                {
                    // this is ugly, because we need to both bubble up errors from source code (so CookResult<T>),
                    // but also there are going to be variables that just aren't axes... so we need std::optional<>
                    // to succinctly return values that aren't invalid, but aren't an axis either. sorry its ugly :(
                    //NOLINTBEGIN(readability-else-after-return)
                    auto axisDeclResult = buildAxisDecl(child);
                    if (!axisDeclResult)
                    {
                        return std::unexpected(axisDeclResult.error());
                    }
                    else if (axisDeclResult->has_value() && (*axisDeclResult == std::nullopt))
                    {
                        continue;
                    }
                    else
                    {
                        axisDeclarations.emplace_back(std::move(*axisDeclResult.value()));
                    }
                    //NOLINTEND(readability-else-after-return)
                }
            }
        }
    }
    
    return axisDeclarations;
}

slang::IGlobalSession* SlangModuleContext::GlobalSession() const noexcept
{
    return globalSession;
}

slang::ISession* SlangModuleContext::Session() const noexcept
{
    return session;
}

std::vector<slang::IComponentType*> SlangModuleContext::BaseComponents() const noexcept
{
    return baseComponents;
}

size_t SlangModuleContext::EntryPointCount() const noexcept
{
    return entryPointNames.size();
}

const std::vector<std::string>& SlangModuleContext::EntryPointNames() const noexcept
{
    return entryPointNames;
}

std::string_view SlangModuleContext::ModuleName() const noexcept
{
    return moduleName;
}

const std::vector<std::string>& SlangModuleContext::ModuleSourceStrings() const noexcept
{
    return moduleSourceStrings;
}

std::vector<std::string_view> SlangModuleContext::ModuleSourceStringViews() const noexcept
{
    return moduleSourceStrings |
           std::views::transform(
               [](const std::string& str)
               {
                   return std::string_view(str);
               }) |
           std::ranges::to<std::vector<std::string_view>>();
}

PlacementKind SlangModuleContext::PlacementKindForTarget() const noexcept
{
    return placementKind;
}

std::vector<SerializedModule> SlangModuleContext::SerializeModules() const
{
    const auto moduleCount = static_cast<int64_t>(session->getLoadedModuleCount());
    std::vector<SerializedModule> serializedModules;
    serializedModules.reserve(static_cast<size_t>(moduleCount));
    for (int64_t i = 0; i < moduleCount; ++i)
    {
        Slang::ComPtr<slang::IBlob> blob;
        slang::IModule* module = session->getLoadedModule(i);
        const SlangResult result = module->serialize(blob.writeRef());
        const char* moduleNameStr = module->getName();
        const std::string modulePath = BuildCachedModulePath(cacheDirectory, moduleNameStr);
        serializedModules.emplace_back(moduleNameStr, modulePath, result < 0 ? nullptr : blob);
    }
    return serializedModules;
}

CookError SlangModuleContext::WriteModuleCache() const
{
    const auto moduleCount = static_cast<int64_t>(session->getLoadedModuleCount());
    for (int64_t i = 0; i < moduleCount; ++i)
    {
        slang::IModule* module = session->getLoadedModule(i);
        const char* moduleNameStr = module->getName();
        const std::string modulePath = BuildCachedModulePath(cacheDirectory, moduleNameStr);
        if (SLANG_FAILED(module->writeToFile(modulePath.c_str())))
        {
            return CookError::FileWriteFailed;
        }
    }

    return CookError::Success;
}

CookError SlangModuleContext::RunWorkerSetup(std::span<const SerializedModule> serialized_modules)
{
    CookError error = primeSessionFromCache(serialized_modules);
    if (error != CookError::Success)
    {
        return error;
    }

    error = loadRootModule();
    if (error != CookError::Success)
    {
        return error;
    }

    error = enumerateEntryPoints();
    if (error != CookError::Success)
    {
        return error;
    }

    error = buildSlangComponents();
    if (error != CookError::Success)
    {
        return error;
    }

    return error;
}

CookError SlangModuleContext::primeSessionFromCache(std::span<const SerializedModule> serialized_modules)
{
    for (const SerializedModule& serializedModule : serialized_modules)
    {
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        const slang::IModule* module = session->loadModuleFromIRBlob(serializedModule.Name.c_str(),
                                                                     serializedModule.Path.c_str(),
                                                                     serializedModule.Blob,
                                                                     diagnosticsBlob.writeRef());

        if (module == nullptr)
        {
            ReportDiagnostics(*diagnosticSink, "SlangModuleContext::loadSerializedModules", diagnosticsBlob);
            return CookError::ModuleLoadFailed;
        }
    }

    return CookError::Success;
}

CookError SlangModuleContext::loadRootModule()
{
    Slang::ComPtr<slang::IBlob> diagnosticsBlob;
    rootModule = session->loadModule(moduleName.c_str(), diagnosticsBlob.writeRef());
    if (rootModule == nullptr)
    {
        ReportDiagnostics(*diagnosticSink, "SlangModuleContext::loadRootModule", diagnosticsBlob);
        return CookError::ModuleLoadFailed;
    }
    return CookError::Success;
}


CookError SlangModuleContext::readDependencySourceStrings()
{
    const SlangInt32 dependencyCount = rootModule->getDependencyFileCount();
    moduleSourceStrings.reserve(static_cast<size_t>(dependencyCount));

    for (SlangInt32 i = 0; i < dependencyCount; ++i)
    {
        const char* dependencyPath = rootModule->getDependencyFilePath(i);
        if (dependencyPath == nullptr)
        {
            continue;
        }

        std::ifstream file(dependencyPath, std::ios::binary);
        if (!file)
        {
            return CookError::FailedToLoadModuleDependencySource;
        }

        moduleSourceStrings.emplace_back(std::istreambuf_iterator<char>(file),
                                         std::istreambuf_iterator<char>());
    }

    return CookError::Success;
}

CookError SlangModuleContext::enumerateEntryPoints()
{
    const SlangInt32 entryPointCount = rootModule->getDefinedEntryPointCount();
    entryPointNames.reserve(static_cast<size_t>(entryPointCount));

    for (SlangInt32 i = 0; i < entryPointCount; ++i)
    {
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        if (SLANG_FAILED(rootModule->getDefinedEntryPoint(i, entryPoint.writeRef())))
        {
            return CookError::EntryPointEnumerationFailed;
        }

        entryPointNames.emplace_back(entryPoint->getFunctionReflection()->getName());
        entryPoints.push_back(entryPoint);
    }

    return CookError::Success;
}

CookError SlangModuleContext::buildSlangComponents()
{
    baseComponents.reserve(8u + static_cast<size_t>(rootModule->getDefinedEntryPointCount()));
    baseComponents.push_back(rootModule);
    for (const auto& entryPoint : entryPoints)
    {
        baseComponents.push_back(entryPoint.get());
    }

    return CookError::Success;
}

CookResult<std::optional<RawAxisDeclaration>> SlangModuleContext::buildAxisDecl(slang::DeclReflection* reflection)
{
    slang::VariableReflection* variableReflection = reflection->asVariable();
    if (variableReflection == nullptr)
    {
        return std::unexpected(CookError::AttributeExpressionParseFailed);
    }

    slang::Attribute* booleanAxisAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_boolean_axis");
    slang::Attribute* valuesAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_axis_values");
    // early out: if neither attribute is present, this is not an axis declaration
    if ((booleanAxisAttr == nullptr) && (valuesAttr == nullptr))
    {
        return std::nullopt;
    }

    // another early out: if there isn't a valid name, we can't reference this by name either!
    // I think this is more likely to be an outright error if the attributes failed though, so
    // we don't return nullopt here
    const char* variableName = variableReflection->getName();
    if (variableName == nullptr)
    {
        return std::unexpected(CookError::SlangGetVariableNameFailed);
    }

    RawAxisDeclaration result{};
    if (booleanAxisAttr != nullptr)
    {
        result.IsBooleanAxis = true;
    }

    result.Name = variableName;
    if (valuesAttr != nullptr)
    {
        // can't have both a values attribute and a boolean axis: the two are mutually exclusive
        if (result.IsBooleanAxis)
        {
            const std::string errStr =
                std::format("Variable '{}' cannot have both a boolean axis and axis values", variableName);
            return std::unexpected(ReportError(*diagnosticSink,
                                                    CookError::AttributeExpressionParseFailed,
                                                    errStr));
        }

        CookResult<std::string> valuesResult =
            extractSingleAttribute(reflection, valuesAttr, "ls_axis_values");
        if (!valuesResult.has_value())
        {
            return std::unexpected(valuesResult.error());
        }

        result.AxisValues = std::move(*valuesResult);
    }

    // ActiveWhen, totally optional
    slang::Attribute* activeWhenAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_active_when");
    if (activeWhenAttr != nullptr)
    {
        CookResult<std::string> activeWhenResult =
            extractSingleAttribute(reflection, activeWhenAttr, "ls_active_when");
        if (!activeWhenResult.has_value())
        {
            return std::unexpected(activeWhenResult.error());
        }

        result.ActiveWhen = std::move(*activeWhenResult);
    }

    // Kind, also optional 
    slang::Attribute* kindAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_axis_kind");
    if (kindAttr != nullptr)
    {
        CookResult<std::string> kindResult =
            extractSingleAttribute(reflection, kindAttr, "ls_axis_kind");
        if (!kindResult.has_value())
        {
            return std::unexpected(kindResult.error());
        }

        result.Kind = std::move(*kindResult);
    }

    // even in case of success, get source location to have around for validation and reporting later 
    // (in case we have later failures in parsing the expression strings, which happens at a layer
    // intentionally without any visibility into slang types!)
    slang::SourceLocation sourceLoc;
    if (SLANG_SUCCEEDED(session->getDeclSourceLocation(reflection, &sourceLoc)))
    {
        result.SourceFile = sourceLoc.filePath != nullptr ? sourceLoc.filePath : "<unknown>";
        result.SourceLine = static_cast<int32_t>(sourceLoc.line);
        result.SourceColumn = static_cast<int32_t>(sourceLoc.column);
    }
    else
    {
        const std::string errStr = std::format("Failed to get source location for axis declaration {}", result.Name);
        return std::unexpected(ReportError(*diagnosticSink,
                                           CookError::SlangGetSourceLocationFailed,
                                           errStr));
    }

    return result;
}

CookResult<std::string> SlangModuleContext::extractSingleAttribute(slang::DeclReflection* reflection,
                                                                   slang::Attribute* attribute,
                                                                   std::string_view attr_name)
{
    size_t length = 0u;
    const char* text = attribute->getArgumentValueString(0u, &length);

    if (text == nullptr)
    {
        slang::SourceLocation sourceLoc;
        std::string errStr = std::format("Failed to read value for attribute {}", attr_name);
        if (SLANG_SUCCEEDED(session->getDeclSourceLocation(reflection, &sourceLoc)))
        {
            const std::string sourceLocStr =
                std::format("{} L{}:C{}", sourceLoc.filePath, sourceLoc.line, sourceLoc.column);
            errStr += " at " + sourceLocStr;
        }
        return std::unexpected(ReportError(*diagnosticSink,
                                           CookError::SlangGetAttributeValueStrFailed,
                                           errStr));
    }

    return std::string(text, length);
}

} // namespace lodestone
