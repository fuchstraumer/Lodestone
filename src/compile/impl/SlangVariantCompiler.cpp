#include "SlangVariantCompiler.hpp"
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "SlangCompilerTypes.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "model/ShaderDataSchema.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationValue.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
  
namespace
{
    // will fix this, but for now bring everything from lodestone into scope
    using namespace lodestone;

    constexpr SlangInt k_WgslTargetIndex = 0;
    /** What Slang names the scope it moves each entry point `uniform` parameter into.
     *
     * `slang-ir-entry-point-uniforms.cpp` writes this string as a name hint, and reflection reports it
     * nowhere. It is a Slang convention, so it lives behind the Slang wall and reaches no other
     * folder. Every emitted name of an entry point parameter starts with it. */
    constexpr std::string_view k_EntryPointScopeName = "entryPointParams";

    constexpr SlangUInt k_WorkgroupAxisCount = 3u;

    // The shader-side attribute names live on RawSizeAttributeKind, so one table states the contract
    // and this file asks it for the spelling. Slang drops the `Attribute` suffix from the struct name,
    // so those strings match the declarations in tests/assets/LodestoneAttributes.slang.
    constexpr std::array<RawSizeAttributeKind, 3u> k_SizeAttributeKinds{ RawSizeAttributeKind::ElementCount,
                                                                         RawSizeAttributeKind::Extent2d,
                                                                         RawSizeAttributeKind::Extent3d };
                                                         
    std::string BlobToString(slang::IBlob* blob)
    {
        // Slang leaves the blob null when a call has nothing to say, and a clean codegen is the common
        // case. `GenerateOneEntryPoint` reads its diagnostic blob without asking, so the check is here
        // and not only in `ReportDiagnostics`.
        if (blob == nullptr)
        {
            return {};
        }

        return std::string{ static_cast<const char*>(blob->getBufferPointer()), blob->getBufferSize() };
    }

    void ReportDiagnostics(DiagnosticSink& sink, std::string_view context, slang::IBlob* blob)
    {
        // this check is the point of this function: only report diagnostics if blob has content,
        // but otherwise make it trivial to call inline in case we want to report diagnositcs from
        // slang calls
        if (blob == nullptr || blob->getBufferSize() == 0u)
        {
            return;
        }

        ParseSlangDiagnostics(BlobToString(blob), context, sink);
    }

    /** Attribute string arguments reflect as a pointer plus a length, and a null return means the
     * argument was not a string literal after all. That is a cook error rather than a skip: the
     * annotation was written, so the author expects it to do something. */
    CookResult<std::string> ReadStringArgument(slang::Attribute* attribute,
                                               uint32_t argument_index,
                                               std::string_view binding_name)
    {
        size_t length = 0u;
        const char* text = attribute->getArgumentValueString(argument_index, &length);
        if (text == nullptr)
        {
            std::println(stderr,
                         "[shader_cooker] argument {} of [{}] on '{}' is not a string literal",
                         argument_index,
                         attribute->getName(),
                         binding_name);
            return std::unexpected(CookError::SizeExpressionParseFailed);
        }

        // The reflected span includes the surrounding quotes on some paths; trim them if present.
        std::string_view value{ text, length };
        if (value.size() >= 2u && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1u, value.size() - 2u);
        }

        return std::string{ value };
    }

    /** What one entry point's codegen produced. The diagnostic text travels with the code so that a
     * worker thread never touches a sink. coalesced after threads join */
    struct GeneratedEntryPoint
    {
        std::string Code;
        std::string Diagnostics;
    };

    GeneratedEntryPoint GenerateOneEntryPoint(slang::IComponentType* linked_program, size_t index)
    {
        Slang::ComPtr<slang::IBlob> code;
        Slang::ComPtr<slang::IBlob> diagnostics;
        const bool failed = SLANG_FAILED(linked_program->getEntryPointCode(
            static_cast<SlangInt>(index), k_WgslTargetIndex, code.writeRef(), diagnostics.writeRef()));

        return GeneratedEntryPoint{ .Code = failed ? std::string{} : BlobToString(code.get()),
                                    .Diagnostics = BlobToString(diagnostics.get()) };
    }


}

