#include "SlangModuleContext.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/EnumTagDecode.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "permute/PermutationValue.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

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

    // Map the enum's Slang scalar type to the Slang-free tag kind. This is the wall: it names the
    // Slang type, and `DecodeEnumTag` (in compile/EnumTagDecode.hpp) does the read. An enum tag is
    // always an integer scalar, so the default is a can't-happen.
    EnumTagKind ScalarTypeToEnumTagKind(slang::TypeReflection::ScalarType scalar)
    {
        using slang::TypeReflection;
        switch (scalar)
        {
        case TypeReflection::ScalarType::Int8:
            return EnumTagKind::Int8;
        case TypeReflection::ScalarType::UInt8:
            return EnumTagKind::UInt8;
        case TypeReflection::ScalarType::Int16:
            return EnumTagKind::Int16;
        case TypeReflection::ScalarType::UInt16:
            return EnumTagKind::UInt16;
        case TypeReflection::ScalarType::Int32:
            return EnumTagKind::Int32;
        case TypeReflection::ScalarType::UInt32:
            return EnumTagKind::UInt32;
        case TypeReflection::ScalarType::Int64:
            return EnumTagKind::Int64;
        case TypeReflection::ScalarType::UInt64:
            return EnumTagKind::UInt64;
        default:
            assert(false && "Unsupported scalar type for enum case blob");
            return EnumTagKind::Int32;
        }
    }

    // Pick a tag kind from the case value blob's width, for when Slang will not report the enum's
    // scalar type. An enum an axis reaches through an `import` reflects with `ScalarType::None`
    // (measured), but its case value blob still carries the tag at its real width. Slang enums default
    // to a signed `int`, so assume signed here. The same-module path uses the real scalar type and
    // never reaches this fallback.
    EnumTagKind EnumTagKindFromByteWidth(size_t byte_width)
    {
        switch (byte_width)
        {
        case 1u:
            return EnumTagKind::Int8;
        case 2u:
            return EnumTagKind::Int16;
        case 4u:
            return EnumTagKind::Int32;
        case 8u:
            return EnumTagKind::Int64;
        default:
            assert(false && "Unexpected enum tag byte width");
            return EnumTagKind::Int32;
        }
    }

    // BelongsToModule returns true when the decl tree rooted at `reflection` declares `type`. It matches
    // by pointer identity: Slang interns a type to one TypeReflection per session, so the type's
    // declaration and every use of it share one pointer, even across a module boundary.
    // DeclReflection::getType() returns the declared type for a type declaration (enum, struct,
    // interface) and a decl-shaped wrapper for a variable, so a variable of `type` does not match here.
    // Only the declaration does.
    //NOLINTBEGIN(misc-no-recursion)
    bool BelongsToModule(slang::TypeReflection* type, slang::DeclReflection* reflection)
    {
        const int64_t childCount = reflection->getChildrenCount();
        for (int64_t i = 0; i < childCount; ++i)
        {
            slang::DeclReflection* child = reflection->getChild(i);
            if (child->getType() == type)
            {
                return true;
            }
            if (child->getChildrenCount() > 0 && BelongsToModule(type, child))
            {
                return true;
            }
        }
        return false;
    }
    //NOLINTEND(misc-no-recursion)

    // FindDeclaringModule returns the name of the loaded module whose own decl tree declares `type`, or
    // an empty view when no loaded module does. The caller uses it to import the module that declares an
    // enum an axis references, which differs from the module the axis variable lives in when the type
    // arrives through an `import`.
    std::string_view FindDeclaringModule(slang::ISession* session, slang::TypeReflection* type)
    {
        if (type == nullptr)
        {
            return {};
        }

        for (int64_t i = 0; i < session->getLoadedModuleCount(); ++i)
        {
            slang::IModule* module = session->getLoadedModule(i);
            if (module == nullptr)
            {
                continue;
            }
            slang::DeclReflection* moduleReflection = module->getModuleReflection();
            if (moduleReflection == nullptr)
            {
                // A precompiled or builtin module can report no reflection. Skip it rather than walk a
                // null tree, the same way BuildDeclaredAxes does.
                continue;
            }
            if (BelongsToModule(type, moduleReflection))
            {
                return module->getName();
            }
        }
        return {};
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
    const std::filesystem::path attributesPath = std::filesystem::canonical("C:/SoftwareDev/Lodestone/builtins/");
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

CookResult<std::vector<RawAxisDeclaration>> SlangModuleContext::BuildDeclaredAxes() const
{
    AxesBuildContext axesBuildContext;
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
            const CookError collected = collectAxesFromDecl(moduleReflection,
                                                            moduleNameSv,
                                                            axesBuildContext);

            if (!collected)
            {
                return std::unexpected(collected);
            }
        }
    }

    const CookError interfacesBuilt = buildInterfaceAxes(axesBuildContext);
    if (!interfacesBuilt)
    {
        return std::unexpected(interfacesBuilt);
    }

    return std::move(axesBuildContext.AxisDeclarations);
}

