#include "SlangModuleContext.hpp"
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/SlangCompiler.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
} // namespace

namespace lodestone
{

CookError SlangModuleContext::Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink)
{
    diagnosticSink = &sink;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
    {
        return CookError::GlobalSessionCreationFailed;
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
            return CookError::FilesystemError;
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

} // namespace lodestone
