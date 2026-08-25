#pragma once
#ifndef LODESTONE_SLANG_VARIANT_COMPILER_HPP
#define LODESTONE_SLANG_VARIANT_COMPILER_HPP
#include "CookerErrors.hpp"
#include "compile/Diagnostics.hpp"
#include "permute/PermutationSpace.hpp"
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>
#include <string>
#include <vector>

namespace lodestone
{

/**@brief The result of the variant compiler: a fully linked variant, but with
 * none of the reflection completed. This purely exists to invoke slang and
 * retrieve bytecode.
 */
struct LinkedVariant
{
    Slang::ComPtr<slang::IComponentType> LinkedProgram;
    slang::ProgramLayout* ProgramLayout{ nullptr };
    std::vector<std::string> EntryPointStrings;
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
