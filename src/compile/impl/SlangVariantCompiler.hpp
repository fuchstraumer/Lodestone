#pragma once
#ifndef LODESTONE_SLANG_VARIANT_COMPILER_HPP
#define LODESTONE_SLANG_VARIANT_COMPILER_HPP
#include "compile/SlangCompiler.hpp"
#include "SlangCompilerTypes.hpp"
#include <slang-com-helper.h>
#include <slang-com-ptr.h>
#include <slang.h>

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

class SlangVariantCompiler
{
public:
    SlangVariantCompiler(const VariantDescriptor& descriptor);

private:

    [[nodiscard]] CookResult<Slang::ComPtr<slang::IComponentType>> linkVariant(
        const PermutationAssignment& assignment) const;
    std::vector<std::string> generateEntryPointCode(slang::IComponentType* linked_program) const;
    
};

} // namespace lodestone

#endif // !LODESTONE_SLANG_VARIANT_COMPILER_HPP
