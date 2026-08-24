#pragma once
#ifndef LODESTONE_SLANG_REFLECTOR_HPP
#define LODESTONE_SLANG_REFLECTOR_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "slang.h"
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>


namespace lodestone
{

class SlangReflector
{
public:
    // Since we need to propagate errors/results, ctor is just defaulted
    SlangReflector() noexcept = default;
    ~SlangReflector() noexcept = default;
    // this thing works like a local one-shot invocation of reflection, so copying
    // shouldn't happen, but better safe than sorry
    SlangReflector(const SlangReflector&) noexcept = delete;
    SlangReflector& operator=(const SlangReflector&) noexcept = delete;

    CookResult<RawVariant> Reflect();

private:
    [[nodiscard]] CookResult<RawEntryPoint> extractRawEntryPoint(slang::IComponentType* linked_program,
                                                                 slang::ProgramLayout* program_layout,
                                                                 SlangInt entry_point_index,
                                                                 std::span<const RawBinding> global_bindings,
                                                                 std::vector<RawBindingDraft>& out_drafts);
    [[nodiscard]] CookError extractRawBindings(slang::ProgramLayout* program_layout,
                                               std::vector<RawBinding>& out_bindings,
                                               std::vector<RawSizeAttribute>& out_attributes) const;
    [[nodiscard]] CookError collectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
                                                      const BindingScope& scope,
                                                      std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookError collectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                                   const BindingScope& scope,
                                                   std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookResult<BindingScope> entryPointScope(slang::EntryPointReflection* entry_point_layout,
                                                           std::string_view entry_point_name) const;
    [[nodiscard]] CookError collectRawSizeAttributes(slang::VariableReflection* leaf_variable,
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

} // namespace lodestone

#endif // !LODESTONE_SLANG_REFLECTOR_HPP
