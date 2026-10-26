#include "SlangModuleContext.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "EmbeddedFileSystem.hpp"
#include "compile/EmbeddedBuiltinModules.hpp"
#include "compile/EnumTagDecode.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationValue.hpp"
#include "slang-com-ptr.h"
#include "slang.h"
#include "target/TargetProfile.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <iterator>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace lodestone
{

namespace
{
    std::string BuildCachedModulePath(std::string_view cache_directory, std::string_view module_name);

    /** Returns `<Slang build tag>-<configuration>`, with path-unsafe characters replaced. */
    std::string BuildCacheFolderName(std::string_view slang_build_tag);

    /** Writes through a temporary file and a rename: Slang reads the cache folder as a search path, so readers must
     * never see a partial file. */
    CookError WriteFileAtomically(const std::filesystem::path& path, std::span<const std::byte> bytes);

    // only `__include` fragments are walked into: their declarations are still at module scope
    void VisitModuleScopeDecls(slang::DeclReflection* reflection,
                               slang::DeclReflection::Kind kind,
                               const std::function<void(slang::DeclReflection*)>& visit);

    void VisitLoadedModuleScopeDecls(slang::ISession* session,
                                     slang::DeclReflection::Kind kind,
                                     const std::function<void(slang::DeclReflection*)>& visit);

    std::vector<std::string> BuildModuleScopeNames(slang::ISession* session, slang::DeclReflection::Kind kind);
}

constexpr bool k_UseSlangWorkaround = true;

CookError SlangModuleContext::Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink)
{
    diagnosticSink = &sink;
    bufferForm = create_info.Forms.Buffer;
    language = create_info.Language;
    compilesDeliveredForm = !AreBaseFlags(create_info.OptimizationLevel, create_info.DebugLevel);
    
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
        MakeCompilerOptions(create_info.OptimizationLevel, create_info.DebugLevel);

    const std::filesystem::path canonicalModulePath = std::filesystem::canonical(create_info.ModulePath);
    const std::string sourceDirectory = canonicalModulePath.parent_path().string();
    // shared modules sit one level above the per-stage directory
    const std::string sharedDirectory = canonicalModulePath.parent_path().parent_path().string();
    // one folder per Slang build and configuration: Slang rejects another IR version (E00131) or worse
    const std::filesystem::path cachePath =
        create_info.ModuleCacheDirectory / BuildCacheFolderName(globalSession->getBuildTagString());

    std::error_code cacheError;
    std::filesystem::create_directories(cachePath, cacheError);
    if (cacheError)
    {
        return ReportError(sink,
                           CookError::DirectoryCouldNotBeCreated,
                           std::format("cannot create the module cache folder '{}': {}",
                                       cachePath.string(),
                                       cacheError.message()));
    }
    cacheDirectory = cachePath.string();

    // The include directories come before the module cache. An import that only the cache can find is a module that an
    // earlier module of this cook wrote there: it loads in this session, and it cannot be serialized for the workers.
    std::vector<std::string> includePaths;
    includePaths.reserve(create_info.IncludeDirectories.size());
    for (const std::filesystem::path& includeDirectory : create_info.IncludeDirectories)
    {
        includePaths.push_back(std::filesystem::canonical(includeDirectory).string());
    }

    std::vector<const char*> searchPaths{ sourceDirectory.c_str(), sharedDirectory.c_str() };
    for (const std::string& includePath : includePaths)
    {
        searchPaths.push_back(includePath.c_str());
    }
    searchPaths.push_back(cacheDirectory.c_str());
    searchPaths.push_back(EmbeddedFileSystem::k_BuiltinSearchPath.data());

    slang::TargetDesc target{};
    target.format = ToSlangCompileTarget(create_info.Language);
    // with no index, the heap takes the next free set, which can differ between the variants of one module
    std::vector<slang::CompilerOptionEntry> deliveredOptions;
    if (create_info.DescriptorHeapSet.has_value())
    {
        slang::CompilerOptionEntry heapSet{};
        heapSet.name = slang::CompilerOptionName::BindlessSpaceIndex;
        heapSet.value.kind = slang::CompilerOptionValueKind::Int;
        heapSet.value.intValue0 = static_cast<int32_t>(*create_info.DescriptorHeapSet);
        deliveredOptions.push_back(heapSet);
    }

    if (!create_info.SlangProfileName.empty())
    {
        const std::string profileName(create_info.SlangProfileName);
        target.profile = globalSession->findProfile(profileName.c_str());
        if (target.profile == SLANG_PROFILE_UNKNOWN)
        {
            return ReportError(sink,
                               CookError::SessionCreationFailed,
                               std::format("Slang has no profile named '{}'", create_info.SlangProfileName));
        }
    }

    // a target's options override the session's, so the base target restates the base flags
    std::vector<slang::CompilerOptionEntry> baseOptions = deliveredOptions;
    if (compilesDeliveredForm)
    {
        baseOptions.append_range(MakeFlagOptions(k_BaseOptimizationLevel, k_BaseDebugLevel));
    }

    std::array<slang::TargetDesc, 2u> targets{ target, target };
    slang::TargetDesc& baseTarget = targets[static_cast<size_t>(k_BaseTargetIndex)];
    slang::TargetDesc& deliveredTarget = targets[static_cast<size_t>(k_DeliveredTargetIndex)];
    baseTarget.compilerOptionEntries = baseOptions.data();
    baseTarget.compilerOptionEntryCount = static_cast<uint32_t>(baseOptions.size());
    deliveredTarget.compilerOptionEntries = deliveredOptions.data();
    deliveredTarget.compilerOptionEntryCount = static_cast<uint32_t>(deliveredOptions.size());

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = targets.data();
    sessionDesc.targetCount = compilesDeliveredForm ? 2 : 1;
    sessionDesc.searchPaths = searchPaths.data();
    sessionDesc.searchPathCount = static_cast<SlangInt>(searchPaths.size());
    sessionDesc.fileSystem = &GetEmbeddedFileSystem();
    sessionDesc.compilerOptionEntries = compileOptions.data();
    sessionDesc.compilerOptionEntryCount = static_cast<uint32_t>(compileOptions.size());
    
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        return CookError::SessionCreationFailed;
    }

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


