#include "SlangVariantCompiler.hpp"
#include "CookerErrors.hpp"
#include "SlangModuleContext.hpp"
#include "compile/Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "permute/PermutationAssignment.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <format>
#include <print>
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

std::vector<std::string> GenerateEntryPointCode(SlangModuleContext& context,
                                                Slang::ComPtr<slang::IComponentType> linked_program,
                                                DiagnosticSink& sink)
{
    const size_t entryPointCount = context.EntryPointCount();
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


CookResult<Slang::ComPtr<slang::IComponentType>> LinkVariant(
    SlangModuleContext& context, const VariantDescriptor& descriptor, DiagnosticSink& sink) const
{
    std::vector<slang::IComponentType*> components = context.BaseComponents();
    components.reserve(context.BaseComponents().size() + descriptor.Active.size());

    for (const PermutationBinding& binding : descriptor.Active)
    {
        const std::string variantModuleName = MakeVariantModuleName(binding.Axis->Name, binding.Value);
        const std::string variantModulePath = MakeVariantModulePath(binding.Axis->Name, binding.Value);
        const std::string variantSource = MakeExportedConstantSource(binding.Axis->Name, binding.Value);

        Slang::ComPtr<slang::IBlob> diagnostics;
        slang::IModule* variantModule =
            context.Session()->loadModuleFromSourceString(variantModuleName.c_str(),
                                                          variantModulePath.c_str(),
                                                          variantSource.c_str(),
                                                          diagnostics.writeRef());
        ReportDiagnostics(sink, "loadModuleFromSourceString", diagnostics.get());

        if (variantModule == nullptr)
        {
            return std::unexpected(CookError::VariantModuleCreationFailed);
        }

        components.push_back(variantModule);
    }

    Slang::ComPtr<slang::IBlob> diagnostics;
    Slang::ComPtr<slang::IComponentType> composite;
    context.Session()->createCompositeComponentType(components.data(),
                                                    static_cast<SlangInt>(components.size()),
                                                    composite.writeRef(),
                                                    diagnostics.writeRef());
    ReportDiagnostics(sink, "createCompositeComponentType", diagnostics.get());

    if (composite == nullptr)
    {
        return std::unexpected(CookError::CompositeCreationFailed);
    }

    Slang::ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diagnostics.writeRef())))
    {
        ReportDiagnostics(sink, "link", diagnostics.get());
        return std::unexpected(CookError::LinkFailed);
    }

    return linked;
}

} // namespace

namespace lodestone
{

CookResult<LinkedVariant> SlangVariantCompiler::CompileVariant(SlangModuleContext& context,
                                                               const VariantDescriptor& descriptor,
                                                               DiagnosticSink& sink)
{
}


} // namespace lodestone