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
    std::ranges::sort(declared, {}, [](const DeclaredBinding& binding)
    {
        return std::make_tuple(binding.Group, binding.Binding);
    });

    BindingComparison comparison = CompareBindings(declared, bindings);

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

        if (declared.ArrayCount != reflected.ArrayCount)
        {
            report(std::format("declared as an array of {}, reflection has {}", declared.ArrayCount, reflected.ArrayCount));
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

    void ReportDeclaredOnly(BindingComparison& comparison, const DeclaredBinding& declared)
    {
        comparison.Matches = false;
        comparison.Report += std::format("  spir-v declares set {} binding {} {} : reflection has no binding at that location\n",
                                         declared.Group,
                                         declared.Binding,
                                         declared.Name);
    }

    void ReportReflectedOnly(BindingComparison& comparison, const ReflectedBinding& reflected)
    {
        comparison.Matches = false;
        comparison.Report += std::format("  reflection has set {} binding {} {} : spir-v has no binding at that location\n",
                                         GroupOf(reflected),
                                         BindingOf(reflected),
                                         reflected.Name);
    }

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
