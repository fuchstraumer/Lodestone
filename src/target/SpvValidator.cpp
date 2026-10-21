#include "target/SpvValidator.hpp"
#include "ShaderLibraryTypes.hpp"
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

namespace
{
    BindingComparison CompareBindings(std::span<const DeclaredBinding> declared,
                                      std::span<const ReflectedBinding*> reflected);
}

SpvValidator::SpvValidator() : context{ spvContextCreate(SPV_ENV_VULKAN_1_2) }
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
                                                               DiagnosticSink& sink) const
{
    ParseState state;
    const CookError parsed = ParseModule(context, source_code, true, state, sink);
    if (parsed != CookError::Success)
    {
        return std::unexpected(parsed);
    }

    std::vector<DeclaredBinding> declared = CollectDeclaredBindings(state);
    // the interface list is not sorted. `bindings` arrives sorted by (group, binding).
    std::ranges::sort(declared, {}, [](const DeclaredBinding& binding)
    {
        return std::make_tuple(binding.Group, binding.Binding);
    });

    BindingComparison comparison = CompareBindings(declared, bindings);
    comparison.Capabilities.reserve(state.Capabilities.size());
    for (const std::string_view capability : state.Capabilities)
    {
        comparison.Capabilities.emplace_back(capability);
    }

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
    return CollectEmittedCodeFacts(state, sink);
}

namespace
{

    /** Shape and access are only compared where both sides record them. SPIR-V only records the shape of
     * images, and only records the access of storage buffers and storage images. Reflection writes ReadOnly
     * on every sampled texture, and SPIR-V has no equivalent for that. */
    bool ShapeIsStated(BindingKind kind) noexcept
    {
        return (kind == BindingKind::Texture) || (kind == BindingKind::StorageTexture);
    }

    bool AccessIsStated(BindingKind kind) noexcept
    {
        return (kind == BindingKind::StorageBuffer) || (kind == BindingKind::StorageTexture);
    }

    /** SPIR-V names scoped variables with dots (`Surface.Material.Albedo`). Reflection joins scopes with
     * underscores, as WGSL does (the scope question is open in `todo.md`). Set and binding already match
     * when this runs, so comparing the last segment is enough to catch a wrong resource at the right
     * location. */
    std::string_view LastNameSegment(std::string_view spirv_name) noexcept
    {
        const size_t lastDot = spirv_name.rfind('.');
        return (lastDot == std::string_view::npos) ? spirv_name : spirv_name.substr(lastDot + 1u);
    }

    void CompareOneBinding(const DeclaredBinding& declared,
                           const ReflectedBinding& reflected,
                           BindingComparison& comparison)
    {
        const auto report = [&declared, &comparison](std::string_view what)
        {
            comparison.Matches = false;
            comparison.Report += std::format("  spir-v declares set {} binding {} {} : {}\n",
                                             declared.Group,
                                             declared.Binding,
                                             declared.Name,
                                             what);
        };

        if (!declared.Unsupported.empty())
        {
            report(declared.Unsupported);
            return;
        }

        const std::string_view declaredName = StripSlangNameMangling(LastNameSegment(declared.Name));
        if (declaredName != reflected.Name)
        {
            report(std::format("reflection has mismatched name \"{}\"", reflected.Name));
        }

        if (declared.Kind != reflected.Kind)
        {
            report(std::format("declared as {}, reflection has kind {}", ToString(declared.Kind), ToString(reflected.Kind)));
            return;
        }

        if (ShapeIsStated(declared.Kind) && (declared.Shape != reflected.Shape))
        {
            report(std::format("declared with shape {}, reflection has shape {}",
                               ToString(declared.Shape),
                               ToString(reflected.Shape)));
        }

        if ((declared.Kind == BindingKind::StorageTexture) && (declared.StorageFormat != reflected.StorageFormat))
        {
            report(std::format("declared with format {}, reflection has format {}",
                               magic_enum::enum_name(declared.StorageFormat),
                               magic_enum::enum_name(reflected.StorageFormat)));
        }

        if (AccessIsStated(declared.Kind) && (declared.Access != reflected.Access))
        {
            report(std::format("declared with access {}, reflection has access {}",
                               magic_enum::enum_name(declared.Access),
                               magic_enum::enum_name(reflected.Access)));
        }
    }

    /** Both lists are sorted by (set, binding), so one pass of two iterators compares them: O(N + M), the
     * same walk as the WGSL validator. */
    BindingComparison CompareBindings(std::span<const DeclaredBinding> declared,
                                      std::span<const ReflectedBinding*> reflected)
    {
        BindingComparison comparison;
        comparison.Matches = true;
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
                CompareOneBinding(declaredBinding, reflectedBinding, comparison);
                ++iterDeclared;
                ++iterReflected;
            }
            else if (declaredTuple < reflectedTuple)
            {
                comparison.Matches = false;
                comparison.Report += std::format("  spir-v declares set {} binding {} {} : reflection has no binding "
                                                 "at that location\n",
                                                 declaredBinding.Group,
                                                 declaredBinding.Binding,
                                                 declaredBinding.Name);
                ++iterDeclared;
            }
            else
            {
                comparison.Matches = false;
                comparison.Report += std::format("  reflection has set {} binding {} {} : spir-v has no binding "
                                                 "at that location\n",
                                                 std::get<0>(reflectedTuple),
                                                 std::get<1>(reflectedTuple),
                                                 reflectedBinding.Name);
                ++iterReflected;
            }
        }

        for (; iterDeclared != declared.end(); ++iterDeclared)
        {
            comparison.Matches = false;
            comparison.Report += std::format("  spir-v declares set {} binding {} {} : reflection has no binding "
                                             "at that location\n",
                                             iterDeclared->Group,
                                             iterDeclared->Binding,
                                             iterDeclared->Name);
        }

        for (; iterReflected != reflected.end(); ++iterReflected)
        {
            const ReflectedBinding& reflectedBinding = **iterReflected;
            comparison.Matches = false;
            comparison.Report += std::format("  reflection has set {} binding {} {} : spir-v has no binding at that "
                                             "location\n",
                                             GroupOf(reflectedBinding),
                                             BindingOf(reflectedBinding),
                                             reflectedBinding.Name);
        }

        return comparison;
    }
}

}
