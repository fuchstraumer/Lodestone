#pragma once
#ifndef PERMUTE_POLICY_DOCUMENT_HPP
#define PERMUTE_POLICY_DOCUMENT_HPP
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lodestone
{

/** A hash that reads any string type. A map keyed by `std::string` then accepts a `std::string_view`
 * query with no copy. The `is_transparent` tag turns on the heterogeneous overloads of
 * `unordered_map::find`, and `std::equal_to<>` supplies the comparison that matches them. */
struct TransparentStringHash
{
    using is_transparent = void;
    [[nodiscard]] size_t operator()(std::string_view text) const noexcept
    {
        return std::hash<std::string_view>{}(text);
    }
};

/** A map from an owned name to a value, queried by a borrowed name. The map owns each key string, so a
 * `std::string_view` this document returns stays valid while the document lives. */
template<typename Value>
using StringMap = std::unordered_map<std::string, Value, TransparentStringHash, std::equal_to<>>;

/** The values to cook for one axis. An axis with no entry cooks every declared value. The values are
 * `int64_t`, which is the type the expression evaluator reads. A boolean lists as 0 or 1. */
struct AxisCookValues
{
    std::string Axis;
    std::vector<int64_t> Values;
};

/** The cook policy for one module on one target profile. */
struct TargetPolicy
{
    /** The variant budget for this target. Zero means no budget. */
    uint32_t MaxVariants{ 0u };
    /** The per-axis value subsets. Empty means every axis cooks every value. */
    std::vector<AxisCookValues> CookValues;
    /** A predicate that removes an assignment from the cook. Empty means no predicate.
     * `EvaluateExpression` reads it, exactly as it reads a `Require`. */
    std::string CookWhen;
};

/** One expected-influence statement, owned. The cooker measures the real influence and compares it
 * against this. `ExpectedAxisInfluence` in `PermutationPolicy.hpp` is the borrowed form the policy
 * check reads today. Step 3 bridges the two. */
struct PolicyInfluence
{
    std::string EntryPoint;
    std::string Axis;
    bool IsInert{ false };
};

/** The whole policy for one module: the influence statements, and one section for each target. */
struct ModulePolicyEntry
{
    std::vector<PolicyInfluence> ExpectedInfluence;
    StringMap<TargetPolicy> Targets;
};

/** A parse failure, with a location a tech artist can act on. `Line` and `Column` are 1-based. A zero
 * means the failure carries no location. This is the minimal form. The semantic checks against the
 * declared axes are step 4. */
struct PolicyParseError
{
    std::string Message;
    uint32_t Line{ 0u };
    uint32_t Column{ 0u };
};

/** The parsed policy file. It owns every string, so a query returns a borrowed view that stays valid
 * for the life of the document.
 *
 * `Load` reads a TOML file through toml++. toml++ lives only in the translation unit that defines
 * `Load`, so no toml++ type appears in this header. This is the facade rule `SlangCompiler.hpp`
 * already follows for Slang. */
class PolicyDocument
{
public:
    [[nodiscard]] static std::expected<PolicyDocument, PolicyParseError> Load(std::string_view path);

    /** Finds one module's policy, or `nullptr` when the file names no such module. The per-target
     * finder with an empty fallback is step 3. */
    [[nodiscard]] const ModulePolicyEntry* FindModule(std::string_view module_name) const noexcept;

    [[nodiscard]] size_t ModuleCount() const noexcept { return modules.size(); }

private:
    StringMap<ModulePolicyEntry> modules;
};

} // namespace lodestone

#endif // !PERMUTE_POLICY_DOCUMENT_HPP
