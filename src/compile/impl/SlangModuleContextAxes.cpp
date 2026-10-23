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

    // an imported enum reflects with ScalarType::None, but its blob has the real width. Slang enums default to int
    EnumTagKind EnumTagKindFromByteWidth(size_t byte_width);

    /** Returns the initializer as reflection reads it: literals only, so no value means "unreadable", not "no default".
     * The source text is the primary reader. */
    std::optional<int64_t> ReadReflectedDefault(slang::VariableReflection* variable)
    {
        Slang::ComPtr<slang::IBlob> defaultBlob;
        if (SLANG_FAILED(variable->getDefaultValueBlob(defaultBlob.writeRef())) || defaultBlob == nullptr)
        {
            return std::nullopt;
        }

        const size_t byteWidth = defaultBlob->getBufferSize();
        const auto* bytes = static_cast<const std::byte*>(defaultBlob->getBufferPointer());
        slang::TypeReflection* type = variable->getType();
        const slang::TypeReflection::ScalarType scalarType =
            type != nullptr ? type->getScalarType() : slang::TypeReflection::ScalarType::None;
        switch (scalarType)
        {
        case slang::TypeReflection::ScalarType::Bool:
        {
            auto isNonZero = [](std::byte value) { return value != std::byte{ 0 }; };
            return std::ranges::any_of(std::span<const std::byte>{ bytes, byteWidth }, isNonZero) ? 1 : 0;
        }
        case slang::TypeReflection::ScalarType::Int8:
        case slang::TypeReflection::ScalarType::UInt8:
        case slang::TypeReflection::ScalarType::Int16:
        case slang::TypeReflection::ScalarType::UInt16:
        case slang::TypeReflection::ScalarType::Int32:
        case slang::TypeReflection::ScalarType::UInt32:
        case slang::TypeReflection::ScalarType::Int64:
        case slang::TypeReflection::ScalarType::UInt64:
            if (byteWidth < EnumTagByteWidth(ScalarTypeToEnumTagKind(scalarType)))
            {
                return std::nullopt;
            }
            return DecodeEnumTag(ScalarTypeToEnumTagKind(scalarType), bytes);
        case slang::TypeReflection::ScalarType::None:
            if (byteWidth == 1u || byteWidth == 2u || byteWidth == 4u || byteWidth == 8u)
            {
                return DecodeEnumTag(EnumTagKindFromByteWidth(byteWidth), bytes);
            }
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

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

    // matches by pointer: Slang interns one TypeReflection per type per session, across modules
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

    // the enum's declaring module can differ from the axis variable's when the enum arrives through an import
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
                // precompiled and builtin modules can report no reflection
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

std::optional<std::string> SlangModuleContext::FindTypeDeclaringModule(std::string_view type_name) const
{
    Slang::ComPtr<slang::IBlob> diagBlob;
    slang::ProgramLayout* programLayout = rootModule->getLayout(0, diagBlob.writeRef());
    if (programLayout == nullptr)
    {
        return std::nullopt;
    }

    const std::string name{ type_name };
    slang::TypeReflection* type = programLayout->findTypeByName(name.c_str());
    if (type == nullptr)
    {
        return std::nullopt;
    }
    
    return std::string{ FindDeclaringModule(session, type) };
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
            const CookError axisDataStaged = stageInterfaceStruct(child, module_name, axes_build_context);
            if (!axisDataStaged)
            {
                return axisDataStaged;
            }
        }
        else if (child->getChildrenCount() > 0u)
        {
            // an `__include` fragment reflects as an Unsupported node, and its children are the real declarations
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
    if ((booleanAxisAttr == nullptr) && (valuesAttr == nullptr) && (enumAxisAttr == nullptr))
    {
        return std::nullopt;
    }

    const char* variableName = variableReflection->getName();
    if (variableName == nullptr)
    {
        return std::unexpected(CookError::SlangGetVariableNameFailed);
    }

    RawAxisDeclaration result{};
    result.ReflectedDefault = ReadReflectedDefault(variableReflection);
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
        // the synthetic module imports the enum's declaring module, or the axis's own when the search finds none
        const std::string_view enumDeclaringModule = FindDeclaringModule(Session(), typeReflection);
        result.RootModule =
            enumDeclaringModule.empty() ? std::string(module_name) : std::string(enumDeclaringModule);
        // the field accessors return enum cases. Values are read, since cases need not count up from zero
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
            int64_t caseValue = 0;
            if (SLANG_SUCCEEDED(caseVar->getDefaultValueBlob(caseValueBlob.writeRef())))
            {
                const void* caseValueData = caseValueBlob->getBufferPointer();
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

    // best effort: declarations in an `__include` fragment have no usable location, and that is still a valid axis
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
    slang::TypeReflection* type = reflection->getType();
    if (type == nullptr)
    {
        return CookError::Success;
    }

    slang::UserAttribute* axisAttr = type->findUserAttributeByName("ls_axis_interface");
    slang::UserAttribute* axisImplAttr = type->findUserAttributeByName("ls_axis_interface_impl");
    if ((axisAttr == nullptr) && (axisImplAttr == nullptr))
    {
        return CookError::Success;
    }

    const char* declNamePtr = reflection->getName();
    const std::string declName = declNamePtr != nullptr ? std::string{ declNamePtr } : "<unknown>";
    if ((axisAttr != nullptr) && (axisImplAttr != nullptr))
    {
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
        std::string matchedInterfaceName;

        for (const InterfaceAxisImplStub& implStub : axes_build_context.InterfaceAxisImplStubs)
        {
            slang::TypeReflection* interfaceType = programLayout->findTypeByName(implStub.InterfaceName.c_str());
            if (interfaceType != nullptr && programLayout->isSubType(stub.Type, interfaceType))
            {
                if (matchedInterfaceName.empty())
                {
                    matchedInterfaceName = implStub.InterfaceName;
                }
                else if (matchedInterfaceName != implStub.InterfaceName)
                {
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
    // a conformance cannot carry a resource: binding layout is fixed before link-time specialization
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