//NOLINTBEGIN(misc-no-recursion)
CookError SlangModuleContext::collectAxesFromDecl(slang::DeclReflection* reflection,
                                                  std::string_view module_name,
                                                  AxesBuildContext& axes_build_context) const
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
            CookResult<std::optional<RawAxisDeclaration>> axisDeclResult = buildAxisDecl(child, module_name);
            if (!axisDeclResult)
            {
                return axisDeclResult.error();
            }
            if (axisDeclResult->has_value())
            {
                axes_build_context.AxisDeclarations.emplace_back(std::move(**axisDeclResult));
            }
        }
        else if (child->getKind() == slang::DeclReflection::Kind::Struct)
        {   
            // ls_axis_interface or ls_axis_interface_impl values
            const CookError axisDataStaged = stageInterfaceStruct(child, module_name, axes_build_context);
            if (!axisDataStaged)
            {
                return axisDataStaged;
            }
        }
        else if (child->getChildrenCount() > 0u)
        {
            // A `__include`/`implementing` fragment reflects as an Unsupported node whose children are
            // the real declarations. `getChild`/`getChildrenCount` are safe on such a node, so descend.
            const CookError nested = collectAxesFromDecl(child, module_name, axes_build_context);
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

CookResult<std::optional<RawAxisDeclaration>> SlangModuleContext::buildAxisDecl(slang::DeclReflection* reflection,
                                                                                std::string_view module_name) const
{
    slang::VariableReflection* variableReflection = reflection->asVariable();
    if (variableReflection == nullptr)
    {
        return std::unexpected(CookError::AttributeExpressionParseFailed);
    }

    slang::Attribute* booleanAxisAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_axis_boolean");
    slang::Attribute* enumAxisAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_axis_enum");
    slang::Attribute* valuesAttr = variableReflection->findAttributeByName(globalSession.get(), "ls_axis_values");
    // early out: if none of the attributes are present, this is not an axis declaration
    if ((booleanAxisAttr == nullptr) && (valuesAttr == nullptr) && (enumAxisAttr == nullptr))
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

    if (enumAxisAttr != nullptr)
    {
        result.IsEnumAxis = true;
        slang::TypeReflection* typeReflection = variableReflection->getType();
        if (typeReflection == nullptr || typeReflection->getKind() != slang::TypeReflection::Kind::Enum)
        {
            const std::string errStr =
                std::format("Variable '{}' has an enum axis attribute but is not of enum type", variableName);
            return std::unexpected(ReportError(*diagnosticSink,
                                                CookError::AttributeExpressionParseFailed,
                                                errStr));
        }

        const char* enumTypeName = typeReflection->getName();
        result.RootName = enumTypeName != nullptr ? enumTypeName : "<TypeNameResolutionFailed>";
        // The synthetic module for each enum value imports the module that DECLARES the enum, so the
        // enum type resolves in that per-variant translation unit. The axis variable can live in a
        // different module that imports the enum, so we find the declaring module by reflection. When
        // the walk finds nothing, the enum is declared in the axis variable's own module, so fall back
        // to that.
        const std::string_view enumDeclaringModule = FindDeclaringModule(Session(), typeReflection);
        result.RootModule =
            enumDeclaringModule.empty() ? std::string(module_name) : std::string(enumDeclaringModule);
        // slang repurposes the field accessors for enum types to return the enum cases, in declaration order
        // we handle value retrieval explicitly since size expressions may reference them, and we don't want to
        // assume enums are just linearly incremented sequences
        const uint32_t enumCaseCount = typeReflection->getFieldCount();
        result.EnumCases.reserve(enumCaseCount);
        for (int32_t i = 0; std::cmp_less(i, enumCaseCount); ++i)
        {
            slang::VariableReflection* caseVar = typeReflection->getFieldByIndex(i);
            if (caseVar == nullptr)
            {
                const std::string errStr = 
                    std::format("Failed to retrieve enum case at index {} for variable '{}'", i, variableName);
                return std::unexpected(ReportError(*diagnosticSink,
                                                    CookError::AttributeExpressionParseFailed,
                                                    errStr));
            }

            const char* caseName = caseVar->getName();
            Slang::ComPtr<slang::IBlob> caseValueBlob;
            // unpack and interpret the enum case value from the blob
            int64_t caseValue = 0;
            if (SLANG_SUCCEEDED(caseVar->getDefaultValueBlob(caseValueBlob.writeRef())))
            {
                const void* caseValueData = caseValueBlob->getBufferPointer();
                // Slang resolves getScalarType() only for an enum reflected from its own declaring
                // module. An enum an axis reaches through an `import` reports ScalarType::None, so fall
                // back to the case value blob's width, which is always present.
                const slang::TypeReflection::ScalarType scalarType = typeReflection->getScalarType();
                const EnumTagKind enumTagKind =
                    scalarType != slang::TypeReflection::ScalarType::None
                        ? ScalarTypeToEnumTagKind(scalarType)
                        : EnumTagKindFromByteWidth(caseValueBlob->getBufferSize());
                caseValue = DecodeEnumTag(enumTagKind, caseValueData);
            }
            result.EnumCases.emplace_back(caseName != nullptr ? caseName : "<CaseNameResolutionFailed>", caseValue);
        }
    }

    result.Name = variableName;
    if (valuesAttr != nullptr)
    {
        // can't have both a values attribute and a boolean axis: the two are mutually exclusive
        if (result.IsBooleanAxis || result.IsEnumAxis)
        {
            const std::string errStr =
                std::format("Variable '{}' cannot have both a boolean axis or enum axis and axis values", variableName);
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
        result.SourceFile = sourceLoc.filePath != nullptr ? sourceLoc.filePath : "<SourceLocFileResolutionFailed>";
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
                                                                   std::string_view attr_name) const
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

CookError SlangModuleContext::stageInterfaceStruct(slang::DeclReflection* reflection,
                                                   std::string_view module_name,
                                                   AxesBuildContext& axes_build_context) const
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
        axes_build_context.InterfaceAxisStubs.emplace_back(std::move(stub));
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
    axes_build_context.InterfaceAxisImplStubs.emplace_back(std::move(stub));
    return CookError::Success;
}

CookError SlangModuleContext::buildInterfaceAxes(AxesBuildContext& axes_build_context) const
{
    if (axes_build_context.InterfaceAxisStubs.empty())
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
    

    for (const InterfaceAxisStub& stub : axes_build_context.InterfaceAxisStubs)
    {
        // match each extern to the one exact interface it actaully conforms to
        std::string matchedInterfaceName;

        // validate there is at least one matching interface, and that there's not multiple conformity
        // we'll have to traverse this list again later, but that shouldn't be too costly
        for (const InterfaceAxisImplStub& implStub : axes_build_context.InterfaceAxisImplStubs)
        {
            slang::TypeReflection* interfaceType = programLayout->findTypeByName(implStub.InterfaceName.c_str());
            // now check to see if implStub is a subtype of the outer type
            if (interfaceType != nullptr && programLayout->isSubType(stub.Type, interfaceType))
            {
                if (matchedInterfaceName.empty())
                {
                    matchedInterfaceName = implStub.InterfaceName;
                    // could we break here, if we know there's at least one matching interface?
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
            .Kind = "technique", // an interface axis selects behaviour; ls_axis_kind cannot sit on a struct, so default it here

            .SourceFile = stub.SourceFile.empty() ? "<unknown>" : stub.SourceFile,
            .SourceLine = stub.SourceLine,
            .SourceColumn = stub.SourceColumn,
            .RootName = matchedInterfaceName,
            .InterfaceImpls = {}
        };

        slang::TypeReflection* interfaceType = programLayout->findTypeByName(matchedInterfaceName.c_str());
        for (const InterfaceAxisImplStub& implStub : axes_build_context.InterfaceAxisImplStubs)
        {
            // now add the conforming implementations to the axis declaration, since we've confirmed this 
            // interface axis has at least one conforming implementation and is otherwise valid
            if (implStub.InterfaceName != matchedInterfaceName)
            {
                continue;
            }

            if (!programLayout->isSubType(implStub.Type, interfaceType))
            {
                const char* implName = implStub.Type->getName();
                const std::string warningStr = std::format("Type '{}' does not conform to interface '{}'",
                                                           implName != nullptr ? implName : "<unknown>",
                                                           matchedInterfaceName);
                ReportWarning(*diagnosticSink, warningStr);
                continue;
            }

            axisDecl.InterfaceImpls.emplace_back(implStub.Impl);
        }

        if (axisDecl.InterfaceImpls.empty())
        {
            const std::string errorStr = std::format("Interface Axis '{}' has no conforming implementations", stub.Name);
            return ReportError(*diagnosticSink,
                               CookError::SlangNoConformingInterfaces,
                               errorStr);
        }

        std::ranges::sort(axisDecl.InterfaceImpls, std::less{});
        axes_build_context.AxisDeclarations.emplace_back(std::move(axisDecl));
    }

    return CookError::Success;
}

CookError SlangModuleContext::rejectResourceMembers(slang::TypeReflection* type, std::string_view type_name) const
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