CookError SlangModuleContext::LoadShimLinkModule(std::string_view source)
{
    if (source.empty())
    {
        return CookError::Success;
    }

    // one module for the whole context: the implementations follow the profile, which never changes between variants
    const std::string name = std::format("LodestoneShimLinks_{}", moduleName);
    const std::string path = name + ".slang";
    const std::string sourceText{ source };
    Slang::ComPtr<slang::IBlob> diagnosticsBlob;
    shimLinkModule = session->loadModuleFromSourceString(name.c_str(), path.c_str(), sourceText.c_str(), diagnosticsBlob.writeRef());
    ReportDiagnostics(*diagnosticSink, "SlangModuleContext::LoadShimLinkModule", diagnosticsBlob);
    return shimLinkModule != nullptr ? CookError::Success : CookError::ModuleLoadFailed;
}

slang::IModule* SlangModuleContext::ShimLinkModule() const noexcept
{
    return shimLinkModule;
}

std::vector<std::string> SlangModuleContext::BuildModuleScopeVariableNames() const
{
    return BuildModuleScopeNames(session, slang::DeclReflection::Kind::Variable);
}

std::vector<std::string> SlangModuleContext::BuildModuleScopeStructNames() const
{
    return BuildModuleScopeNames(session, slang::DeclReflection::Kind::Struct);
}

std::vector<StructStringAttribute> SlangModuleContext::BuildStructStringAttributes(std::string_view attribute_name) const
{
    const std::string attributeName{ attribute_name };
    std::vector<StructStringAttribute> found;
    auto vistorFn = [&](slang::DeclReflection* decl)
    {
        slang::TypeReflection* type = decl->getType();
        const char* name = decl->getName();
        slang::UserAttribute* attribute =
            type != nullptr ? type->findUserAttributeByName(attributeName.c_str()) : nullptr;
        if (attribute == nullptr || name == nullptr)
        {
            return;
        }

        size_t length = 0u;
        const char* text = attribute->getArgumentValueString(0u, &length);
        found.push_back(
            StructStringAttribute
            {
            .StructName = name,
            .Argument = text != nullptr ? std::string{ text, length } : std::string{}
            });
    };

    VisitLoadedModuleScopeDecls(session,
                                slang::DeclReflection::Kind::Struct,
                                vistorFn);

    return found;
}

