#pragma once
#ifndef LODESTONE_TARGET_SPV_VALIDATOR_HPP
#define LODESTONE_TARGET_SPV_VALIDATOR_HPP
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

struct spv_context_t;

namespace lodestone
{
struct ReflectedBinding;

class SpvValidator final : public ResolvedLibraryValidator
{
public:
    /** @brief `descriptor_heap_set` is the set of the profile's descriptor heap, with the mutable descriptor lowering:
     * samplers at binding 0, combined image samplers at 1, and every other resource at 2. */
    explicit SpvValidator(std::optional<uint32_t> descriptor_heap_set);
    ~SpvValidator() final;
    SpvValidator(const SpvValidator&) = delete;
    SpvValidator& operator=(const SpvValidator&) = delete;
protected:
    [[nodiscard]] CookResult<BindingComparison> validateEntryPoint(std::span<const std::byte> source_code,
                                                                   std::span<const ReflectedBinding*> bindings,
                                                                   DiagnosticSink& sink) const final;
    [[nodiscard]] CookResult<EmittedCodeFacts> readEmittedCodeFacts(std::span<const std::byte> source_code,
                                                                    DiagnosticSink& sink) const override;
private:
    spv_context_t* context{ nullptr };
    std::optional<uint32_t> descriptorHeapSet;
};

}

#endif // !LODESTONE_TARGET_SPV_VALIDATOR_HPP
