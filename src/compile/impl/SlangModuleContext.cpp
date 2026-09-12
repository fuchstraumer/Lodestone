#include "SlangModuleContext.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <algorithm>
#include <array>
#include <cassert>
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
#include <tuple>
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
            const char* moduleNamePtr = moduleReflection->getName();
            const std::string_view moduleNameSv =
                moduleNamePtr != nullptr ? std::string_view(moduleNamePtr) : std::string_view();
            const CookError collected = collectAxesFromDecl(moduleReflection, moduleNameSv);
            if (!collected)
            {
                return std::unexpected(collected);
            }
        }
    }

    const CookError interfacesBuilt = buildInterfaceAxes();
    if (!interfacesBuilt)
    {
        return std::unexpected(interfacesBuilt);
    }

    return axisDeclarations;
}

//NOLINTBEGIN(misc-no-recursion)
CookError SlangModuleContext::collectAxesFromDecl(slang::DeclReflection* reflection, std::string_view module_name)
{
    const unsigned int childCount = reflection->getChildrenCount();
    for (unsigned int j = 0u; j < childCount; ++j)
    {
        slang::DeclReflection* child = reflection->getChild(j);
        if (child == nullptr)
        {
            continue;
        }

        if (child->getKind() == slang::DeclReflection::Kind::Variable)
        {
            // A variable is an axis candidate. buildAxisDecl returns nullopt for a variable that
            // carries no axis attribute, which is the common case, so a nullopt is skipped and never
            // an error.
            CookResult<std::optional<RawAxisDeclaration>> axisDeclResult = buildAxisDecl(child);
            if (!axisDeclResult)
            {
                return axisDeclResult.error();
            }
            if (axisDeclResult->has_value())
            {
                axisDeclarations.emplace_back(std::move(**axisDeclResult));
            }
        }
        else if (child->getKind() == slang::DeclReflection::Kind::Struct)
        {   
            // ls_axis_interface or ls_axis_interface_impl values
            const CookError axisDataStaged = stageInterfaceStruct(reflection, module_name);
            if (!axisDataStaged)
            {
                return axisDataStaged;
            }
        }
        else if (child->getChildrenCount() > 0u)
        {
            // A `__include`/`implementing` fragment reflects as an Unsupported node whose children are
            // the real declarations. `getChild`/`getChildrenCount` are safe on such a node, so descend.
            const CookError nested = collectAxesFromDecl(child, module_name);
            if (!nested)
            {
                return nested;
            }
        }
    }

    return CookError::Success;
}
//NOLINTEND(misc-no-recursion)

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
    slang::Attribute* activeWhenAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_axis_active_when");
    if (activeWhenAttr != nullptr)
    {
        CookResult<std::string> activeWhenResult =
            extractSingleAttribute(reflection, activeWhenAttr, "ls_axis_active_when");
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

    // Get the source location to carry along for later validation and reporting (a later failure to
    // parse one of the expression strings happens in a layer with no visibility into Slang types, so
    // it needs this to point at the line). This is best-effort, not required: a declaration reached
    // through an `__include`d fragment reflects without a usable source location, and Slang returns a
    // failure here. That must not sink the axis -- an axis with no location is still a valid axis, it
    // just cannot name its own line in a later diagnostic. Leave the location unset and continue.
    slang::SourceLocation sourceLoc;
    if (SLANG_SUCCEEDED(session->getDeclSourceLocation(reflection, &sourceLoc)))
    {
        result.SourceFile = sourceLoc.filePath != nullptr ? sourceLoc.filePath : "<unknown>";
        result.SourceLine = static_cast<int32_t>(sourceLoc.line);
        result.SourceColumn = static_cast<int32_t>(sourceLoc.column);
    }
    else
    {
        result.SourceFile = "<unknown>";
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

CookError SlangModuleContext::stageInterfaceStruct(slang::DeclReflection* reflection, std::string_view module_name)
{
    // this is gonna be a doozy
    slang::TypeReflection* type = reflection->getType();
    if (type == nullptr)
    {
        return CookError::Success;
    }

    slang::UserAttribute* axisAttr = type->findUserAttributeByName("ls_axis_interface");
    slang::UserAttribute* axisImplAttr = type->findUserAttributeByName("ls_axis_interface_impl");
    if ((axisAttr == nullptr) && (axisImplAttr == nullptr))
    {
        // just an ordinary struct, not one of our decorated ones
        return CookError::Success;
    }

    const char* declNamePtr = reflection->getName();
    const std::string declName = declNamePtr != nullptr ? std::string{ declNamePtr } : "<unknown>";
    if ((axisAttr != nullptr) && (axisImplAttr != nullptr))
    {
        // can't have *both* of these attributes on a struct
        const std::string errStr =
            std::format("Struct {} cannot have both ls_axis_interface and ls_axis_interface_impl attributes",
                        declName);
        return ReportError(*diagnosticSink,
                           CookError::SlangInvalidAttributesOnDecl,
                           errStr);
    }

    if (axisAttr != nullptr)
    {
        InterfaceAxisStub stub;
        stub.Name = declName;
        stub.Type = type;
        slang::SourceLocation sourceLoc;
        if (SLANG_SUCCEEDED(session->getDeclSourceLocation(reflection, &sourceLoc)) &&
            (sourceLoc.filePath != nullptr))
        {
            stub.SourceFile = sourceLoc.filePath;
            stub.SourceLine = static_cast<int32_t>(sourceLoc.line);
            stub.SourceColumn = static_cast<int32_t>(sourceLoc.column);
        }
        interfaceAxisStubs.emplace_back(std::move(stub));
        return CookError::Success;
    }

    // we'll need to use a blob to get the full name
    Slang::ComPtr<slang::IBlob> fullNameBlob;
    type->getFullName(fullNameBlob.writeRef());
    std::string typeName;
    if (fullNameBlob != nullptr)
    {
        typeName = std::string{ static_cast<const char*>(fullNameBlob->getBufferPointer()),
                                fullNameBlob->getBufferSize() };
    }
    else
    {
        typeName = declName; // fallback to the declaration name if full name is not available
    }

    size_t interfaceNameLength = 0u;
    const char* interfaceName = axisImplAttr->getArgumentValueString(0u, &interfaceNameLength);
    if (interfaceName == nullptr)
    {
        const std::string errStr =
            std::format("ls_axis_interface_impl on '{}' has no interface name", typeName);
        return ReportError(*diagnosticSink,
                           CookError::SlangInvalidAttributesOnDecl,
                           errStr);
    }

    const CookError resourceCheck = rejectResourceMembers(type, typeName);
    if (!resourceCheck)
    {
        return resourceCheck;
    }

    InterfaceAxisImplStub stub;
    stub.InterfaceName.assign(interfaceName, interfaceNameLength);
    stub.Type = type;
    stub.Impl.Module = std::string(module_name);
    stub.Impl.TypeName = std::move(typeName);
    interfaceAxisImplStubs.emplace_back(std::move(stub));
    return CookError::Success;
}

CookError SlangModuleContext::buildInterfaceAxes()
{
    if (interfaceAxisStubs.empty())
    {
        return CookError::Success;
    }

    Slang::ComPtr<slang::IBlob> diagBlob;
    slang::ProgramLayout* programLayout = rootModule->getLayout(0, diagBlob.writeRef());
    if (programLayout == nullptr)
    {
        const std::string errStr =
            std::format("Failed to get program layout from rootModule for interface axis resolution");
        return ReportError(*diagnosticSink,
                           CookError::SlangProgramLayoutNotFound,
                           errStr);
    }

    for (const InterfaceAxisStub& stub : interfaceAxisStubs)
    {
        // match each extern to the one exact interface it actaully conforms to
        std::string matchedInterfaceName;

        // validate there is at least one matching interface, and that there's not multiple conformity
        // we'll have to traverse this list again later, but that shouldn't be too costly
        for (const InterfaceAxisImplStub& implStub : interfaceAxisImplStubs)
        {
            slang::TypeReflection* interfaceType = programLayout->findTypeByName(implStub.InterfaceName.c_str());
            // now check to see if implStub is a subtype of the outer type
            if (interfaceType != nullptr && programLayout->isSubType(stub.Type, interfaceType))
            {
                if (matchedInterfaceName.empty())
                {
                    matchedInterfaceName = implStub.InterfaceName;
                }
                else if (matchedInterfaceName != implStub.InterfaceName)
                {
                    // can't conform to more than one interface
                    const std::string errStr = std::format("Type '{}' conforms to multiple interfaces: '{}' and '{}'",
                                                           stub.Name,
                                                           matchedInterfaceName,
                                                           implStub.InterfaceName);
                    return ReportError(*diagnosticSink,
                                       CookError::SlangMultipleConformingInterfaces,
                                       errStr);
                }
            }
        }

        if (matchedInterfaceName.empty())
        {
            const std::string errStr = std::format("Interface Axis {} has no tagged and conforming impls", stub.Name);
            return ReportError(*diagnosticSink,
                               CookError::SlangNoConformingInterfaces,
                               errStr);
        }

        RawAxisDeclaration axisDecl
        {
            .Name = stub.Name,
            .IsBooleanAxis = false,
            .IsInterfaceAxis = true,
            .AxisValues = {},
            .ActiveWhen = {}, // todo: fill this in if present
            .Kind = {}, // todo: also fill this in if present
            .SourceFile = stub.SourceFile.empty() ? "<unknown>" : stub.SourceFile,
            .SourceLine = stub.SourceLine,
            .SourceColumn = stub.SourceColumn
        };

        slang::TypeReflection* interfaceType = programLayout->findTypeByName(matchedInterfaceName.c_str());
        for (const InterfaceAxisImplStub& implStub : interfaceAxisImplStubs)
        {
            // now add the conforming implementations to the axis declaration, since we've confirmed this 
            // interface axis has at least one conforming implementation and is otherwise valid
            if (implStub.InterfaceName != matchedInterfaceName)
            {
                continue;
            }

            if (!programLayout->isSubType(implStub.Type, interfaceType))
            {
                const std::string warningStr = std::format("Type '{}' does not conform to interface '{}'",
                                                           implStub.Type->getName(),
                                                           matchedInterfaceName);
                ReportWarning(*diagnosticSink, warningStr);
                continue;
            }

            axisDecl.InterfaceImpls.emplace_back(implStub.Impl);
        }

        assert(!axisDecl.InterfaceImpls.empty());

        auto sortRawInterfaceImpl = [](const RawInterfaceImpl& lhs, const RawInterfaceImpl& rhs)
        {
            return std::tie(lhs.Module, lhs.TypeName) < std::tie(rhs.Module, rhs.TypeName);
        };

        std::ranges::sort(axisDecl.InterfaceImpls, sortRawInterfaceImpl);
    }

    interfaceAxisStubs.clear();
    interfaceAxisStubs.shrink_to_fit();
    interfaceAxisImplStubs.clear();
    interfaceAxisImplStubs.shrink_to_fit();
    return CookError::Success;
}

CookError SlangModuleContext::rejectResourceMembers(slang::TypeReflection* type, std::string_view type_name)
{
    // we have to reject interface types that declare resource members, as that's not valid with this 
    // model for link-time specialization (resource binding layout is made concrete before link-time)
    const uint32_t memberCount = static_cast<uint32_t>(type->getFieldCount());
    for (uint32_t i = 0; i < memberCount; ++i)
    {
        slang::VariableReflection* field = type->getFieldByIndex(i);
        slang::TypeReflection* fieldType = field != nullptr ? field->getType() : nullptr;
        if (fieldType == nullptr)
        {
            continue;
        }

        if (IsResourceTypeKind(fieldType->getKind()))
        {
            const char* fieldName = field->getName();
            const std::string_view fieldNameView = fieldName != nullptr ? fieldName : "<field>";
            const std::string errStr = std::format("Interface type '{}' declares a resource member '{}', which is not allowed",
                                                   type_name,
                                                   fieldNameView);
            return ReportError(*diagnosticSink, CookError::SlangInterfaceHasResourceMember, errStr);
        }
    }
    return CookError::Success;
}

} // namespace lodestone
