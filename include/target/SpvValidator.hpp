#pragma once
#ifndef LODESTONE_TARGET_SPV_VALIDATOR_HPP
#define LODESTONE_TARGET_SPV_VALIDATOR_HPP
#include "CookerErrors.hpp"
#include "target/TargetProfile.hpp"
#include "ShaderManifestFormat.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

struct spv_context_t;

namespace lodestone
{
struct ReflectedBinding;

class SpvValidator final : public ResolvedLibraryValidator
{
public:
    /** @brief `descriptor_heap_set` is the set of the profile's descriptor heap, and `heap_lowering` states the binding of
     * each descriptor type in it. The mutable descriptor lowering has samplers at binding 0, combined image samplers at 1,
     * and every other resource at 2. The per-descriptor-type lowering has one binding for each type. */
    SpvValidator(std::optional<uint32_t> descriptor_heap_set, manifest::DescriptorHeapLowering heap_lowering);
    ~SpvValidator() final;
    SpvValidator(const SpvValidator&) = delete;
    SpvValidator& operator=(const SpvValidator&) = delete;
protected:
    [[nodiscard]] CookResult<std::unique_ptr<const ParsedEmittedCode>> parseEmittedCode(std::span<const std::byte> source_code,
                                                                                         bool validate,
                                                                                         DiagnosticSink& sink) const final;
    [[nodiscard]] BindingComparison compareEmittedCode(const ParsedEmittedCode& parsed,
                                                       std::span<const ReflectedBinding*> bound,
                                                       std::span<const ReflectedBinding*> stored,
                                                       DiagnosticSink& sink) const final;
private:
    spv_context_t* context{ nullptr };
    std::optional<uint32_t> descriptorHeapSet;
    manifest::DescriptorHeapLowering heapLowering;
};

}

#endif // !LODESTONE_TARGET_SPV_VALIDATOR_HPP
