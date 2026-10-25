#pragma once
#ifndef LODESTONE_SLANG_VARIANT_COMPILER_HPP
#define LODESTONE_SLANG_VARIANT_COMPILER_HPP
#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "permute/PermutationSpace.hpp"
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>
#include <cstddef>
#include <vector>

namespace lodestone
{

/** @brief A linked variant and its code, before reflection. */
struct LinkedVariant
{
    Slang::ComPtr<slang::IComponentType> LinkedProgram;
    slang::ProgramLayout* ProgramLayout{ nullptr };
    using EpCode = std::vector<std::byte>;
    /** The base target's code. */
    std::vector<EpCode> EntryPointCode;
    /** The delivered target's code. Empty when the session has the base target only. */
    std::vector<EpCode> DeliveredEntryPointCode;
    /** The base target's metadata. */
    std::vector<Slang::ComPtr<slang::IMetadata>> EntryPointMetadata;
};

class SlangModuleContext;

class SlangVariantCompiler
{
public:
    SlangVariantCompiler() noexcept = default;
    [[nodiscard]] CookResult<LinkedVariant> CompileVariant(SlangModuleContext& context,
                                                           const VariantDescriptor& descriptor,
                                                           DiagnosticSink& sink);
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_VARIANT_COMPILER_HPP
