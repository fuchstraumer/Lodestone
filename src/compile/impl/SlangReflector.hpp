#pragma once
#ifndef LODESTONE_SLANG_REFLECTOR_HPP
#define LODESTONE_SLANG_REFLECTOR_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationSpace.hpp"
#include "target/TargetProfile.hpp"
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
    SlangReflector(std::span<const std::string> entry_point_names,
                   DiagnosticSink* sink,
                   slang::IGlobalSession* global_session,
                   AccessModel buffer_form,
                   TargetLanguage target_language = TargetLanguage::Invalid) noexcept;
    ~SlangReflector() noexcept = default;
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
    [[nodiscard]] CookError readBlockContainer(const ParameterBlockInfo& block,
                                               std::string_view scope_name,
                                               std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookError appendStoredValueDrafts(std::span<const StoredValueLeaf> values,
                                                    StoredValueContainer container,
                                                    BoundPlacement block,
                                                    std::string_view scope_name,
                                                    std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookError collectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                                   const BindingScope& scope,
                                                   std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookResult<slang::TypeLayoutReflection*> readEntryPointConstants(
        const LinkedVariant& linked_variant,
        int64_t entry_point_index,
        slang::EntryPointReflection* entry_point_layout,
        const BindingScope& scope,
        RawEntryPoint& out_entry_point,
        std::vector<RawBindingDraft>& out_drafts) const;
    [[nodiscard]] CookResult<BindingScope> entryPointScope(slang::EntryPointReflection* entry_point_layout,
                                                           std::string_view entry_point_name) const;
    [[nodiscard]] CookError readSingleStringAttribute(slang::VariableReflection* leaf_variable,
                                                      const char* attribute_name,
                                                      std::string_view binding_name,
                                                      std::string& out_value) const;
    [[nodiscard]] CookError collectRawSizeAttributes(slang::VariableReflection* leaf_variable,
                                                     std::string_view binding_name,
                                                     std::vector<RawSizeAttribute>& out_attributes) const;
    /** Null `out_values` means the struct cannot store a shim value, and a marked member there is an error. */
    [[nodiscard]] CookError collectStructMembers(slang::TypeLayoutReflection* struct_layout,
                                                  std::vector<ReflectedUniformMember>& members,
                                                  std::vector<StoredValueLeaf>* out_values) const;
    [[nodiscard]] CookError applyLeafTypeSamplerLayout(slang::TypeReflection* leaf_type,
                                                       RawBinding& binding) const;
    [[nodiscard]] CookError applyLeafTypeTextureLayout(slang::TypeReflection* leaf_type,
                                                       RawBinding& binding) const;
    [[nodiscard]] CookError applyLeafTypeUniformBufferLayout(slang::TypeLayoutReflection* leaf_layout,
                                                             RawBinding& binding,
                                                             std::vector<StoredValueLeaf>& out_values) const;
    [[nodiscard]] CookError applyLeafTypeStorageBufferLayout(slang::TypeLayoutReflection* leaf_layout,
                                                             RawBinding& binding) const;
    [[nodiscard]] CookError applyLeafTypeTexelBufferLayout(slang::TypeLayoutReflection* containing_layout,
                                                           SlangInt range_index,
                                                           slang::TypeReflection* leaf_type,
                                                           RawBinding& binding) const;
    [[nodiscard]] CookError applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                                SlangInt range_index,
                                                RawBinding& binding,
                                                std::vector<StoredValueLeaf>& out_values) const;

    static void extractRasterState(slang::EntryPointReflection* entry_point_layout,
                                   ShaderStageKind stage,
                                   ReflectedRasterState& raster);

    std::span<const std::string> entryPointNames;
    DiagnosticSink* sink{ nullptr };
    slang::IGlobalSession* globalSession;
    AccessModel bufferForm;
    TargetLanguage language{ TargetLanguage::Invalid };
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_REFLECTOR_HPP