namespace lodestone
{

CookError SlangVariantCompiler::loadRootModule()
{
    Slang::ComPtr<slang::IBlob> diagnostics;
    rootModule = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
    ReportDiagnostics(*sink, "loadModule", diagnostics.get());

    if (rootModule == nullptr)
    {
        return CookError::ModuleLoadFailed;
    }

    baseComponents.clear();
    baseComponents.reserve(4u + static_cast<size_t>(rootModule->getDefinedEntryPointCount()));
    baseComponents.push_back(rootModule);

    readDependencySourceTexts();
    return CookError::Success;
}

/** Slang reports every file the module pulled in, transitively. That set is what the axis-name check
 * searches, and it is also the right input for a future content hash driving live reload. */
void SlangVariantCompiler::readDependencySourceTexts()
{
    const SlangInt32 dependencyCount = rootModule->getDependencyFileCount();
    moduleSourceTexts.clear();
    moduleSourceTexts.reserve(static_cast<size_t>(dependencyCount));

    for (SlangInt32 i = 0; i < dependencyCount; ++i)
    {
        const char* dependencyPath = rootModule->getDependencyFilePath(i);
        if (dependencyPath == nullptr)
        {
            continue;
        }

        std::ifstream file{ dependencyPath, std::ios::binary };
        if (!file)
        {
            std::println(stderr, "[shader_cooker] could not read dependency {}", dependencyPath);
            continue;
        }

        moduleSourceTexts.emplace_back(std::istreambuf_iterator<char>{ file },
                                       std::istreambuf_iterator<char>{});
    }
}

CookError SlangVariantCompiler::collectEntryPoints()
{
    const SlangInt32 entryPointCount = rootModule->getDefinedEntryPointCount();
    entryPointNames.clear();
    entryPointNames.reserve(static_cast<size_t>(entryPointCount));
    entryPointHandles.clear();
    entryPointHandles.reserve(static_cast<size_t>(entryPointCount));

    for (SlangInt32 i = 0; i < entryPointCount; ++i)
    {
        Slang::ComPtr<slang::IEntryPoint> entryPoint;
        if (SLANG_FAILED(rootModule->getDefinedEntryPoint(i, entryPoint.writeRef())))
        {
            return CookError::EntryPointEnumerationFailed;
        }

        entryPointNames.emplace_back(entryPoint->getFunctionReflection()->getName());
        baseComponents.push_back(entryPoint.get());
        entryPointHandles.push_back(entryPoint);
    }

    return CookError::Success;
}

CookResult<Slang::ComPtr<slang::IComponentType>> SlangVariantCompiler::linkVariant(
    const PermutationAssignment& assignment) const
{
    std::vector<slang::IComponentType*> components = baseComponents;
    components.reserve(baseComponents.size() + assignment.size());

    for (const PermutationBinding& binding : assignment)
    {
        const std::string variantModuleName = MakeVariantModuleName(binding.Axis->Name, binding.Value);
        const std::string variantModulePath = MakeVariantModulePath(binding.Axis->Name, binding.Value);
        const std::string variantSource = MakeExportedConstantSource(binding.Axis->Name, binding.Value);

        Slang::ComPtr<slang::IBlob> diagnostics;
        slang::IModule* variantModule = session->loadModuleFromSourceString(variantModuleName.c_str(),
                                                                            variantModulePath.c_str(),
                                                                            variantSource.c_str(),
                                                                            diagnostics.writeRef());
        ReportDiagnostics(*Sink, "loadModuleFromSourceString", diagnostics.get());

        if (variantModule == nullptr)
        {
            return std::unexpected(CookError::VariantModuleCreationFailed);
        }

        components.push_back(variantModule);
    }

    Slang::ComPtr<slang::IBlob> diagnostics;
    Slang::ComPtr<slang::IComponentType> composite;
    session->createCompositeComponentType(components.data(),
                                          static_cast<SlangInt>(components.size()),
                                          composite.writeRef(),
                                          diagnostics.writeRef());
    ReportDiagnostics(*sink, "createCompositeComponentType", diagnostics.get());

    if (composite == nullptr)
    {
        return std::unexpected(CookError::CompositeCreationFailed);
    }

    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef())))
    {
        ReportDiagnostics(*sink, "link", diagnostics.get());
        return std::unexpected(CookError::LinkFailed);
    }

    return linked;
}

