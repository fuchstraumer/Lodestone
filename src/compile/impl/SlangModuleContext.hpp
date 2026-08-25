#pragma once
#ifndef LODESTONE_SLANG_MODULE_CONTEXT_HPP
#define LODESTONE_SLANG_MODULE_CONTEXT_HPP
#include "CookerErrors.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/SlangCompiler.hpp"
#include "slang.h"
#include "slang-com-ptr.h"
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace lodestone
{
/**@brief Each worker thread creates one of these objects, encapsulating the context and Slang state
  *needed for efficient compiliation of variants. The global session and base session types both need
  *to be per-thread, ensuring that each thread has its own independent Slang context.
*/
class SlangModuleContext
{
public:
    SlangModuleContext() = default;
    ~SlangModuleContext() = default;

    [[nodiscard]] CookError Initialize(const SlangCompilerCreateInfo& create_info, DiagnosticSink& sink);
    [[nodiscard]] CookError RunBootstrap();

    [[nodiscard]] slang::ISession* Session() const noexcept;
    [[nodiscard]] std::vector<slang::IComponentType*> BaseComponents() const noexcept;
    [[nodiscard]] size_t EntryPointCount() const noexcept;
    [[nodiscard]] std::vector<std::string_view> EntryPointNames() const noexcept;
    [[nodiscard]] std::string_view ModuleName() const noexcept;
    [[nodiscard]] std::vector<std::string_view> ModuleSourceStrings() const noexcept;

private:

    [[nodiscard]] CookError loadRootModule();
    [[nodiscard]] CookError readDependencySourceStrings();
    [[nodiscard]] CookError enumerateEntryPoints();

    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;
    slang::IModule* rootModule{ nullptr };
    std::vector<slang::IComponentType*> baseComponents;
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;
    std::vector<std::string> entryPointNames;
    std::string moduleName;
    std::vector<std::string> moduleSourceStrings;
    DiagnosticSink* diagnosticSink{ nullptr };
};
}

#endif // !LODESTONE_SLANG_MODULE_CONTEXT_HPP
