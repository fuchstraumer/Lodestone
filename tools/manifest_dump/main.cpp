#include "JsonWriter.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <fstream>
#include <magic_enum/magic_enum.hpp>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

enum class DumpError : uint8_t
{
    Invalid = 0,
    Success = 1,
    UsageError = 2,
    FileOpenFailed = 3,
    FileReadFailed = 4,
    ManifestOpenFailed = 5,
    JsonUnbalanced = 6,
};

std::string_view ToString(DumpError error) noexcept
{
    return magic_enum::enum_name(error);
}

struct Options
{
    std::filesystem::path ManifestPath;
    std::filesystem::path OutputPath;
    bool Pretty{ true };
    bool WithSources{ false };
};

std::expected<Options, DumpError> ParseOptions(std::span<char*> args) noexcept
{
    Options options;
    for (size_t i = 1u; i < args.size(); i++)
    {
        const std::string_view arg{ args[i] };
        if (arg == "--compact")
        {
            options.Pretty = false;
        }
        else if (arg == "--with-sources")
        {
            options.WithSources = true;
        }
        else if (arg == "-o" || arg == "--output")
        {
            if (i + 1u >= args.size())
            {
                return std::unexpected(DumpError::UsageError);
            }
            options.OutputPath = args[++i];
        }
        else if (options.ManifestPath.empty())
        {
            options.ManifestPath = arg;
        }
        else
        {
            return std::unexpected(DumpError::UsageError);
        }
    }

    if (options.ManifestPath.empty())
    {
        return std::unexpected(DumpError::UsageError);
    }

    return options;
}

std::expected<std::vector<std::byte>, DumpError> ReadFileBytes(const std::filesystem::path& path) noexcept
{
    std::ifstream file{ path, std::ios::binary | std::ios::ate };
    if (!file)
    {
        return std::unexpected(DumpError::FileOpenFailed);
    }

    const std::streamsize size = file.tellg();
    if (size < 0)
    {
        return std::unexpected(DumpError::FileReadFailed);
    }

    file.seekg(0);
    std::vector<std::byte> bytes(static_cast<size_t>(size));
    if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        return std::unexpected(DumpError::FileReadFailed);
    }

    return bytes;
}

namespace manifest = lodestone::manifest;

void WriteProfiles(lodestone::JsonWriter& writer, const manifest::BundleView& bundle) noexcept
{
    writer.Key("profiles");
    writer.BeginArray();
    for (const manifest::Profile& profile : bundle.Profiles())
    {
        writer.BeginObject();
        writer.KeyString("target", bundle.String(profile.TargetNameString));
        writer.KeyString("accessModel", magic_enum::enum_name(profile.AccessModel));
        writer.EndObject();
    }
    writer.EndArray();
}

void WriteEntryPoints(lodestone::JsonWriter& writer, const manifest::ModuleView& module) noexcept
{
    writer.Key("entryPoints");
    writer.BeginArray();
    for (const manifest::EntryPoint& entryPoint : module.EntryPoints())
    {
        writer.BeginObject();
        writer.KeyString("name", module.Bundle().String(entryPoint.NameString));
        writer.KeyString("stage",
                         magic_enum::enum_name(static_cast<lodestone::ShaderStageKind>(entryPoint.Stage)));
        writer.EndObject();
    }
    writer.EndArray();
}

/** The module's axes as the module keys them: only the values its mask selects, in digit order. A named
 * value (a type or an enum case) is written as its name. */
void WriteModuleAxes(lodestone::JsonWriter& writer, const manifest::ModuleView& module) noexcept
{
    writer.Key("axes");
    writer.BeginArray();
    for (uint32_t axisIndex = 0u; axisIndex < module.AxisCount(); axisIndex++)
    {
        const manifest::Axis& axis = module.AxisData(axisIndex);
        const bool namedValues =
            axis.Domain == lodestone::AxisValueDomain::Type || axis.Domain == lodestone::AxisValueDomain::Enum;
        writer.BeginObject();
        writer.KeyString("name", module.Bundle().String(axis.NameString));
        writer.KeyUInt("rootAxis", module.ModuleAxes()[axisIndex].AxisIndex);
        writer.KeyString("domain", magic_enum::enum_name(axis.Domain));
        writer.Key("values");
        writer.BeginArray();
        for (uint32_t digit = 0u; digit < module.AxisValueCount(axisIndex); digit++)
        {
            const lodestone::AxisValueType value = module.AxisValue(axisIndex, digit);
            if (namedValues)
            {
                writer.String(module.Bundle().String(value));
            }
            else
            {
                writer.Int(static_cast<int64_t>(value));
            }
        }
        writer.EndArray();
        writer.EndObject();
    }
    writer.EndArray();
}