std::vector<std::string> SlangVariantCompiler::generateEntryPointCode(
    slang::IComponentType* linked_program) const
{
    const size_t entryPointCount = entryPointNames.size();
    std::vector<std::string> generated(entryPointCount);

    for (size_t i = 0; i < entryPointCount; ++i)
    {
        GeneratedEntryPoint result = GenerateOneEntryPoint(linked_program, i);
        if (!result.Diagnostics.empty())
        {
            ParseSlangDiagnostics(result.Diagnostics, "getEntryPointCode", *sink);
        }
        generated[i] = std::move(result.Code);
    }

    return generated;
}

/** Reads the size, shape, and type facts off one binding range's leaf type layout.
 *
 * Slang wraps a resource type around the type it carries, so the useful facts sit one level down.
 * A structured buffer reports its element layout. A texture reports the type it returns. */
void SlangVariantCompiler::applyLeafTypeLayout(slang::TypeLayoutReflection* containing_layout,
                                              SlangInt range_index,
                                              slang::BindingType binding_type,
                                              RawBinding& binding)
{
    slang::TypeLayoutReflection* leafLayout = containing_layout->getBindingRangeLeafTypeLayout(range_index);
    if (leafLayout == nullptr)
    {
        return;
    }

    if (binding.Kind == BindingKind::StorageTexture)
    {
        binding.StorageFormat =
            FromSlangImageFormat(containing_layout->getBindingRangeImageFormat(range_index));
        binding.StorageAccess = FromSlangBindingTypeAccess(binding_type);
    }

    if (binding.Kind == BindingKind::Sampler)
    {
        binding.Shape = ResourceShape::Invalid;
        binding.SamplerType = SamplerBindingType::Filtering;
        return;
    }

    slang::TypeReflection* leafType = leafLayout->getType();
    if (leafType == nullptr)
    {
        return;
    }

    binding.Shape = FromSlangResourceShape(leafType->getResourceShape());

    if (IsTextureBinding(binding.Kind))
    {
        slang::TypeReflection* resultType = leafType->getResourceResultType();
        if (resultType != nullptr)
        {
            binding.SampleType = FromSlangScalarType(resultType->getScalarType());
        }

        return;
    }

    if (binding.Kind == BindingKind::UniformBuffer)
    {
        // A uniform block's size is fully determined. The graph must never take it from the caller.
        binding.ByteSize = static_cast<uint64_t>(leafLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        slang::TypeLayoutReflection* elementLayout = leafLayout->getElementTypeLayout();
        if (elementLayout != nullptr && binding.ByteSize == 0u)
        {
            binding.ByteSize =
                static_cast<uint64_t>(elementLayout->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));
        }

        CollectUniformMembers(elementLayout, binding.UniformMembers);
        return;
    }

    slang::TypeLayoutReflection* elementLayout = leafLayout->getElementTypeLayout();
    if (elementLayout != nullptr)
    {
        binding.ElementStride = static_cast<uint32_t>(elementLayout->getSize());
    }
}

/** Reads one `[vx_*]` annotation into its argument strings. Stage 3 never evaluates one, so a
 * malformed argument is caught here only when it is not a string at all. */
CookError SlangVariantCompiler::CollectRawSizeAttributes(slang::VariableReflection* leaf_variable,
                                                        std::string_view binding_name,
                                                        std::vector<RawSizeAttribute>& out_attributes) const
{
    if (leaf_variable == nullptr)
    {
        // end of leaf/chain
        return CookError::Success;
    }

    for (const RawSizeAttributeKind kind : k_SizeAttributeKinds)
    {
        const std::string attributeName{ ToString(kind) };
        slang::Attribute* found =
            leaf_variable->findAttributeByName(globalSession.get(), attributeName.c_str());
        if (found == nullptr)
        {
            continue;
        }

        RawSizeAttribute attribute;
        attribute.Kind = kind;
        attribute.Arguments.reserve(ArgumentCountOf(kind));

        for (uint32_t i = 0u; i < ArgumentCountOf(kind); ++i)
        {
            CookResult<std::string> argument = ReadStringArgument(found, i, binding_name);
            if (!argument)
            {
                return argument.error();
            }

            attribute.Arguments.emplace_back(std::move(argument.value()));
        }

        out_attributes.emplace_back(std::move(attribute));
    }

    return CookError::Success;
}

