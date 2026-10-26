#include "target/SpvValidator.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestFormat.hpp"
#include "target/TargetProfile.hpp"
#include "target/TargetUtils.hpp"
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "model/ShaderDataSchema.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>
#include <magic_enum/magic_enum.hpp>
#include "impl/SpvModuleReader.hpp"

namespace lodestone
{

using spirv::CollectDeclaredBindings;
using spirv::CollectEmittedCodeFacts;
using spirv::DeclaredBinding;
using spirv::ParseModule;
using spirv::ParseState;
using spirv::BlockMember;
using spirv::FindContainerStruct;
using spirv::FindMemberAtOffset;
using spirv::IsDevicePointerType;
using spirv::IsHandleType;
using spirv::StoredValueLocation;

namespace
{
    BindingComparison CompareBindings(std::span<const DeclaredBinding> declared,
                                      std::span<const ReflectedBinding*> reflected,
                                      DiagnosticSink& sink);
    std::vector<BindingMismatch> CompareDescriptorHeap(std::span<const DeclaredBinding> heap,
                                                       std::span<const ReflectedBinding*> reflected,
                                                       uint32_t heap_set);
    // compares the offset of each stored value with the member of its container that starts there
    std::vector<BindingMismatch> CompareStoredValues(const ParseState& parse_state,
                                                     std::span<const ReflectedBinding*> reflected);
}

SpvValidator::SpvValidator(std::optional<uint32_t> descriptor_heap_set)
    : context{ spvContextCreate(SPV_ENV_VULKAN_1_2) },
      descriptorHeapSet{ descriptor_heap_set }
{
}

SpvValidator::~SpvValidator()
{
    if (context != nullptr)
    {
        spvContextDestroy(context);
        context = nullptr;
    }
}

CookResult<BindingComparison> SpvValidator::validateEntryPoint(std::span<const std::byte> source_code,
                                                               std::span<const ReflectedBinding*> bindings,
                                                               std::span<const ReflectedBinding*> stored,
                                                               DiagnosticSink& sink) const
{
    ParseState state;
    const CookError parsed = ParseModule(context, source_code, true, state, sink);
    if (parsed != CookError::Success)
    {
        return std::unexpected(parsed);
    }
    state.DescriptorHeapSet = descriptorHeapSet;

    std::vector<DeclaredBinding> declared = CollectDeclaredBindings(state);
    std::ranges::sort(declared, {}, [](const DeclaredBinding& binding)
    {
        return std::make_tuple(binding.Group, binding.Binding);
    });

    // the descriptor heap is not in reflection, so its variables are compared with the heap lowering instead
    std::vector<BindingMismatch> heapMismatches;
    if (descriptorHeapSet.has_value())
    {
        const auto heapBegin = std::ranges::remove_if(declared, [this](const DeclaredBinding& binding)
        {
            return binding.Group == *descriptorHeapSet;
        });
        heapMismatches = CompareDescriptorHeap(std::span{ heapBegin.begin(), heapBegin.end() }, bindings, *descriptorHeapSet);
        declared.erase(heapBegin.begin(), heapBegin.end());
    }

    BindingComparison comparison = CompareBindings(declared, bindings, sink);
    comparison.Mismatches.append_range(std::move(heapMismatches));
    comparison.Mismatches.append_range(CompareStoredValues(state, stored));

    CookResult<EmittedCodeFacts> facts = CollectEmittedCodeFacts(state, sink);
    if (!facts)
    {
        return std::unexpected(facts.error());
    }
    comparison.Emitted = std::move(*facts);
    return comparison;
}

CookResult<EmittedCodeFacts> SpvValidator::readEmittedCodeFacts(std::span<const std::byte> source_code,
                                                                DiagnosticSink& sink) const
{
    ParseState state;
    const CookError parsed = ParseModule(context, source_code, false, state, sink);
    if (parsed != CookError::Success)
    {
        return std::unexpected(parsed);
    }
    state.DescriptorHeapSet = descriptorHeapSet;
    return CollectEmittedCodeFacts(state, sink);
}

namespace
{

    /** SPIR-V states shape for images only, and access for storage buffers and images only. */
    bool ShapeIsStated(BindingKind kind) noexcept
    {
        return (kind == BindingKind::Texture) || (kind == BindingKind::StorageTexture);
    }

    bool AccessIsStated(BindingKind kind) noexcept
    {
        return (kind == BindingKind::StorageBuffer) || (kind == BindingKind::StorageTexture);
    }

    /** SPIR-V joins scopes with dots, and reflection with underscores (todo.md has the scope question). Placement
     * already matched, so the last segment is enough. */
    std::string_view LastNameSegment(std::string_view spirv_name) noexcept
    {
        const size_t lastDot = spirv_name.rfind('.');
        return (lastDot == std::string_view::npos) ? spirv_name : spirv_name.substr(lastDot + 1u);
    }