void WriteUniformMembers(lodestone::JsonWriter& writer,
                         const manifest::EnvironmentView& view,
                         const manifest::Binding& binding) noexcept
{
    writer.Key("members");
    writer.BeginArray();
    for (const manifest::UniformMember& member : view.UniformMembers(binding))
    {
        writer.BeginObject();
        writer.KeyString("name", view.String(member.NameString));
        writer.KeyUInt("offset", member.Offset);
        writer.KeyUInt("size", member.Size);
        writer.KeyUInt("arrayCount", member.ArrayCount);
        writer.KeyUInt("elementStride", member.ElementStride);
        writer.KeyString("matrixLayout",
                        magic_enum::enum_name(static_cast<lodestone::MatrixLayout>(member.MatrixLayout)));
        writer.EndObject();
    }
    writer.EndArray();
}

void WriteBinding(lodestone::JsonWriter& writer,
                  const manifest::EnvironmentView& view,
                  const manifest::Binding& binding) noexcept
{
    writer.BeginObject();
    writer.KeyString("name", view.String(binding.NameString));
    writer.KeyString("scope", view.String(binding.ScopeString));
    writer.KeyString("placementKind",
                     magic_enum::enum_name(static_cast<lodestone::PlacementKind>(binding.PlacementKind)));
    writer.KeyUInt("placementWord0", binding.Placement.Word0);
    writer.KeyUInt("placementWord1", binding.Placement.Word1);
    writer.KeyString("kind", magic_enum::enum_name(static_cast<lodestone::BindingKind>(binding.Kind)));
    writer.KeyString("shape", magic_enum::enum_name(static_cast<lodestone::ResourceShape>(binding.Shape)));
    writer.KeyBool("isComparisonSampler", binding.IsComparisonSampler);
    writer.KeyString("storageFormat",
                     magic_enum::enum_name(static_cast<lodestone::TextureFormat>(binding.StorageFormat)));
    writer.KeyString(
        "storageAccess",
        magic_enum::enum_name(static_cast<lodestone::ResourceAccess>(binding.Access)));
    writer.KeyUInt("byteSize", binding.ByteSize);
    writer.KeyUInt("elementStride", binding.ElementStride);
    writer.KeyUInt("arrayCount", binding.ArrayCount);
    WriteUniformMembers(writer, view, binding);
    writer.EndObject();
}

void WriteBindingsFlat(lodestone::JsonWriter& writer, const manifest::EnvironmentView& view) noexcept
{
    writer.Key("bindings");
    writer.BeginArray();
    for (const manifest::Binding& binding : view.Bindings())
    {
        WriteBinding(writer, view, binding);
    }
    writer.EndArray();
}

void WriteRaster(lodestone::JsonWriter& writer,
                 const manifest::EnvironmentView& view,
                 uint32_t raster_index) noexcept
{
    writer.Key("raster");
    writer.BeginObject();
    writer.KeyBool("writesFragDepth", view.WritesFragDepth(raster_index));

    writer.Key("vertexInputs");
    writer.BeginArray();
    for (const manifest::VertexInput& input : view.VertexInputs(raster_index))
    {
        writer.BeginObject();
        writer.KeyString("semanticName", view.String(input.SemanticNameString));
        writer.KeyUInt("semanticIndex", input.SemanticIndex);
        writer.KeyUInt("location", input.Location);
        writer.KeyString("scalarType",
                         magic_enum::enum_name(static_cast<lodestone::VertexScalarType>(input.ScalarType)));
        writer.KeyUInt("componentCount", input.ComponentCount);
        writer.EndObject();
    }
    writer.EndArray();

    writer.Key("colorTargets");
    writer.BeginArray();
    for (const manifest::ColorTarget& target : view.ColorTargets(raster_index))
    {
        writer.BeginObject();
        writer.KeyUInt("location", target.Location);
        writer.KeyString("scalarType",
                         magic_enum::enum_name(static_cast<lodestone::VertexScalarType>(target.ScalarType)));
        writer.KeyUInt("componentCount", target.ComponentCount);
        writer.EndObject();
    }
    writer.EndArray();

    writer.EndObject();
}