/** Walks the binding ranges of one scope, and drafts a binding for each one. */
CookError SlangVariantCompiler::CollectBindingRangeDrafts(slang::TypeLayoutReflection* containing_layout,
                                                         const BindingScope& scope,
                                                         std::vector<RawBindingDraft>& out_drafts) const
{
    if (containing_layout == nullptr)
    {
        // reached end of the binding range chain (hopefully)
        return CookError::Success;
    }

    const SlangInt bindingRangeCount = containing_layout->getBindingRangeCount();
    out_drafts.reserve(out_drafts.size() + static_cast<size_t>(bindingRangeCount));

    std::vector<std::string> scopeNames(static_cast<size_t>(bindingRangeCount), scope.Name);
    CollectScopeNames(containing_layout, scope.Name, 0, scopeNames);

    for (SlangInt rangeIndex = 0; rangeIndex < bindingRangeCount; ++rangeIndex)
    {
        const slang::BindingType bindingType = containing_layout->getBindingRangeType(rangeIndex);
        // Skip input/output attributes, which slang considers to be part of the global params
        if (bindingType == slang::BindingType::Unknown || bindingType == slang::BindingType::VaryingInput ||
            bindingType == slang::BindingType::VaryingOutput)
        {
            continue;
        }

        const SlangInt descriptorSetIndex = containing_layout->getBindingRangeDescriptorSetIndex(rangeIndex);
        const SlangInt descriptorRangeIndex =
            containing_layout->getBindingRangeFirstDescriptorRangeIndex(rangeIndex);
        if (descriptorSetIndex < 0 || descriptorRangeIndex < 0)
        {
            continue;
        }

        const std::optional<uint32_t> spaceOffset = ResolvedNumber(
            static_cast<size_t>(containing_layout->getDescriptorSetSpaceOffset(descriptorSetIndex)));
        const std::optional<uint32_t> indexOffset =
            ResolvedNumber(static_cast<size_t>(containing_layout->getDescriptorSetDescriptorRangeIndexOffset(
                descriptorSetIndex, descriptorRangeIndex)));
        if (!spaceOffset || !indexOffset)
        {
            ReportUnresolvedLocation("binding", rangeIndex);
            continue;
        }

        slang::VariableReflection* leafVariable = containing_layout->getBindingRangeLeafVariable(rangeIndex);
        const char* leafName = leafVariable != nullptr ? leafVariable->getName() : nullptr;

        RawBindingDraft draft;
        draft.Binding.Name = leafName != nullptr ? leafName : std::string{};
        draft.Binding.ScopeName = std::move(scopeNames[static_cast<size_t>(rangeIndex)]);
        draft.Binding.Placement = BoundPlacement{ .Group = scope.Base.Group + *spaceOffset,
                                                  .Binding = scope.Base.Binding + *indexOffset };
        draft.Binding.Kind = FromSlangBindingType(bindingType);
        draft.Binding.ArrayCount =
            static_cast<uint32_t>(containing_layout->getBindingRangeBindingCount(rangeIndex));

        applyLeafTypeLayout(containing_layout, rangeIndex, bindingType, draft.Binding);
        const CookError collectAttributesError =
            collectRawSizeAttributes(leafVariable, draft.Binding.Name, draft.Attributes);
        if (collectAttributesError != CookError::Success)
        {
            return collectAttributesError;
        }

        out_drafts.emplace_back(std::move(draft));
    }

    return collectSubObjectDrafts(containing_layout, scope, out_drafts);
}

/** Walks the parameter blocks of one scope, and drafts the container and the contents of each.
 *
 * Slang gives a parent scope only descriptor ranges for a block, so the binding range of the block
 * carries a descriptor set index of -1 and the range walk drops it. The contents live in a sub-object
 * range instead, and this is the only walk that reaches them. `ReadParameterBlock` reads one range,
 * and `ReadBlockContainer` reads the slot Slang adds to hold the ordinary data. */