    /** Workaround for shader-slang/slang#13895: the direct SPIR-V emitter gives an array of write-only storage images no
     * NonReadable decoration, and a single image gets one. When SlangNonReadableCanaryTest fails, delete this function,
     * its call, `DeclaredBinding::Array`, the sink parameter of `CompareBindings`, and that test. */
    bool IsSlangNonReadableArrayFault(const DeclaredBinding& declared, const ReflectedBinding& reflected) noexcept
    {
        return declared.Kind == BindingKind::StorageTexture &&
               declared.Array &&
               declared.Access == ResourceAccess::ReadWrite &&
               reflected.Access == ResourceAccess::WriteOnly;
    }

    void CompareOneBinding(const DeclaredBinding& declared,
                           const ReflectedBinding& reflected,
                           BindingComparison& comparison,
                           DiagnosticSink& sink)
    {
        const auto record = [&declared, &comparison](BindingMismatchKind kind, std::string emitted, std::string reflected_value)
        {
            comparison.Mismatches.push_back(BindingMismatch{ .Kind = kind,
                                                             .Group = declared.Group,
                                                             .Binding = declared.Binding,
                                                             .Name = std::string{ declared.Name },
                                                             .Emitted = std::move(emitted),
                                                             .Reflected = std::move(reflected_value) });
        };

        if (!declared.Unsupported.empty())
        {
            record(BindingMismatchKind::Unsupported, std::string{ declared.Unsupported }, std::string{});
            return;
        }

        const std::string_view declaredName = StripSlangNameMangling(LastNameSegment(declared.Name));
        if (declaredName != reflected.Name)
        {
            record(BindingMismatchKind::Name, std::string{ declaredName }, reflected.Name);
        }

        if (declared.Kind != reflected.Kind)
        {
            record(BindingMismatchKind::Kind, std::string{ ToString(declared.Kind) }, std::string{ ToString(reflected.Kind) });
            return;
        }

        if (declared.ArrayCount != reflected.ArrayCount)
        {
            record(BindingMismatchKind::ArrayCount, std::to_string(declared.ArrayCount), std::to_string(reflected.ArrayCount));
        }

        if (ShapeIsStated(declared.Kind) && (declared.Shape != reflected.Shape))
        {
            record(BindingMismatchKind::Shape, std::string{ ToString(declared.Shape) }, std::string{ ToString(reflected.Shape) });
        }

        if ((declared.Kind == BindingKind::StorageTexture) && (declared.StorageFormat != reflected.StorageFormat))
        {
            record(BindingMismatchKind::StorageFormat,
                   std::string{ magic_enum::enum_name(declared.StorageFormat) },
                   std::string{ magic_enum::enum_name(reflected.StorageFormat) });
        }

        if (IsSlangNonReadableArrayFault(declared, reflected))
        {
            ReportWarning(sink,
                          std::format("workaround for shader-slang/slang#13895: set {} binding {} {} is an array of write-only storage "
                                      "images with no NonReadable decoration, and reflection's WriteOnly is kept",
                                      declared.Group,
                                      declared.Binding,
                                      declared.Name));
        }
        else if (AccessIsStated(declared.Kind) && (declared.Access != reflected.Access))
        {
            record(BindingMismatchKind::Access,
                   std::string{ magic_enum::enum_name(declared.Access) },
                   std::string{ magic_enum::enum_name(reflected.Access) });
        }
    }

    bool HeapBindingAllows(uint32_t binding, BindingKind kind) noexcept
    {
        switch (binding)
        {
        case manifest::k_MutableHeapSamplerBinding:
            return kind == BindingKind::Sampler;
        case manifest::k_MutableHeapCombinedImageSamplerBinding:
            return kind == BindingKind::CombinedTextureSampler;
        case manifest::k_MutableHeapResourceBinding:
            return kind == BindingKind::Texture || kind == BindingKind::StorageTexture || kind == BindingKind::TexelBuffer ||
                   kind == BindingKind::StorageBuffer || kind == BindingKind::UniformBuffer;
        default:
            return false;
        }
    }

