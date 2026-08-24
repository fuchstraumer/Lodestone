#pragma once
#ifndef LODESTONE_SLANG_COMPILER_IMPL_HPP
#define LODESTONE_SLANG_COMPILER_IMPL_HPP
#include "compile/SlangCompiler.hpp"
#include "SlangCompilerTypes.hpp"
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>

namespace lodestone
{


class SlangCompilerImpl
{
public:
    SlangCompilerImpl(const VariantDescriptor& descriptor);

private:
    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;
    slang::IModule* rootModule{ nullptr };
    std::vector<slang::IComponentType*> baseComponents;
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPointHandles;
    std::vector<std::string> entryPointNames;
    std::vector<slang::CompilerOptionEntry> compilerOptions;
    std::vector<std::string> moduleSourceTexts;
    std::string moduleName;
    /** Set once, by `Initialize`, and never null after that. A pointer rather than a reference only
     * because this object moves. */
    DiagnosticSink* sink{ nullptr };

    CookError createSession(const SlangCompilerCreateInfo& create_info);
    CookError loadRootModule();
    void readDependencySourceTexts();
    CookError collectEntryPoints();
    [[nodiscard]] CookResult<Slang::ComPtr<slang::IComponentType>> linkVariant(
        const PermutationAssignment& assignment) const;
    std::vector<std::string> generateEntryPointCode(slang::IComponentType* linked_program) const;
    CookResult<RawEntryPoint> extractRawEntryPoint(slang::IComponentType* linked_program,
                                                   slang::ProgramLayout* program_layout,
                                                   SlangInt entry_point_index,
                                                   std::span<const RawBinding> global_bindings,
                                                   std::vector<RawBindingDraft>& out_drafts);
    CookError extractRawBindings(slang::ProgramLayout* program_layout,
                                 std::vector<RawBinding>& out_bindings,
                                 std::vector<RawSizeAttribute>& out_attributes) const;
    CookError collectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
                                        const BindingScope& scope,
                                        std::vector<RawBindingDraft>& out_drafts) const;
    CookError collectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                     const BindingScope& scope,
                                     std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookResult<BindingScope> entryPointScope(slang::EntryPointReflection* entry_point_layout,
                                                           std::string_view entry_point_name) const;
    CookError collectRawSizeAttributes(slang::VariableReflection* leaf_variable,
                                       std::string_view binding_name,
                                       std::vector<RawSizeAttribute>& out_attributes) const;
    static void applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                    SlangInt range_index,
                                    slang::BindingType binding_type,
                                    RawBinding& binding);
    // Not static: it reports through the sink, which is a member.
    void collectUsedBindingIndices(slang::IComponentType* linked_program,
                                   SlangInt entry_point_index,
                                   std::span<const RawBinding> global_bindings,
                                   std::vector<uint32_t>& out_used_indices) const;
    static void extractRasterState(slang::EntryPointReflection* entry_point_layout,
                                   ShaderStageKind stage,
                                   ReflectedRasterState& raster);
};

}

#endif // !LODESTONE_SLANG_COMPILER_IMPL_HPP