CookError SlangVariantCompiler::CollectSubObjectDrafts(slang::TypeLayoutReflection* containing_layout,
                                                      const BindingScope& scope,
                                                      std::vector<RawBindingDraft>& out_drafts) const
{
    const SlangInt subObjectRangeCount = containing_layout->getSubObjectRangeCount();

    for (SlangInt subObjectIndex = 0; subObjectIndex < subObjectRangeCount; ++subObjectIndex)
    {
        const std::optional<ParameterBlockInfo> block =
            ReadParameterBlock(containing_layout, subObjectIndex, scope);
        if (!block)
        {
            continue;
        }

        if (block->UniformSize != 0u)
        {
            CookResult<RawBindingDraft> container = ReadBlockContainer(*block, scope.Name);
            if (!container)
            {
                return container.error();
            }

            out_drafts.emplace_back(std::move(container.value()));
        }

        const CookError contentsError =
            collectBindingRangeDrafts(block->ElementLayout, block->Scope, out_drafts);
        if (contentsError != CookError::Success)
        {
            return contentsError;
        }
    }

    return CookError::Success;
}

/** The parameter scope of one entry point: where it starts, and the name Slang emits around it.
 *
 * Slang reports an unresolved offset as `SLANG_UNKNOWN_SIZE`. That answer cannot be a placement, and
 * a cook that carried it would emit a binding at a location no shader holds.
 *
 * The scope name is the parameter's own name when the entry point declares one parameter of a struct
 * type. Slang collects loose `uniform` parameters into a synthetic struct instead, and names that
 * `entryPointParams`. Both answers come from the same call, so neither is a special case here. */
CookResult<BindingScope> SlangVariantCompiler::entryPointScope(slang::EntryPointReflection* entry_point_layout,
                                                              std::string_view entry_point_name) const
{
    slang::VariableLayoutReflection* varLayout = entry_point_layout->getVarLayout();
    if (varLayout == nullptr)
    {
        return BindingScope{};
    }

    const std::optional<uint32_t> group =
        ResolvedNumber(varLayout->getBindingSpace(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT));
    const std::optional<uint32_t> binding =
        ReadOffset(varLayout, SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT);
    const std::optional<uint32_t> spaceBase =
        ReadOffset(varLayout, SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE);
    if (!group || !binding || !spaceBase)
    {
        Diagnostic report;
        report.Severity = DiagnosticSeverity::Error;
        report.Context = "entryPointScope";
        report.Message =
            std::format("entry point {} has an unresolved parameter scope offset", entry_point_name);
        sink->Report(report);
        return std::unexpected(CookError::ReflectionUnavailable);
    }

    return BindingScope{ .Base = BoundPlacement{ .Group = *group, .Binding = *binding },
                         .SpaceBase = *spaceBase,
                         .Name = std::string{ k_EntryPointScopeName } };
}

CookError SlangVariantCompiler::ExtractRawBindings(slang::ProgramLayout* program_layout,
                                                  std::vector<RawBinding>& out_bindings,
                                                  std::vector<RawSizeAttribute>& out_attributes) const
{
    std::vector<RawBindingDraft> drafts;
    const CookError walkError =
        collectBindingRangeDrafts(program_layout->getGlobalParamsTypeLayout(), BindingScope{}, drafts);
    if (walkError != CookError::Success)
    {
        return walkError;
    }

    std::ranges::stable_sort(drafts,
                             PlacementLess,
                             [](const RawBindingDraft& draft) -> const RawPlacement&
                             {
                                 return draft.Binding.Placement;
                             });

    AppendBindingDrafts(drafts, out_bindings, out_attributes);

    return CookError::Success;
}

/** Asks the metadata of one entry point which global bindings that entry point reads.
 *
 * `global_bindings` holds the global scope alone. Slang generates each entry point as its own
 * artifact, so two entry points can place different resources at one group and binding. A placement
 * query over the entry point rows would then let one entry point claim the parameter of another. */
