#pragma once
#ifndef LODESTONE_SLANG_MODULE_CONTEXT_HPP
#define LODESTONE_SLANG_MODULE_CONTEXT_HPP
#include "CookerErrors.hpp"
#include "SlangCompilerTypes.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/ShimLinkSource.hpp"
#include "compile/SlangCompiler.hpp"
#include "target/TargetProfile.hpp"
#include "ShaderLibraryTypes.hpp"
#include "slang.h"
#include "slang-com-ptr.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace lodestone
{

struct StructStringAttribute
{
    std::string StructName;
    std::string Argument;
};

/** @brief One thread's Slang sessions and loaded modules. */
class SlangModuleContext
{
public:
    SlangModuleContext() = default;
    ~SlangModuleContext() = default;
    SlangModuleContext(const SlangModuleContext&) = delete;
    SlangModuleContext& operator=(const SlangModuleContext&) = delete;

    [[nodiscard]] CookError Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink);
    [[nodiscard]] CookError RunBootstrap();
    [[nodiscard]] CookResult<std::vector<RawAxisDeclaration>> BuildDeclaredAxes() const;
    /** @brief Sorted module-scope variable names, including `__include` fragments. Filters the symbol table's derived
      * constants, since a line scanner cannot tell scopes apart. */
    [[nodiscard]] std::vector<std::string> BuildModuleScopeVariableNames() const;
    /** @brief Sorted module-scope struct names, including `__include` fragments. Confirms what the symbol table read. */
    [[nodiscard]] std::vector<std::string> BuildModuleScopeStructNames() const;
    [[nodiscard]] std::vector<StructStringAttribute> BuildStructStringAttributes(std::string_view attribute_name) const;
    [[nodiscard]] std::optional<std::string> FindTypeDeclaringModule(std::string_view type_name) const;
    [[nodiscard]] CookError LoadShimLinkModule(std::string_view source);
    [[nodiscard]] slang::IModule* ShimLinkModule() const noexcept;
    [[nodiscard]] CookError LoadShimFormatModules(std::span<const ShimFormatLinkSource> sources);
    /** @brief Null when no extern names the axis. */
    [[nodiscard]] slang::IModule* ShimFormatLinkModule(std::string_view axis_name, uint32_t case_ordinal) const noexcept;
    [[nodiscard]] slang::IGlobalSession* GlobalSession() const noexcept;
    [[nodiscard]] slang::ISession* Session() const noexcept;
    [[nodiscard]] std::vector<slang::IComponentType*> BaseComponents() const noexcept;
    [[nodiscard]] size_t EntryPointCount() const noexcept;
    [[nodiscard]] const std::vector<std::string>& EntryPointNames() const noexcept;
    [[nodiscard]] std::string_view ModuleName() const noexcept;
    [[nodiscard]] const std::vector<std::string>& ModuleSourceStrings() const noexcept;
    /** @brief Parallel to `ModuleSourceStrings()`. Builtins appear by search path. */
    [[nodiscard]] const std::vector<std::string>& ModuleSourcePaths() const noexcept;
    [[nodiscard]] std::vector<std::string_view> ModuleSourceStringViews() const noexcept;
    /** @brief The file paths for the depfile, excluding embedded builtins. */
    [[nodiscard]] const std::vector<std::string>& DependencyFilePaths() const noexcept;
    /** @brief The form of the profile's buffers. Only the Pointer form permits a pointer member. */
    [[nodiscard]] AccessModel BufferForm() const noexcept;
    [[nodiscard]] TargetLanguage Language() const noexcept;

    /** @brief Returns every loaded module, serialized in dependency order. */
    [[nodiscard]] std::vector<SerializedModule> SerializeModules() const;

    /** @brief Primes a worker's session from the bootstrap's serialized modules. */
    [[nodiscard]] CookError RunWorkerSetup(std::span<const SerializedModule> serialized_modules);

private:

    [[nodiscard]] CookError primeSessionFromCache(std::span<const SerializedModule> serialized_modules);
    [[nodiscard]] CookError loadRootModule();
    [[nodiscard]] CookError readDependencySourceStrings();
    [[nodiscard]] CookError enumerateEntryPoints();
    // fixed: base components are added in this one place only
    [[nodiscard]] CookError buildSlangComponents();


    struct AxesBuildContext
    {
        std::vector<RawAxisDeclaration> AxisDeclarations;
        std::vector<InterfaceAxisStub> InterfaceAxisStubs;
        std::vector<InterfaceAxisImplStub> InterfaceAxisImplStubs;
    };

    [[nodiscard]] CookError collectAxesFromDecl(slang::DeclReflection* reflection,
                                                std::string_view module_name,
                                                AxesBuildContext& axes_build_context) const;
    [[nodiscard]] CookResult<std::optional<RawAxisDeclaration>> buildAxisDecl(slang::DeclReflection* reflection,
                                                                              std::string_view module_name) const;
    [[nodiscard]] CookResult<std::string> extractSingleAttribute(slang::DeclReflection* decl_reflection,
                                                                 slang::Attribute* attribute,
                                                                 std::string_view attr_name) const;
    [[nodiscard]] CookError stageInterfaceStruct(slang::DeclReflection* reflection,
                                                 std::string_view module_name,
                                                 AxesBuildContext& axes_build_context) const;
    [[nodiscard]] CookError buildInterfaceAxes(AxesBuildContext& axes_build_context) const;
    [[nodiscard]] CookError rejectResourceMembers(slang::TypeReflection* type,
                                                  std::string_view type_name,
                                                  std::string_view module_name,
                                                  const ShaderLocation& where) const;

    std::string cacheDirectory;
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;
    slang::IModule* rootModule{ nullptr };
    slang::IModule* shimLinkModule{ nullptr };

    struct ShimFormatModule
    {
        std::string AxisName;
        uint32_t CaseOrdinal{ 0u };
        slang::IModule* Module{ nullptr };
    };
    std::vector<ShimFormatModule> shimFormatModules;
    std::vector<slang::IComponentType*> baseComponents;
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;
    std::vector<std::string> entryPointNames;
    std::string moduleName;
    std::vector<std::string> moduleSourceStrings;
    std::vector<std::string> moduleSourcePaths;
    std::vector<std::string> dependencyFilePaths;
    AccessModel bufferForm{ AccessModel::Invalid };
    TargetLanguage language{ TargetLanguage::Invalid };
    DiagnosticSink* diagnosticSink{ nullptr };
};

/** @brief Writes the bootstrap's modules to the cache, from the calling thread only. A failure is a warning: the cache
 * is an optimization. */
CookError WriteModuleCache(std::span<const SerializedModule> serialized_modules, DiagnosticSink& sink);

}

#endif // !LODESTONE_SLANG_MODULE_CONTEXT_HPP
