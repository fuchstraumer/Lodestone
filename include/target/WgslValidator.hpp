#pragma once
#ifndef LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
#define LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"
#include <cstddef>
#include <span>

namespace lodestone
{

struct ReflectedBinding;
    
class WgslValidator final : public ResolvedLibraryValidator
{
public:
    WgslValidator();
    ~WgslValidator() final;
protected:
    [[nodiscard]] CookResult<BindingComparison> validateEntryPoint(std::span<const std::byte> source_code,
                                                                   std::span<const ReflectedBinding*> bindings,
                                                                   std::span<const ReflectedBinding*> stored,
                                                                   DiagnosticSink& sink) const final;
    [[nodiscard]] CookResult<EmittedCodeFacts> readEmittedCodeFacts(std::span<const std::byte> source_code,
                                                                    DiagnosticSink& sink) const override;
};

} // namespace lodestone

#endif //!LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
