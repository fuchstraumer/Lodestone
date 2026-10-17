#pragma once
#ifndef LODESTONE_AXIS_INFLUENCE_HPP
#define LODESTONE_AXIS_INFLUENCE_HPP
#include "CookerErrors.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/** The measured influence of each axis on each entry point of one cooked module (one environment).
 *
 * An axis influences an entry point when two variants that differ only in that axis give two different entry
 * point instances. An instance is everything a consumer reads for one (variant, entry point): the source, the
 * layout (each visible resource with its footprint), the raster state, and the workgroup size. The source
 * alone is not enough: an axis that appears only in a size attribute changes a footprint and leaves the code
 * the same.
 *
 * The measurement reads content and never an interner index, so `--no-dedupe` gives the same answer. The
 * dedupe report, the inert-axis policy check, and the manifest's axis relevance table all read it. */
namespace lodestone
{

struct CookedModule;

enum class AxisInfluence : uint8_t
{
    Invalid = 0,
    /** The axis never changes this entry point's instance. */
    Inert,
    /** The axis changes this entry point's instance. */
    Active,
    /** Too few variants to decide. One axis value cannot show a difference. */
    Undetermined,
};

struct EntryPointInfluence
{
    std::string EntryPointName;
    /** Runs parallel to the module's permutation space. */
    std::vector<AxisInfluence> Axes;
};

struct ModuleInfluence
{
    std::string ModuleName;
    /** Runs parallel to `CookedModule::EntryPoints`. */
    std::vector<EntryPointInfluence> EntryPoints;
};

/**@brief Measures the influence of every axis on every entry point of `module`. Returns an error when an
 * entry point layout cannot be resolved from the module's tables. */
CookResult<ModuleInfluence> ComputeActualInfluence(const CookedModule& module);

/**@brief Returns true when a consumer must pin the axis for the entry point. An undetermined axis counts as
 * relevant: with one cooked value, the pin always resolves, so the conservative answer costs nothing. */
bool IsAxisRelevant(AxisInfluence influence) noexcept;

std::string_view ToString(AxisInfluence influence) noexcept;

} // namespace lodestone

#endif // !LODESTONE_AXIS_INFLUENCE_HPP
