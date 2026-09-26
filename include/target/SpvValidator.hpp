#pragma once
#ifndef LODESTONE_TARGET_SPV_VALIDATOR_HPP
#define LODESTONE_TARGET_SPV_VALIDATOR_HPP
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"
#include <cstddef>
#include <span>

struct spv_context_t;

namespace lodestone
{
struct ReflectedBinding;

class SpvValidator final : public ResolvedLibraryValidator
{
public:
    SpvValidator();
    ~SpvValidator() final;
    SpvValidator(const SpvValidator&) = delete;
    SpvValidator& operator=(const SpvValidator&) = delete;
protected:
    [[nodiscard]] CookResult<BindingComparison> validateEntryPoint(std::span<const std::byte> source_code,
                                                                   std::span<const ReflectedBinding*> bindings,
                                                                   DiagnosticSink& sink) const final;
private:
    spv_context_t* context{ nullptr };
};

}

#endif // !LODESTONE_TARGET_SPV_VALIDATOR_HPP