CookError SlangModuleContext::LoadShimFormatModules(std::span<const ShimFormatLinkSource> sources)
{
    for (const ShimFormatLinkSource& source : sources)
    {
        const std::string name = std::format("LodestoneShimLinks_{}_{}_{}", moduleName, source.AxisName, source.CaseOrdinal);
        const std::string path = name + ".slang";
        Slang::ComPtr<slang::IBlob> diagnosticsBlob;
        slang::IModule* module =
            session->loadModuleFromSourceString(name.c_str(), path.c_str(), source.Source.c_str(), diagnosticsBlob.writeRef());
        ReportDiagnostics(*diagnosticSink, "SlangModuleContext::LoadShimFormatModules", diagnosticsBlob);
        if (module == nullptr)
        {
            return CookError::ModuleLoadFailed;
        }

        shimFormatModules.push_back(ShimFormatModule{ .AxisName = source.AxisName, .CaseOrdinal = source.CaseOrdinal, .Module = module });
    }
    return CookError::Success;
}

slang::IModule* SlangModuleContext::ShimFormatLinkModule(std::string_view axis_name, uint32_t case_ordinal) const noexcept
{
    const auto found = std::ranges::find_if(shimFormatModules,
                                            [&](const ShimFormatModule& module)
                                            { return module.CaseOrdinal == case_ordinal && module.AxisName == axis_name; });
    return found != shimFormatModules.end() ? found->Module : nullptr;
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

const std::vector<std::string>& SlangModuleContext::ModuleSourcePaths() const noexcept
{
    return moduleSourcePaths;
}

const std::vector<std::string>& SlangModuleContext::DependencyFilePaths() const noexcept
{
    return dependencyFilePaths;
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

AccessModel SlangModuleContext::BufferForm() const noexcept
{
    return bufferForm;
}

TargetLanguage SlangModuleContext::Language() const noexcept
{
    return language;
}

bool SlangModuleContext::CompilesDeliveredForm() const noexcept
{
    return compilesDeliveredForm;
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

CookError WriteModuleCache(std::span<const SerializedModule> serialized_modules, DiagnosticSink& sink)
{
    CookError result = CookError::Success;
    for (const SerializedModule& serializedModule : serialized_modules)
    {
        if (serializedModule.Blob == nullptr)
        {
            continue;
        }

        const std::span<const std::byte> bytes(static_cast<const std::byte*>(serializedModule.Blob->getBufferPointer()),
                                               serializedModule.Blob->getBufferSize());
        if (WriteFileAtomically(serializedModule.Path, bytes) != CookError::Success)
        {
            ReportWarning(sink,
                          std::format("cannot replace the module cache file '{}', which another cook may be reading. "
                                      "The old file stays, and the next cook may recompile this module",
                                      serializedModule.Path));
            result = CookError::FileWriteFailed;
        }
    }
    return result;
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
            static_cast<void>(ReportError(*diagnosticSink,
                                          CookError::ModuleLoadFailed,
                        std::format("a worker cannot load the module '{}'. Slang gives no reason. If the module was found "
                                    "only in the module cache, pass the folder of its source with --include-dir",
                                    serializedModule.Name)));
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
    moduleSourcePaths.reserve(static_cast<size_t>(dependencyCount));

    for (SlangInt32 i = 0; i < dependencyCount; ++i)
    {
        const char* dependencyPath = rootModule->getDependencyFilePath(i);
        if (dependencyPath == nullptr)
        {
            continue;
        }

        const EmbeddedBuiltinModule* builtin = FindEmbeddedBuiltin(dependencyPath);
        if (builtin != nullptr)
        {
            //NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            moduleSourceStrings.emplace_back(reinterpret_cast<const char*>(builtin->Source), builtin->SourceSize);
            moduleSourcePaths.emplace_back(dependencyPath);
            continue;
        }

        std::ifstream file(dependencyPath, std::ios::binary);
        if (!file)
        {
            return ReportError(*diagnosticSink,
                               CookError::FailedToLoadModuleDependencySource,
                               std::format("module {} depends on '{}', and that file cannot be read",
                                           moduleName,
                                           dependencyPath));
        }

        moduleSourceStrings.emplace_back(std::istreambuf_iterator<char>(file),
                                         std::istreambuf_iterator<char>());
        moduleSourcePaths.emplace_back(dependencyPath);
        dependencyFilePaths.emplace_back(dependencyPath);
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

    std::string BuildCacheFolderName(std::string_view slang_build_tag)
    {
        std::string name = std::format("{}-{}", slang_build_tag, LODESTONE_BUILD_CONFIG);
        for (char& character : name)
        {
            const bool isAlNum = std::isalnum(static_cast<unsigned char>(character)) != 0;
            const bool allowed = isAlNum ||
                                 character == '.' ||
                                 character == '-';
            if (!allowed)
            {
                character = '_';
            }
        }
        return name;
    }

    CookError WriteFileAtomically(const std::filesystem::path& path, std::span<const std::byte> bytes)
    {
        std::random_device random;
        std::filesystem::path temporaryPath = path;
        temporaryPath += std::format(".{:08x}{:08x}.tmp", random(), random());
        {
            std::ofstream stream(temporaryPath, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream)
            {
                std::error_code ignored;
                std::filesystem::remove(temporaryPath, ignored);
                return CookError::FileWriteFailed;
            }
        }

        std::error_code renameError;
        std::filesystem::rename(temporaryPath, path, renameError);
        if (renameError)
        {
            // on Windows an open reader blocks the replace, which costs one cache miss
            std::error_code ignored;
            std::filesystem::remove(temporaryPath, ignored);
            return CookError::FileWriteFailed;
        }
        return CookError::Success;
    }

    // NOLINTBEGIN(misc-no-recursion)
    void VisitModuleScopeDecls(slang::DeclReflection* reflection,
                               slang::DeclReflection::Kind kind,
                               const std::function<void(slang::DeclReflection*)>& visit)
    {
        const unsigned int childCount = reflection->getChildrenCount();
        for (unsigned int i = 0u; i < childCount; ++i)
        {
            slang::DeclReflection* child = reflection->getChild(i);
            if (child == nullptr)
            {
                continue;
            }

            if (child->getKind() == kind)
            {
                visit(child);
            }
            else if (child->getKind() == slang::DeclReflection::Kind::Unsupported && child->getChildrenCount() > 0u)
            {
                VisitModuleScopeDecls(child, kind, visit);
            }
        }
    }
    // NOLINTEND(misc-no-recursion)

    void VisitLoadedModuleScopeDecls(slang::ISession* session,
                                     slang::DeclReflection::Kind kind,
                                     const std::function<void(slang::DeclReflection*)>& visit)
    {
        const int64_t loadedModuleCount = static_cast<int64_t>(session->getLoadedModuleCount());
        for (int64_t i = 0; i < loadedModuleCount; ++i)
        {
            slang::DeclReflection* moduleReflection = session->getLoadedModule(i)->getModuleReflection();
            if (moduleReflection != nullptr)
            {
                VisitModuleScopeDecls(moduleReflection, kind, visit);
            }
        }
    }

    std::vector<std::string> BuildModuleScopeNames(slang::ISession* session, slang::DeclReflection::Kind kind)
    {
        std::vector<std::string> names;

        auto scopeNameVisitor = [&](slang::DeclReflection* decl)
        {
            const char* name = decl->getName();
            if (name != nullptr)
            {
                names.emplace_back(name);
            }
        };

        VisitLoadedModuleScopeDecls(session,
                                    kind,
                                    scopeNameVisitor);

        std::ranges::sort(names);
        const auto [duplicatesBegin, duplicatesEnd] = std::ranges::unique(names);
        names.erase(duplicatesBegin, duplicatesEnd);
        return names;
    }
}

} // namespace lodestone
