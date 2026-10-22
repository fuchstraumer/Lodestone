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
 * point instances, or when the closing pass below needs it. An instance is everything a consumer reads for
 * one (variant, entry point): the source, the layout (each visible resource with its footprint), the raster
 * state, and the workgroup size. The source alone is not enough: an axis that appears only in a size
 * attribute changes a footprint and leaves the code the same.
 *
 * The measurement has two passes. The first compares variants that differ in one axis. The second closes the
 * result: variants that agree on every relevant axis must hold one instance, and a sparse cook can leave two
 * such variants with no chain of one-axis steps between them, so the second pass marks more axes relevant
 * until that holds. `ManifestQueryBuilder::SingleFor` rests on that property.
 *
 * The measurement reads content and never an interner index, so `--no-dedupe` gives the same answer. The
 * dedupe report, the inert-axis policy check, and the manifest's axis relevance table all read it. */
namespace lodestone
{

class DiagnosticSink;

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

/**@brief The measured influence of each axis on the footprint of one resource: its element count, or its
 * extent. Resources are named by scope because anything that varies has several entries under the same scope.
 * Variants that don't declare resources - due to pruning or other reasons - do not contribute to the measurement. */
struct ResourceFootprintInfluence
{
    std::string ScopeName;
    std::string Name;
    /** Same as the existing axis influence measurement: only active when two variants that differ in that axis alone give two footprints. */
    std::vector<AxisInfluence> PairAxes;
    /** The closed set of axes: Active or Inert, never Undetermined. Variants that agree on every Active axis give one footprint. */
    std::vector<AxisInfluence> Axes;
};

/**@brief Measures the influence of every axis on the footprint of every resource of `module`.
  * Runs as two passes: the first pass is the same as `ComputeActualInfluence`, measuring one-axis differences.
  * The second pass closes the set of axes, marking any remaining Undetermined axes as Inert if they do not change the footprint.
  * @return The measured footprint influence for each resource, sorted by scope and name */
std::vector<ResourceFootprintInfluence> ComputeFootprintInfluence(const CookedModule& module);

/**@brief Compares the measured footprint influence of `module` with the axes that its size attributes name */
CookError VerifyFootprintAxes(const CookedModule& module, DiagnosticSink& sink);

/**@brief Returns one footprint axis mask for each record of `CookedModule::Resources`, in the word layout of
 * the axis mask table (`word_count` words each). Bit k is set when axis k is measured as Active */
std::vector<uint64_t> BuildFootprintAxisMasks(const CookedModule& module, size_t word_count);

/**@brief Returns true when a consumer must pin the axis for the entry point. An undetermined axis counts as
 * relevant: with one cooked value, the pin always resolves, so the conservative answer costs nothing. */
bool IsAxisRelevant(AxisInfluence influence) noexcept;

std::string_view ToString(AxisInfluence influence) noexcept;

} // namespace lodestone

#endif // !LODESTONE_AXIS_INFLUENCE_HPP
