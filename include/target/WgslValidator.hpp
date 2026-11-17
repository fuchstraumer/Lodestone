#pragma once
#ifndef LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
#define LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"
#include <cstddef>
#include <memory>
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
    [[nodiscard]] CookResult<std::unique_ptr<const ParsedEmittedCode>> parseEmittedCode(std::span<const std::byte> source_code,
                                                                                         bool validate,
                                                                                         DiagnosticSink& sink) const final;
    [[nodiscard]] BindingComparison compareEmittedCode(const ParsedEmittedCode& parsed,
                                                       std::span<const ReflectedBinding*> bound,
                                                       std::span<const ReflectedBinding*> stored,
                                                       DiagnosticSink& sink) const final;
};

} // namespace lodestone

#endif //!LODESTONE_TARGET_WGSL_BINDING_VALIDATOR_HPP
