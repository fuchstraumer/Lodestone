#pragma once
#ifndef PERMUTE_POLICY_DOCUMENT_HPP
#define PERMUTE_POLICY_DOCUMENT_HPP
#include "CookerErrors.hpp"
#include "permute/PermutationValue.hpp"
#include "TransparentHash.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lodestone
{

class PermutationSpace;
class DiagnosticSink;


template<typename Value>
using StringMap = std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

struct AxisCookValues
{
    std::string Axis;
    std::vector<PermutationValue> Values;
    /** @brief The cases of an enum axis. The TOML is read before the axes exist, so they stay names until then. */
    std::vector<std::string> CaseNames;
};

struct TargetCookPolicy
{
    uint32_t MaxVariants{ 0u };
    std::vector<AxisCookValues> CookValues;
    /** @brief Only variants where this evaluates true are cooked, such as `WaveOpsEnabled==1`. */
    std::string CookIf;
};

/** @brief One type expression that the policy file adds to an interface axis, such as `Composed<LeafA, LeafB>`. */
struct PolicyInterfaceType
{
    std::string Type;
    /** @brief The modules that the link line imports so that `Type` resolves. */
    std::vector<std::string> Imports;
    /** @brief Where the entry is written. `File` is empty when the text did not come from a file. */
    std::string File;
    uint32_t Line{ 0u };
};

struct ModulePolicyEntry
{
    StringMap<std::vector<std::string>> InertAxesForEntryPoints;
    /** @brief The types of each interface axis, from `[<Module>.axes.<AXIS>]`. */
    StringMap<std::vector<PolicyInterfaceType>> InterfaceAxes;
    StringMap<TargetCookPolicy> Targets;
};

//todo-ship: This needs to either reuse Diagnostic's range object, or have a conversion operator
struct PolicyParseError
{
    std::string Message;
    /** @brief Empty when the text did not come from a file. */
    std::string File;
    CookError Code{ CookError::PolicyDocumentLoadFailed };
    uint32_t Line{ 0u };
    uint32_t Column{ 0u };
};

/** @brief `OtherTargetsOnly` warns: constraining one target and not the others is error-prone. */
enum class TargetPolicyCoverage : uint8_t
{
    Invalid = 0,
    NoModuleEntry, // module has no policy coverage at all
    NoTargetSections, // no per-target policy sections at all
    Present, // (module, target) pair is covered
    OtherTargetsOnly, // only other targets have policy coverage
};

template<typename T>
using PolicyDocResult = std::expected<T, PolicyParseError>;

/** @brief A parsed TOML policy file. It owns its strings. */
class PolicyDocument
{
public:
    [[nodiscard]] static PolicyDocResult<PolicyDocument> Load(std::string_view path);
    /** @brief Loads each file and merges them. A table merges with a table. Any other key that two files both set fails
     * with `PolicyKeyConflict`, and an array is never merged. */
    [[nodiscard]] static PolicyDocResult<PolicyDocument> LoadMerged(std::span<const std::string> paths);
    // from memory, for tests
    [[nodiscard]] static PolicyDocResult<PolicyDocument> Parse(std::string_view text);
    [[nodiscard]] const ModulePolicyEntry* FindModule(std::string_view module_name) const noexcept;
    /** @brief Returns an empty policy when none is found. */
    [[nodiscard]] const TargetCookPolicy& FindTargetPolicy(std::string_view module_name,
                                                       std::string_view target_name) const noexcept;
    /** @brief Returns the types that the policy adds to an axis, or an empty span. */
    [[nodiscard]] std::span<const PolicyInterfaceType> InterfaceTypesForAxis(std::string_view module_name,
                                                                             std::string_view axis_name) const noexcept;
    [[nodiscard]] std::span<const std::string> InertAxesForEntryPoint(std::string_view module_name,
                                                                std::string_view entry_point_name) const noexcept;
    /** @brief Checks every axis name and value in the policy against the space. */
    [[nodiscard]] CookError ValidateAgainstSpace(std::string_view module_name,
                                                 const PermutationSpace& space,
                                                 DiagnosticSink& sink) const;

    [[nodiscard]] size_t ModuleCount() const noexcept;

    [[nodiscard]] TargetPolicyCoverage FindTargetCoverage(std::string_view module_name,
                                                          std::string_view target_name) const noexcept;
    [[nodiscard]] CookError ValidateTargetNames(std::span<const std::string_view> target_names,
                                                DiagnosticSink& sink) const;

private:
    StringMap<ModulePolicyEntry> modules;
};

} // namespace lodestone

#endif // !PERMUTE_POLICY_DOCUMENT_HPP
