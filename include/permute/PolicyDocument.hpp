#pragma once
#ifndef PERMUTE_POLICY_DOCUMENT_HPP
#define PERMUTE_POLICY_DOCUMENT_HPP
#include "CookerErrors.hpp"
#include "permute/PermutationValue.hpp"
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

// required to enable copy-free hashing of string_views when doing lookups in a map
struct TransparentStringHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

/**@brief Specialization of unordered_map using the above hash to allow for string_view queries
  * without copying the key */
template<typename Value>
using StringMap = std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

/**@brief Values to cook for the specified axis - uses basic typing of TOML values to make sure
 * we construct PermutationValues explicitly, since that tremendously simplifies downstream code */
struct AxisCookValues
{
    std::string Axis;
    std::vector<PermutationValue> Values;
};

/** The cook policy for one module on one target profile. */
struct TargetPolicy
{
    uint32_t MaxVariants{ 0u };
    /** @brief Override values for axes in a modules permutation space */
    std::vector<AxisCookValues> CookValues;
    /**@brief Conditional expression that determines the set of axis values required for cooking.
      * e.g, if we set this to be like WaveOpsEnabled==1, it would *only* cook and build variants
      * where the condition specified by CookIf evaluates to true.*/
    std::string CookIf;
};

/**@brief PolicyInfluence is used to specify that for a given entrypoint, the named axis
 * should have no influence on it's variant count / permutation assignments. */
struct PolicyInfluence
{
    std::string EntryPoint;
    std::string Axis;
    bool IsInert{ false };
};

/**@brief Whole policy for one module: influence statements and one section for each target.*/
struct ModulePolicyEntry
{
    std::vector<PolicyInfluence> ExpectedInfluence;
    StringMap<TargetPolicy> Targets;
};

/**@brief Policy parsing error with location information to make diagnosing/fixing it less painful */
//todo-ship: This needs to either reuse Diagnostic's range object, or have a conversion operator
struct PolicyParseError
{
    std::string Message;
    uint32_t Line{ 0u };
    uint32_t Column{ 0u };
};

template<typename T>
using PolicyDocResult = std::expected<T, PolicyParseError>;

/**@brief A parsed TOML policy file. This is used to specify expected axis influences, cooking policies,
  *and per target overrides for axis values or enable/disable status. 
  *@note All strings are stored and persisted internally, so string_view returns should not pose a problem*/
class PolicyDocument
{
public:
    [[nodiscard]] static PolicyDocResult<PolicyDocument> Load(std::string_view path);
    // reads TOML "file" from memory - mostly used for unit tests so they don't need to have assets
    [[nodiscard]] static PolicyDocResult<PolicyDocument> Parse(std::string_view text);
    /**@brief Finding a policy can fail, which will return `nullptr`: this is expected and find */
    [[nodiscard]] const ModulePolicyEntry* FindModule(std::string_view module_name) const noexcept;
    /**@brief Finds the target-specific policy: if unfound, it will return an empty policy. Downstream
      * code then just uses this to use the axes with their values in the shader source. */
    [[nodiscard]] const TargetPolicy& FindTargetPolicy(std::string_view module_name,
                                                       std::string_view target_name) const noexcept;
    [[nodiscard]] std::span<const PolicyInfluence> ExpectedInfluenceFor(std::string_view module_name) const noexcept;
    /**@brief Verifies that `CookValues` and `CookWhen` entries in the policy are consistent with the 
      * permutation space, i.e. lints the policy for naming and presence of axes and their values. */
    [[nodiscard]] CookError ValidateAgainstSpace(std::string_view module_name,
                                                 const PermutationSpace& space,
                                                 DiagnosticSink& sink) const;

    [[nodiscard]] size_t ModuleCount() const noexcept;

private:
    StringMap<ModulePolicyEntry> modules;
};

} // namespace lodestone

#endif // !PERMUTE_POLICY_DOCUMENT_HPP
