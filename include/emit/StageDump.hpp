#pragma once
#ifndef LODESTONE_STAGE_DUMP_HPP
#define LODESTONE_STAGE_DUMP_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "driver/CookerOptions.hpp"
#include <span>
#include <string>
#include <string_view>

/** Writes the data between two stages as JSON through the `OutputSink`, so `--verify-deterministic` compares dumps
 * too. Sources appear as an index, a length, and a hash. */
namespace lodestone
{

class PermutationSpace;
struct VariantSet;
struct RawModule;
struct CompiledVariant;
struct InternedModule;
struct CookedModule;

std::string MakeStageDumpFileName(std::string_view module_name, StageDumpKind kind);

std::string DumpPermutationSpace(std::string_view module_name, const PermutationSpace& space);
std::string DumpVariantSet(std::string_view module_name, const VariantSet& variant_set);
std::string DumpRawModule(const RawModule& module);
/** @brief todo-ship: "Resolved" undersells it: this evaluates more than it resolves. */
std::string DumpResolvedModule(std::string_view module_name, std::span<const CompiledVariant> variants);
/** @brief The interners' tables, with the provenance that the freeze discards. */
std::string DumpInternedModule(const InternedModule& module);
std::string DumpCookedModule(const CookedModule& module);
/** Writes each unique source next to a `SourceTable.json`. SPIR-V files contain only words, so tools read them. */
CookError DumpShaderSources(const CookedModule& module,
                            std::string_view subdir,
                            ShaderCodeFormat code_format,
                            class OutputSink& sink);

} // namespace lodestone

#endif // !LODESTONE_STAGE_DUMP_HPP