void SlangVariantCompiler::CollectUsedBindingIndices(slang::IComponentType* linked_program,
                                                    SlangInt entry_point_index,
                                                    std::span<const RawBinding> global_bindings,
                                                    std::vector<uint32_t>& out_used_indices) const
{
    Slang::ComPtr<slang::IMetadata> metadata;
    Slang::ComPtr<slang::IBlob> diagnostics;
    if (SLANG_FAILED(linked_program->getEntryPointMetadata(
            entry_point_index, k_WgslTargetIndex, metadata.writeRef(), diagnostics.writeRef())) ||
        metadata == nullptr)
    {
        ReportDiagnostics(*sink, "getEntryPointMetadata", diagnostics.get());
        return;
    }

    auto isUsedLambda = [&metadata](const RawPlacement& placement) -> bool
    {
        const BoundPlacement* boundPlacement = GetBoundPlacement(placement);
        if (boundPlacement == nullptr)
        {
            return false;
        }

        bool isUsed = false;
        metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT,
                                          boundPlacement->Group,
                                          boundPlacement->Binding,
                                          isUsed);
        return isUsed;
    };

    out_used_indices.append_range(std::views::enumerate(global_bindings) |
                                  std::views::filter(
                                      [&](const auto& pair)
                                      {
                                          return isUsedLambda(std::get<1>(pair).Placement);
                                      }) |
                                  std::views::transform(
                                      [](const auto& pair)
                                      {
                                          return std::get<0>(pair);
                                      }));
}

CookResult<RawEntryPoint> SlangVariantCompiler::ExtractRawEntryPoint(
    slang::IComponentType* linked_program,
    slang::ProgramLayout* program_layout,
    SlangInt entry_point_index,
    std::span<const RawBinding> global_bindings,
    std::vector<RawBindingDraft>& out_drafts)
{
    RawEntryPoint rawEntryPoint;
    rawEntryPoint.Name = entryPointNames[static_cast<size_t>(entry_point_index)];

    slang::EntryPointReflection* entryPointLayout =
        program_layout->getEntryPointByIndex(static_cast<SlangUInt>(entry_point_index));
    if (entryPointLayout == nullptr)
    {
        return rawEntryPoint;
    }

    rawEntryPoint.Stage = FromSlangStage(entryPointLayout->getStage());

    if (rawEntryPoint.Stage == ShaderStageKind::Compute)
    {
        std::array<SlangUInt, k_WorkgroupAxisCount> workgroupSizes{ 1u, 1u, 1u };
        entryPointLayout->getComputeThreadGroupSize(k_WorkgroupAxisCount, workgroupSizes.data());
        rawEntryPoint.Workgroup.X = static_cast<uint32_t>(workgroupSizes[0]);
        rawEntryPoint.Workgroup.Y = static_cast<uint32_t>(workgroupSizes[1]);
        rawEntryPoint.Workgroup.Z = static_cast<uint32_t>(workgroupSizes[2]);
    }

    extractRasterState(entryPointLayout, rawEntryPoint.Stage, rawEntryPoint.Raster);

    collectUsedBindingIndices(
        linked_program, entry_point_index, global_bindings, rawEntryPoint.UsedBindingIndices);

    // A `uniform` parameter on the entry point takes a placement in the space the global bindings
    // use, and the entry point var layout says where that scope starts.
    CookResult<BindingScope> scope = entryPointScope(entryPointLayout, rawEntryPoint.Name);
    if (!scope)
    {
        return std::unexpected(scope.error());
    }

    const CookError walkError =
        collectBindingRangeDrafts(entryPointLayout->getTypeLayout(), scope.value(), out_drafts);
    if (walkError != CookError::Success)
    {
        return std::unexpected(walkError);
    }

    return rawEntryPoint;
}

void SlangVariantCompiler::extractRasterState(slang::EntryPointReflection* entry_point_layout,
                                             ShaderStageKind stage,
                                             ReflectedRasterState& raster)
{
    if (stage == ShaderStageKind::Vertex)
    {
        CollectVertexInputs(entry_point_layout->getVarLayout(), raster);
        return;
    }

    if (stage != ShaderStageKind::Fragment)
    {
        return;
    }

    CollectColorTargets(entry_point_layout->getResultVarLayout(), raster);
    CollectDepthWrites(entry_point_layout->getVarLayout(), raster);
}

}