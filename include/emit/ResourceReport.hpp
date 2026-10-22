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

/** For each resource, which creation fields the shader fixes and which the renderer supplies. Built from the written
 * manifest through the client views, so it reads what a renderer reads. */
namespace lodestone
{

inline constexpr std::string_view k_ResourceReportFileName = "ShaderLibrary.resources.txt";

/** One scope and name, joined over every variant and entry point that reads it. */
struct ResourceReportEntry
{
    std::string ScopeName;
    std::string Name;
    BindingKind Kind{ BindingKind::Invalid };
    /** Each field joins to the source that asks the most of the renderer. */
    manifest::CreationFields Fields;
    std::vector<std::string> FootprintAxes;
    std::vector<std::string> EntryPoints;
    /** Zero when none is declared, or when it varies. */
    uint64_t ElementCount{ 0u };
    uint32_t ExtentX{ 0u };
    uint32_t ExtentY{ 0u };
    uint32_t ExtentZ{ 0u };
};

struct ResourceReportEnvironment
{
    std::string ModuleName;
    std::string TargetName;
    std::vector<ResourceReportEntry> Resources;
};

struct ResourceReport
{
    /** Profile-major. Skipped environments have no entry. */
    std::vector<ResourceReportEnvironment> Environments;
};

CookResult<ResourceReport> BuildResourceReport(std::span<const std::byte> manifest_bytes);

std::string FormatResourceReport(const ResourceReport& report);

} // namespace lodestone

#endif // !LODESTONE_RESOURCE_REPORT_HPP
