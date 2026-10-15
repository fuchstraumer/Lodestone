#pragma once
#ifndef LODESTONE_STAGE_DUMP_HPP
#define LODESTONE_STAGE_DUMP_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "driver/CookerOptions.hpp"
#include <span>
#include <string>
#include <string_view>

/** Writes the data between two stages as JSON, so that a change to a stage shows up as a diff.
 *
 * Dumps are written through the `OutputSink` like every other artifact. This means:
 *
 * - `MemoryOutputSink` captures dumps, so tests can read them without touching the disk.
 * - `--verify-deterministic` compares every artifact in the sink, including the dumps, so an unordered
 *   container that reaches a dump fails the cook.
 * - Dump files are named the same way as every other artifact.
 *
 * Dumps contain no shader source code. Each source is written as an index, a byte length, and a content
 * hash.
 * That's all we need to diff and verify idempotence */
namespace lodestone
{

class PermutationSpace;
struct VariantSet;
struct RawModule;
struct CompiledVariant;
struct InternedModule;
struct CookedModule;

/** `<module>.stage-<name>.json` */
std::string MakeStageDumpFileName(std::string_view module_name, StageDumpKind kind);

/**@brief The evaluated and expanded permutation axes (space) the module declares, and the indices these will map to */
std::string DumpPermutationSpace(std::string_view module_name, const PermutationSpace& space);
/**@brief Every single variant we have, with it's active permutation values and it's fully expanded canonical
 * permutation space */
std::string DumpVariantSet(std::string_view module_name, const VariantSet& variant_set);
/**@brief "Raw" here means just what came out of Slang, exactly as it is. We have not yet evaluated
 * or collapsed our meta-language attributes like vx_size etc */
std::string DumpRawModule(const RawModule& module);
/**@brief The same as `DumpRawModule`, but with our various meta-attributes in our DSL evaluated
 * todo-ship: Resolved may be overloaded or misleading, I might change that. We really do evaluate more than
 * resolve */
std::string DumpResolvedModule(std::string_view module_name, std::span<const CompiledVariant> variants);
/**@brief The tables while they are still in the interners, plus the provenance that the freeze
 * discards. `cooked` shows what collapsed as a whole; this shows where each collapsed item came from */
std::string DumpInternedModule(const InternedModule& module);
/** @brief The frozen tables, indices used to key into each table, and the measurements from the interner per
 *  table type (measures collapse/dedupe efficiency) */
std::string DumpCookedModule(const CookedModule& module);
/** @brief Dumps the shader source strings to the given subdir under the output sink, keyed by the 
  * module name on the input cooked module. */
/** Writes each unique source of one module to `subdir`, next to a `SourceTable.json`. WGSL files start with
 * a comment that lists their variants. SPIR-V files contain only the words, so that tools can read them. */
CookError DumpShaderSources(const CookedModule& module,
                            std::string_view subdir,
                            ShaderCodeFormat code_format,
                            class OutputSink& sink);

} // namespace lodestone

#endif // !LODESTONE_STAGE_DUMP_HPP
