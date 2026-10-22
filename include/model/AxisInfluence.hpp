#pragma once
#ifndef LODESTONE_AXIS_INFLUENCE_HPP
#define LODESTONE_AXIS_INFLUENCE_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/** The measured influence of each axis on each entry point instance of one cooked module, read from content and
 * never from interner indices, so that `--no-dedupe` gives the same answer. */
namespace lodestone
{

class DiagnosticSink;

struct CookedModule;

enum class AxisInfluence : uint8_t
{
    Invalid = 0,
    Inert,
    Active,
    /** One cooked value cannot show a difference. */
    Undetermined,
};

struct EntryPointInfluence
{
    std::string EntryPointName;
    std::vector<AxisInfluence> Axes;
};

struct ModuleInfluence
{
    std::string ModuleName;
    std::vector<EntryPointInfluence> EntryPoints;
};

/** @brief Measures the two passes: one-axis pairs, then the closure that `SingleFor` rests on. */
CookResult<ModuleInfluence> ComputeActualInfluence(const CookedModule& module);

/** @brief The influence of each axis on one resource's element count or extent. Variants that do not declare the
 * resource do not take part. */
struct ResourceFootprintInfluence
{
    std::string ScopeName;
    std::string Name;
    /** The first pass alone: each Active axis is proven by a one-axis pair. The validator reads this. */
    std::vector<AxisInfluence> PairAxes;
    /** The closed set, Active or Inert. The manifest stores this. */
    std::vector<AxisInfluence> Axes;
};

/** @brief Sorted by scope and name. Undetermined counts as Inert before the closing pass. */
std::vector<ResourceFootprintInfluence> ComputeFootprintInfluence(const CookedModule& module);

/** @brief Fails when a proven footprint axis is not named by the resource's size attributes. */
CookError VerifyFootprintAxes(const CookedModule& module, DiagnosticSink& sink);

/** @brief One mask per `CookedModule::Resources` record, `word_count` words each. */
std::vector<uint64_t> BuildFootprintAxisMasks(const CookedModule& module, size_t word_count);

/** @brief Undetermined counts as relevant: with one cooked value, the pin always resolves. */
bool IsAxisRelevant(AxisInfluence influence) noexcept;

std::string_view ToString(AxisInfluence influence) noexcept;

} // namespace lodestone

#endif // !LODESTONE_AXIS_INFLUENCE_HPP
