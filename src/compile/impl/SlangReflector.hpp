#pragma once
#ifndef LODESTONE_SLANG_REFLECTOR_HPP
#define LODESTONE_SLANG_REFLECTOR_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationSpace.hpp"
#include "slang.h"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lodestone
{
struct LinkedVariant;

class SlangReflector
{
public:
    // Since we need to propagate errors/results, ctor is just defaulted
    SlangReflector(std::span<const std::string> entry_point_names,
                   DiagnosticSink* sink,
                   slang::IGlobalSession* global_session,
                   PlacementKind active_placement_kind) noexcept;
    ~SlangReflector() noexcept = default;
    // this thing works like a local one-shot invocation of reflection, so copying
    // shouldn't happen, but better safe than sorry
    SlangReflector(const SlangReflector&) noexcept = delete;
    SlangReflector& operator=(const SlangReflector&) noexcept = delete;

    CookResult<RawVariant> Reflect(LinkedVariant& linked_variant, const VariantDescriptor& descriptor);

private:
    [[nodiscard]] CookResult<RawEntryPoint> extractRawEntryPoint(const LinkedVariant& linked_variant,
                                                                 int64_t entry_point_index,
                                                                 std::span<const RawBinding> global_bindings,
                                                                 std::vector<RawBindingDraft>& out_drafts);
    [[nodiscard]] CookError extractRawBindings(slang::ProgramLayout* program_layout,
                                               std::vector<RawBinding>& out_bindings,
                                               std::vector<RawSizeAttribute>& out_attributes) const;
    [[nodiscard]] CookError collectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
                                                      const BindingScope& scope,
                                                      std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookResult<RawBindingDraft> readBlockContainer(const ParameterBlockInfo& block,
                                                                 std::string_view scope_name) const;
    [[nodiscard]] CookError collectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                                   const BindingScope& scope,
                                                   std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookResult<BindingScope> entryPointScope(slang::EntryPointReflection* entry_point_layout,
                                                           std::string_view entry_point_name) const;
    [[nodiscard]] CookError collectRawSizeAttributes(slang::VariableReflection* leaf_variable,
                                                     std::string_view binding_name,
                                                     std::vector<RawSizeAttribute>& out_attributes) const;
    [[nodiscard]] CookError collectUniformMembers(slang::TypeLayoutReflection* struct_layout,
                                                  std::vector<ReflectedUniformMember>& members) const;
    [[nodiscard]] CookError applyLeafTypeUniformBufferLayout(slang::TypeLayoutReflection* buffer_leaf_layout,
                                                             RawBinding& binding) const;
    [[nodiscard]] CookError applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                                SlangInt range_index,
                                                slang::BindingType binding_type,
                                                RawBinding& binding) const;

    static void extractRasterState(slang::EntryPointReflection* entry_point_layout,
                                   ShaderStageKind stage,
                                   ReflectedRasterState& raster);

    std::span<const std::string> entryPointNames;
    DiagnosticSink* sink{ nullptr };
    slang::IGlobalSession* globalSession;
    // governs checks for how bindings and layouts are interpreted or lowered
    PlacementKind activePlacementKind;
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_REFLECTOR_HPP
