#pragma once
#ifndef LODESTONE_TESTS_COOK_COMPARISON_HPP
#define LODESTONE_TESTS_COOK_COMPARISON_HPP
#include "CookerErrors.hpp"
#include "ResourceFlags.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestFormat.hpp"
#include "ShaderManifestLayout.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include <utility>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

/** Cooks a subject and a control written with the resource types themselves, and compares their resources. */
namespace lodestone::tests
{

struct CookedBundle
{
    std::vector<std::byte> Bytes;
    /** `Success` when the cook wrote a manifest, and the error of the cook otherwise. */
    CookError Result{ CookError::Invalid };
};

/** Cooks for each of `target_names` in one run, twice, so that the two cooks must agree byte for byte. */
inline CookedBundle CookForTargets(std::vector<std::filesystem::path> module_paths,
                                   std::optional<std::filesystem::path> policy_file,
                                   std::vector<std::string> target_names)
{
    CookerOptions options;
    options.ModulePaths = std::move(module_paths);
    options.TargetNames = std::move(target_names);
    options.OutputPath = "CookComparison";
    options.PolicyFile = std::move(policy_file);
    options.VerifyDeterministic = true;

    MemoryOutputSink sink;
    CookedBundle result;
    const CookResult<CookStatistics> cooked = RunCook(options, sink);
    result.Result = cooked.has_value() ? CookError::Success : cooked.error();
    const auto manifest = sink.GetArtifacts().find(std::string{ k_ManifestFileName });
    if (manifest != sink.GetArtifacts().end())
    {
        result.Bytes.resize(manifest->second.size());
        std::memcpy(result.Bytes.data(), manifest->second.data(), result.Bytes.size());
    }
    return result;
}

inline CookedBundle CookBothTargets(std::vector<std::filesystem::path> module_paths,
                                    std::optional<std::filesystem::path> policy_file)
{
    return CookForTargets(std::move(module_paths), std::move(policy_file), { "wgsl", "spirv" });
}

/** What a renderer needs to create and bind one resource, without its name: a shim and its control name it
 * differently. */
using ResourceKey = std::tuple<uint32_t,
                               uint32_t,
                               BindingKind,
                               ResourceShape,
                               ResourceAccess,
                               uint32_t,
                               uint32_t,
                               uint64_t,
                               TextureFormat>;

/** Sorted, so that two lists compare by content. */
inline std::vector<ResourceKey> ResourceKeysOf(const manifest::EnvironmentView& environment,
                                               uint32_t variant_index,
                                               uint32_t entry_point)
{
    std::vector<ResourceKey> keys;
    for (const manifest::ResolvedResource resource :
         environment.VariantAt(variant_index).EntryPoint(entry_point).Layout())
    {
        const manifest::PlacementPayload placement = resource.PlacementValue();
        keys.emplace_back(placement.Word0,
                          placement.Word1,
                          resource.Kind(),
                          resource.Shape(),
                          resource.Access(),
                          resource.ElementStride(),
                          resource.ArrayCount(),
                          resource.ByteSize(),
                          resource.StorageFormat());
    }
    std::ranges::sort(keys);
    return keys;
}

/** Sorted, so that two lists compare by content. */
inline std::vector<std::string_view> CapabilityNamesOf(const manifest::EnvironmentView& environment,
                                                       uint32_t variant_index,
                                                       uint32_t entry_point)
{
    const manifest::EntryPointInstanceView instance =
        environment.VariantAt(variant_index).EntryPoint(entry_point);
    std::vector<std::string_view> names;
    for (uint32_t i = 0u; i < environment.CapabilityNames().size(); ++i)
    {
        if (instance.RequiresCapability(i))
        {
            names.push_back(environment.CapabilityName(i));
        }
    }
    std::ranges::sort(names);
    return names;
}

} // namespace lodestone::tests

#endif // !LODESTONE_TESTS_COOK_COMPARISON_HPP