    std::vector<BindingMismatch> CompareStoredValues(const ParseState& parse_state,
                                                     std::span<const ReflectedBinding*> reflected)
    {
        std::vector<BindingMismatch> mismatches;
        for (const ReflectedBinding* binding : reflected)
        {
            const StoredValuePlacement* stored = GetStoredValuePlacement(binding->Placement);
            if (stored == nullptr)
            {
                continue;
            }

            // a container the code removed takes its values out of the visibility, which is no mismatch
            const StoredValueLocation container{ .Container = stored->Container,
                                                 .Group = stored->Block.Group,
                                                 .Binding = stored->Block.Binding };
            const uint32_t structType = FindContainerStruct(parse_state, container);
            if (structType == 0u)
            {
                continue;
            }

            std::string reason;
            const std::optional<BlockMember> member = FindMemberAtOffset(parse_state, structType, stored->ByteOffset);
            const bool typeMatches = member.has_value() && (stored->Kind == PlacementKind::Handle
                                                                ? IsHandleType(parse_state, member->TypeId)
                                                                : IsDevicePointerType(parse_state, member->TypeId));
            if (!member)
            {
                reason = std::format("no member of the container starts at byte {}", stored->ByteOffset);
            }
            else if (member->Name != binding->Name)
            {
                reason = std::format("the member at byte {} is named '{}'", stored->ByteOffset, member->Name);
            }
            else if (!typeMatches)
            {
                reason = std::format("the member at byte {} is not a {}",
                                     stored->ByteOffset,
                                     stored->Kind == PlacementKind::Handle ? "uint2 handle" : "PhysicalStorageBuffer pointer");
            }

            if (!reason.empty())
            {
                mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::StoredValue,
                                                      .Group = stored->Block.Group,
                                                      .Binding = stored->Block.Binding,
                                                      .Name = binding->Name,
                                                      .Emitted = std::move(reason) });
            }
        }

        return mismatches;
    }

    std::vector<BindingMismatch> CompareDescriptorHeap(std::span<const DeclaredBinding> heap,
                                                       std::span<const ReflectedBinding*> reflected,
                                                       uint32_t heap_set)
    {
        std::vector<BindingMismatch> mismatches;
        for (const DeclaredBinding& variable : heap)
        {
            std::string reason;
            if (!variable.Array || variable.ArrayCount != 0u)
            {
                reason = "a variable in the descriptor heap set that is not an unbounded array";
            }
            else if (!HeapBindingAllows(variable.Binding, variable.Kind))
            {
                reason = std::format("a heap of {} at a binding that the mutable descriptor lowering gives to other kinds",
                                     ToString(variable.Kind));
            }

            if (!reason.empty())
            {
                mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::DescriptorHeap,
                                                      .Group = variable.Group,
                                                      .Binding = variable.Binding,
                                                      .Name = std::string{ variable.Name },
                                                      .Emitted = std::move(reason),
                                                      .Reflected = {} });
            }
        }

        for (const ReflectedBinding* binding : reflected)
        {
            if (GroupOf(*binding) == heap_set)
            {
                mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::DescriptorHeap,
                                                      .Group = GroupOf(*binding),
                                                      .Binding = BindingOf(*binding),
                                                      .Name = binding->Name,
                                                      .Emitted = "reflection places a resource in the descriptor heap set",
                                                      .Reflected = {} });
            }
        }

        return mismatches;
    }

    void ReportDeclaredOnly(BindingComparison& comparison, const DeclaredBinding& declared)
    {
        comparison.Mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::EmittedOnly,
                                                         .Group = declared.Group,
                                                         .Binding = declared.Binding,
                                                         .Name = std::string{ declared.Name },
                                                         .Emitted = {},
                                                         .Reflected = {} });
    }

    void ReportReflectedOnly(BindingComparison& comparison, const ReflectedBinding& reflected)
    {
        comparison.Mismatches.push_back(BindingMismatch{ .Kind = BindingMismatchKind::ReflectedOnly,
                                                         .Group = GroupOf(reflected),
                                                         .Binding = BindingOf(reflected),
                                                         .Name = reflected.Name,
                                                         .Emitted = {},
                                                         .Reflected = {} });
    }

    BindingComparison CompareBindings(std::span<const DeclaredBinding> declared,
                                      std::span<const ReflectedBinding*> reflected,
                                      DiagnosticSink& sink)
    {
        BindingComparison comparison;
        auto iterDeclared = declared.begin();
        auto iterReflected = reflected.begin();

        while ((iterDeclared != declared.end()) && (iterReflected != reflected.end()))
        {
            const DeclaredBinding& declaredBinding = *iterDeclared;
            const ReflectedBinding& reflectedBinding = **iterReflected;
            const auto declaredTuple = std::make_tuple(declaredBinding.Group, declaredBinding.Binding);
            const auto reflectedTuple = std::make_tuple(GroupOf(reflectedBinding), BindingOf(reflectedBinding));

            if (declaredTuple == reflectedTuple)
            {
                CompareOneBinding(declaredBinding, reflectedBinding, comparison, sink);
                ++iterDeclared;
                ++iterReflected;
            }
            else if (declaredTuple < reflectedTuple)
            {
                ReportDeclaredOnly(comparison, declaredBinding);
                ++iterDeclared;
            }
            else
            {
                ReportReflectedOnly(comparison, reflectedBinding);
                ++iterReflected;
            }
        }

        for (; iterDeclared != declared.end(); ++iterDeclared)
        {
            ReportDeclaredOnly(comparison, *iterDeclared);
        }

        for (; iterReflected != reflected.end(); ++iterReflected)
        {
            ReportReflectedOnly(comparison, **iterReflected);
        }

        return comparison;
    }
}

}
