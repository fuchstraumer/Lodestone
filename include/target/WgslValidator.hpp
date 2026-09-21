#pragma once
#ifndef LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
#define LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"
#include <span>
#include <string_view>

namespace lodestone
{

struct ReflectedBinding;
    
class WgslValidator final : public ResolvedLibraryValidator
{
public:
    WgslValidator();
    ~WgslValidator() override;

    [[nodiscard]] CookResult<BindingComparison> validateEntryPoint(std::string_view source_code,
                                                                   std::span<const ReflectedBinding*> bindings,
                                                                   DiagnosticSink& sink) const final;

};

} // namespace lodestone

#endif //!LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
