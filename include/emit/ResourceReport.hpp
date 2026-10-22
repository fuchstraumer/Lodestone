#pragma once
#ifndef LODESTONE_RESOURCE_REPORT_HPP
#define LODESTONE_RESOURCE_REPORT_HPP
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifestLayout.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/**
 * The resource report: for each resource of each cooked module, which creation fields the shader fixes,
 * and which the renderer must supply. A shader edit then shows the CPU work it causes as a diff of this file.
 *
 * The report is built from the written manifest through the client views, and never from the cooked model. So it
 * reads what a renderer reads, and `DescribeCreation` runs on real bytes in every cook.
 *
 * The data and its text form are separate. `BuildResourceReport` gives the data, and `FormatResourceReport` writes the
 * text. A JSON form is a second writer over the same data.
 */
namespace lodestone
{

inline constexpr std::string_view k_ResourceReportFileName = "ShaderLibrary.resources.txt";

/** One resource of one environment: one scope and name, joined over every variant and entry point that reads it. */
struct ResourceReportEntry
{
    std::string ScopeName;
    std::string Name;
    BindingKind Kind{ BindingKind::Invalid };
    /** When the records of the resource give different sources for a field, the field holds the source that asks the
     * most of the renderer: `Cpu`, then `ShaderPerVariant`, then `Shader`. */
    manifest::CreationFields Fields;
    /** The axes that change the element count or the extent, by name, in the module's axis order. */
    std::vector<std::string> FootprintAxes;
    /** The entry points that read the resource, in the module's entry point order. */
    std::vector<std::string> EntryPoints;
    /** The element count or extent when every variant gives the same one. Zero when the shader states none, or when it
     * varies. */
    uint64_t ElementCount{ 0u };
    uint32_t ExtentX{ 0u };
    uint32_t ExtentY{ 0u };
    uint32_t ExtentZ{ 0u };
};

/** One module cooked for one profile. */
struct ResourceReportEnvironment
{
    std::string ModuleName;
    std::string TargetName;
    /** Sorted by scope and then by name. */
    std::vector<ResourceReportEntry> Resources;
};

struct ResourceReport
{
    /** Ordered by profile and then by module, as the manifest directory is. A skipped environment has no entry. */
    std::vector<ResourceReportEnvironment> Environments;
};

/**@brief Reads the report from one manifest bundle. Returns `LibraryRoundTripFailed` when the bundle or one of its
 * environments does not open. */
CookResult<ResourceReport> BuildResourceReport(std::span<const std::byte> manifest_bytes);

std::string FormatResourceReport(const ResourceReport& report);

} // namespace lodestone

#endif // !LODESTONE_RESOURCE_REPORT_HPP