void WriteFootprint(lodestone::JsonWriter& writer, const manifest::Footprint& footprint) noexcept
{
    writer.Key("footprint");
    writer.BeginObject();
    writer.KeyString("kind", magic_enum::enum_name(static_cast<lodestone::FootprintKind>(footprint.Kind)));

    if (footprint.Kind == static_cast<uint32_t>(lodestone::FootprintKind::Buffer))
    {
        writer.KeyUInt("elementCount", footprint.ElementCount);
    }
    else if (footprint.Kind == static_cast<uint32_t>(lodestone::FootprintKind::Texture))
    {
        writer.KeyUInt("extentX", footprint.ExtentX);
        writer.KeyUInt("extentY", footprint.ExtentY);
        writer.KeyUInt("extentZ", footprint.ExtentZ);
    }

    writer.EndObject();
}

void WriteSlot(lodestone::JsonWriter& writer,
               const manifest::EnvironmentView& view,
               const manifest::Variant& variant,
               const manifest::EntryPoint& entry_point,
               const manifest::EntryPointInstance& slot,
               bool with_sources) noexcept
{
    writer.BeginObject();
    writer.KeyString("entryPoint", view.String(entry_point.NameString));
    writer.KeyString("stage",
                     magic_enum::enum_name(static_cast<lodestone::ShaderStageKind>(entry_point.Stage)));

    writer.Key("workgroup");
    writer.BeginObject();
    writer.KeyUInt("x", slot.WorkgroupX);
    writer.KeyUInt("y", slot.WorkgroupY);
    writer.KeyUInt("z", slot.WorkgroupZ);
    writer.EndObject();

    // Resolved the way a consumer resolves it: visibility names a resource of the variant, and the
    // footprint list of that variant says how much of it.
    const std::span<const uint32_t> resources = view.ResourceList(variant.ResourceListIndex);
    const std::span<const manifest::Footprint> footprints = view.FootprintList(variant.FootprintListIndex);

    writer.Key("layout");
    writer.BeginArray();
    for (const uint32_t local : view.VisibilityList(slot.VisibilityIndex))
    {
        writer.BeginObject();
        writer.Key("resource");
        WriteBinding(writer, view, view.Bindings()[resources[local]]);

        if (local < footprints.size())
        {
            WriteFootprint(writer, footprints[local]);
        }

        writer.EndObject();
    }
    writer.EndArray();

    WriteRaster(writer, view, slot.RasterIndex);

    if (with_sources)
    {
        writer.KeyString("source", view.Source(slot.SourceIndex));
    }

    writer.EndObject();
}

void WriteVariants(lodestone::JsonWriter& writer,
                   const manifest::EnvironmentView& view,
                   bool with_sources) noexcept
{
    const std::span<const lodestone::VariantKey> keys = view.VariantKeys();
    const std::span<const manifest::EntryPoint> entryPoints = view.Module().EntryPoints();
    writer.Key("variants");
    writer.BeginArray();
    for (uint32_t variantIndex = 0u; variantIndex < view.Variants().size(); variantIndex++)
    {
        const manifest::Variant& variant = view.Variants()[variantIndex];
        writer.BeginObject();
        writer.KeyUInt("key", std::to_underlying(keys[variantIndex]));
        writer.KeyString("suffix", view.String(variant.SuffixString));

        writer.Key("activeAxes");
        writer.BeginArray();
        for (uint32_t axisIndex = 0u; axisIndex < view.Module().AxisCount(); axisIndex++)
        {
            if (view.IsAxisActive(variantIndex, axisIndex))
            {
                writer.String(view.String(view.Module().AxisData(axisIndex).NameString));
            }
        }
        writer.EndArray();

        writer.Key("slots");
        writer.BeginArray();
        const std::span<const manifest::EntryPointInstance> slots = view.VariantSlots(variantIndex);
        for (size_t entryPointIndex = 0u; entryPointIndex < entryPoints.size(); entryPointIndex++)
        {
            WriteSlot(writer, view, variant, entryPoints[entryPointIndex], slots[entryPointIndex], with_sources);
        }
        writer.EndArray();

        writer.EndObject();
    }
    writer.EndArray();
}

/** One module: its profile-invariant facts, then one object for each profile that cooked it. */
DumpError WriteModule(lodestone::JsonWriter& writer,
                      const manifest::BundleView& bundle,
                      uint32_t module_index,
                      bool with_sources) noexcept
{
    const manifest::ModuleView module = bundle.Module(module_index);
    writer.BeginObject();
    writer.KeyString("name", module.Name());
    WriteEntryPoints(writer, module);
    WriteModuleAxes(writer, module);

    writer.Key("environments");
    writer.BeginArray();
    for (uint32_t profileIndex = 0u; profileIndex < bundle.Profiles().size(); profileIndex++)
    {
        const manifest::ManifestResult<manifest::EnvironmentView> view =
            bundle.OpenEnvironment(profileIndex, module_index);
        if (!view.has_value() && view.error().Code == manifest::ErrorCode::EnvironmentNotCooked)
        {
            continue;
        }

        if (!view.has_value())
        {
            std::println(stderr,
                         "Failed to open module {} for profile {}: {}",
                         module.Name(),
                         profileIndex,
                         manifest::DescribeShaderManifestError(view.error()));
            return DumpError::ManifestOpenFailed;
        }

        writer.BeginObject();
        writer.KeyString("target", bundle.String(bundle.Profiles()[profileIndex].TargetNameString));
        WriteBindingsFlat(writer, *view);
        WriteVariants(writer, *view, with_sources);
        writer.EndObject();
    }
    writer.EndArray();

    writer.EndObject();
    return DumpError::Success;
}

std::expected<std::string, DumpError> BuildManifestJson(const manifest::BundleView& bundle,
                                                        bool pretty,
                                                        bool with_sources) noexcept
{
    lodestone::JsonWriter writer{ pretty };
    writer.BeginObject();
    WriteProfiles(writer, bundle);

    writer.Key("modules");
    writer.BeginArray();
    for (uint32_t moduleIndex = 0u; moduleIndex < bundle.ModuleCount(); moduleIndex++)
    {
        const DumpError written = WriteModule(writer, bundle, moduleIndex, with_sources);
        if (written != DumpError::Success)
        {
            return std::unexpected(written);
        }
    }
    writer.EndArray();
    writer.EndObject();

    const lodestone::JsonResult<std::string> result = writer.Finish();
    if (!result.has_value())
    {
        return std::unexpected(DumpError::JsonUnbalanced);
    }

    return result.value();
}

int PrintUsage() noexcept
{
    std::println(stderr,
                 "usage: manifest_dump <ShaderLibrary.ldmanifest> [--compact] [--with-sources] "
                 "[-o <output.json>]");
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    const auto optionsResult = ParseOptions(std::span<char*>(argv, static_cast<size_t>(argc)));
    if (!optionsResult.has_value())
    {
        return PrintUsage();
    }
    const Options& options = optionsResult.value();

    const auto bytesResult = ReadFileBytes(options.ManifestPath);
    if (!bytesResult.has_value())
    {
        std::println(
            stderr, "Failed to read '{}': {}", options.ManifestPath.string(), ToString(bytesResult.error()));
        return 1;
    }

    const auto viewResult = manifest::BundleView::Open(bytesResult.value());
    if (!viewResult.has_value())
    {
        std::println(stderr,
                     "Failed to open manifest '{}': {}",
                     options.ManifestPath.string(),
                     manifest::DescribeShaderManifestError(viewResult.error()));
        return 1;
    }

    const auto jsonResult = BuildManifestJson(viewResult.value(), options.Pretty, options.WithSources);
    if (!jsonResult.has_value())
    {
        std::println(stderr, "Failed to build JSON: {}", ToString(jsonResult.error()));
        return 1;
    }

    if (options.OutputPath.empty())
    {
        std::println("{}", jsonResult.value());
        return 0;
    }

    std::ofstream output{ options.OutputPath, std::ios::binary };
    if (!output)
    {
        std::println(stderr, "Failed to open output '{}'", options.OutputPath.string());
        return 1;
    }
    output.write(jsonResult.value().data(), static_cast<std::streamsize>(jsonResult.value().size()));
    return 0;
}
