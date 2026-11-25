#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestLayout.hpp"
#include "emit/OutputSink.hpp"

#include LODESTONE_HOST_HEADER_MAIN
#include LODESTONE_HOST_HEADER_SHIM

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The generated headers hold static asserts that compare the layout of the host compiler with the layout that Slang
// gave the cooker. That comparison runs when this file compiles. This test then compares the generated types with the
// member records of the written manifest, which a different code path produced. Both manifests come from build-time cooks
// (tests/CMakeLists.txt), and the cooks write the headers that this file includes.

namespace
{

using namespace lodestone;

namespace main_cook = HostHeaderCook;
namespace shim_cook = HostHeaderShimCook;

struct MemberOffset
{
    std::string_view Path;
    size_t Offset;
};

std::vector<std::byte> ReadFileBytes(const std::filesystem::path& file_path)
{
    std::ifstream stream{ file_path, std::ios::binary };
    std::vector<char> characters{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
    std::vector<std::byte> bytes(characters.size());
    std::memcpy(bytes.data(), characters.data(), bytes.size());

    return bytes;
}

std::string ReadFileText(const std::filesystem::path& file_path)
{
    std::ifstream stream{ file_path, std::ios::binary };
    std::stringstream text;
    text << stream.rdbuf();

    return text.str();
}

std::optional<manifest::ResolvedResource> FindResource(const manifest::LayoutRange& layout, std::string_view name)
{
    for (const manifest::ResolvedResource resource : layout)
    {
        if (resource.Name() == name)
        {
            return resource;
        }
    }

    return std::nullopt;
}

void CheckMembers(tests::TestRunner& runner,
                  const manifest::UniformMemberRange& members,
                  std::initializer_list<MemberOffset> expected)
{
    for (const MemberOffset& member : expected)
    {
        const std::optional<manifest::UniformMemberView> record = members.Find(member.Path);
        if (runner.Check(record.has_value(), std::format("the manifest has the member '{}'", member.Path)))
        {
            runner.CheckEqual(static_cast<size_t>(record->Offset()),
                              member.Offset,
                              std::format("the offset of '{}' in the generated type", member.Path));
        }
    }
}

/** The environment and the layout of one entry point, in the first variant. */
struct Located
{
    manifest::BundleView Bundle;
    manifest::ManifestResult<manifest::EnvironmentView> Environment;
    uint32_t EntryPoint{ 0u };
};

std::optional<Located> Locate(tests::TestRunner& runner,
                              const manifest::BundleView& bundle,
                              uint32_t profile_index,
                              std::string_view module_name,
                              std::string_view entry_point_name)
{
    const int32_t moduleIndex = bundle.FindModule(module_name);
    if (!runner.Check(moduleIndex >= 0, std::format("the manifest has module '{}'", module_name)))
    {
        return std::nullopt;
    }

    const manifest::ManifestResult<uint32_t> entryPoint =
        bundle.Module(static_cast<uint32_t>(moduleIndex)).FindEntryPointIndex(entry_point_name);
    if (!runner.CheckOk(entryPoint, std::format("the module has entry point '{}'", entry_point_name)))
    {
        return std::nullopt;
    }

    Located located{ .Bundle = bundle,
                     .Environment = bundle.OpenEnvironment(profile_index, static_cast<uint32_t>(moduleIndex)),
                     .EntryPoint = *entryPoint };
    if (!runner.CheckOk(located.Environment, "the environment opens"))
    {
        return std::nullopt;
    }

    return located;
}

manifest::EntryPointInstanceView InstanceOf(const Located& located)
{
    const manifest::VariantView variant = located.Environment->VariantAt(0u);

    return variant.EntryPoint(located.EntryPoint);
}

/** A constant buffer holds one struct: its size is the size of the type, and each member is where the type puts it. */
template <class HostType>
void CheckUniformBlock(tests::TestRunner& runner,
                       const Located& located,
                       std::string_view binding_name,
                       std::initializer_list<MemberOffset> members)
{
    const auto context = runner.Context(std::format("constant buffer '{}'", binding_name));
    const manifest::EntryPointInstanceView instance = InstanceOf(located);
    const manifest::LayoutRange layout = instance.Layout();
    const std::optional<manifest::ResolvedResource> resource = FindResource(layout, binding_name);
    if (runner.Check(resource.has_value(), "the entry point uses the binding"))
    {
        runner.CheckEqual(resource->ByteSize(), static_cast<uint64_t>(sizeof(HostType)), "the block size is sizeof of the type");
        CheckMembers(runner, resource->Members(), members);
    }
}

/** A structured buffer holds an array of one struct: the element stride is the size of the type. */
template <class HostType>
void CheckStorageElement(tests::TestRunner& runner,
                         const Located& located,
                         std::string_view binding_name,
                         std::initializer_list<MemberOffset> members)
{
    const auto context = runner.Context(std::format("structured buffer '{}'", binding_name));
    const manifest::EntryPointInstanceView instance = InstanceOf(located);
    const manifest::LayoutRange layout = instance.Layout();
    const std::optional<manifest::ResolvedResource> resource = FindResource(layout, binding_name);
    if (runner.Check(resource.has_value(), "the entry point uses the binding"))
    {
        runner.CheckEqual(resource->ElementStride(), static_cast<uint32_t>(sizeof(HostType)), "the element stride is sizeof of the type");
        CheckMembers(runner, resource->Members(), members);
    }
}

template <class HostType>
void CheckConstants(tests::TestRunner& runner, const Located& located, std::initializer_list<MemberOffset> members)
{
    const auto context = runner.Context("entry point constants");
    const manifest::EntryPointInstanceView instance = InstanceOf(located);
    const manifest::EntryPointConstantsView constants = instance.Constants();
    runner.CheckEqual(constants.ByteSize(), static_cast<uint32_t>(sizeof(HostType)), "the constants size is sizeof of Params");
    CheckMembers(runner, constants.Members(), members);
}

template <main_cook::CookProfile Profile>
void CheckHostTypesModule(tests::TestRunner& runner, const manifest::BundleView& bundle)
{
    constexpr auto k_Std140 = LayoutRule::Std140;
    constexpr auto k_Std430 = LayoutRule::Std430;
    using main_cook::CookProfile;
    constexpr uint32_t profileIndex = static_cast<uint32_t>(Profile);

    runner.CheckEqual(bundle.FindProfile(Profile == CookProfile::Wgsl ? "wgsl" : "spirv"),
                      static_cast<int32_t>(profileIndex),
                      "the enumerator value is the index of the profile in the manifest");

    // a comma between template arguments would split the arguments of offsetof, so each type has a name
    using Scene = main_cook::Scene<Profile, k_Std140>;
    using SceneLight = main_cook::Light<Profile, k_Std140>;
    using Light = main_cook::Light<Profile, k_Std430>;
    using GridBlock = main_cook::Grid<Profile, k_Std140>;
    using GridBuffer = main_cook::Grid<Profile, k_Std430>;
    using FalloffBlock = main_cook::Falloff<Profile, k_Std140>;
    using FalloffBuffer = main_cook::Falloff<Profile, k_Std430>;
    constexpr size_t keyOffset = offsetof(Scene, key);
    if (const std::optional<Located> located = Locate(runner, bundle, profileIndex, "HostTypes", "SceneCS"))
    {
        CheckUniformBlock<Scene>(runner,
                                 *located,
                                 main_cook::HostTypes::SceneCS::scene,
                                 { { "world", offsetof(Scene, world) },
                                   { "skew", offsetof(Scene, skew) },
                                   { "key.position", keyOffset + offsetof(SceneLight, position) },
                                   { "key.range", keyOffset + offsetof(SceneLight, range) },
                                   { "tints", offsetof(Scene, tints) },
                                   { "flags", offsetof(Scene, flags) } });
        CheckUniformBlock<GridBlock>(runner,
                                     *located,
                                     main_cook::HostTypes::SceneCS::gridBlock,
                                     { { "cells", offsetof(GridBlock, cells) }, { "tag", offsetof(GridBlock, tag) } });
        CheckStorageElement<GridBuffer>(runner,
                                        *located,
                                        main_cook::HostTypes::SceneCS::gridBuffer,
                                        { { "cells", offsetof(GridBuffer, cells) }, { "tag", offsetof(GridBuffer, tag) } });
        CheckUniformBlock<FalloffBlock>(
            runner,
            *located,
            main_cook::HostTypes::SceneCS::falloffBlock,
            { { "samples", offsetof(FalloffBlock, samples) }, { "bias", offsetof(FalloffBlock, bias) } });
        CheckStorageElement<FalloffBuffer>(
            runner,
            *located,
            main_cook::HostTypes::SceneCS::falloffBuffer,
            { { "samples", offsetof(FalloffBuffer, samples) }, { "bias", offsetof(FalloffBuffer, bias) } });
        CheckStorageElement<Light>(runner,
                                   *located,
                                   main_cook::HostTypes::SceneCS::lights,
                                   { { "position", offsetof(Light, position) }, { "range", offsetof(Light, range) } });
    }

    using ParamsType = main_cook::HostTypes::ParamsCS::Params<Profile>;
    if (const std::optional<Located> located = Locate(runner, bundle, profileIndex, "HostTypes", "ParamsCS"))
    {
        CheckConstants<ParamsType>(runner,
                                   *located,
                                   { { "weights", offsetof(ParamsType, weights) }, { "count", offsetof(ParamsType, count) } });
    }

    using NamedType = main_cook::HostTypes::NamedCS::Params<Profile>;
    if (const std::optional<Located> located = Locate(runner, bundle, profileIndex, "HostTypes", "NamedCS"))
    {
        CheckConstants<NamedType>(
            runner,
            *located,
            { { "light.position", offsetof(NamedType, light) + offsetof(main_cook::detail::Light_0, position) },
              { "light.range", offsetof(NamedType, light) + offsetof(main_cook::detail::Light_0, range) } });
    }

    using Packed = main_cook::Packed<Profile, k_Std140>;
    if (const std::optional<Located> located = Locate(runner, bundle, profileIndex, "HostTypesHalf", "PackedCS"))
    {
        CheckUniformBlock<Packed>(runner,
                                  *located,
                                  main_cook::HostTypesHalf::PackedCS::packed,
                                  { { "scale", offsetof(Packed, scale) },
                                    { "uv", offsetof(Packed, uv) },
                                    { "tag", offsetof(Packed, tag) } });
    }

    using Generic = main_cook::HasIf_vector_float_4<Profile, k_Std140>;
    if (const std::optional<Located> located = Locate(runner, bundle, profileIndex, "HostTypesVariants", "VariantCS"))
    {
        CheckUniformBlock<Generic>(runner,
                                   *located,
                                   main_cook::HostTypesVariants::VariantCS::generic,
                                   { { "value", offsetof(Generic, value) }, { "pad", offsetof(Generic, pad) } });

        // two variants: the binding exists in the variant that turns the axis on, and in no other
        uint32_t variantsWithTint = 0u;
        for (uint32_t variantIndex = 0u; variantIndex < located->Environment->Variants().size(); ++variantIndex)
        {
            const manifest::VariantView variant = located->Environment->VariantAt(variantIndex);
            const manifest::LayoutRange layout = variant.EntryPoint(located->EntryPoint).Layout();
            variantsWithTint += FindResource(layout, main_cook::HostTypesVariants::VariantCS::SomeVariants::tint).has_value() ? 1u : 0u;
        }

        runner.CheckEqual(variantsWithTint, 1u, "the binding of SomeVariants exists in one of two variants");
        runner.CheckEqual(located->Environment->Variants().size(), size_t{ 2u }, "the module has two variants");
    }
}

void CheckMainCook(tests::TestRunner& runner)
{
    const std::vector<std::byte> bytes = ReadFileBytes(LODESTONE_HOST_MANIFEST_MAIN);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (!runner.CheckOk(bundle, "the manifest of the main cook opens"))
    {
        return;
    }

    runner.BeginSection("wgsl: generated types match the member records of the manifest");
    CheckHostTypesModule<main_cook::CookProfile::Wgsl>(runner, *bundle);

    runner.BeginSection("spirv: generated types match the member records of the manifest");
    CheckHostTypesModule<main_cook::CookProfile::Spirv>(runner, *bundle);
}

void CheckSelectionByProfileAndRule(tests::TestRunner& runner)
{
    using main_cook::CookProfile;
    constexpr auto k_Std140 = LayoutRule::Std140;
    constexpr auto k_Std430 = LayoutRule::Std430;

    runner.BeginSection("a type with one layout is one C++ type for every profile and rule");
    runner.Check(std::is_same_v<main_cook::Light<CookProfile::Wgsl, k_Std140>, main_cook::Light<CookProfile::Spirv, k_Std140>>,
                 "Light has one type on wgsl and spirv");
    runner.Check(std::is_same_v<main_cook::Light<CookProfile::Spirv, k_Std140>, main_cook::Light<CookProfile::Spirv, k_Std430>>,
                 "Light has equal layouts in both rules, so one type");

    runner.BeginSection("a type with two layouts is two C++ types");
    runner.Check(!std::is_same_v<main_cook::Falloff<CookProfile::Spirv, k_Std140>, main_cook::Falloff<CookProfile::Spirv, k_Std430>>,
                 "Falloff differs between std140 and std430");
    runner.Check(std::is_same_v<main_cook::Falloff<CookProfile::Wgsl, k_Std140>, main_cook::Falloff<CookProfile::Spirv, k_Std140>>,
                 "the std140 layout of Falloff is one type on both profiles");
    runner.CheckEqual(sizeof(main_cook::Falloff<CookProfile::Spirv, k_Std140>), size_t{ 64u }, "Falloff in std140");
    runner.CheckEqual(sizeof(main_cook::Falloff<CookProfile::Spirv, k_Std430>), size_t{ 16u }, "Falloff in std430");

    runner.BeginSection("the params of one entry point follow the delivery of the profile");
    runner.Check(!std::is_same_v<main_cook::HostTypes::ParamsCS::Params<CookProfile::Spirv>,
                                 main_cook::HostTypes::ParamsCS::Params<CookProfile::Wgsl>>,
                 "a push constant block and a uniform buffer are two types");
    runner.CheckEqual(sizeof(main_cook::HostTypes::ParamsCS::Params<CookProfile::Spirv>), size_t{ 16u }, "std430 params");
    runner.CheckEqual(sizeof(main_cook::HostTypes::ParamsCS::Params<CookProfile::Wgsl>), size_t{ 64u }, "std140 params");

    runner.BeginSection("the binding names are the names of the shader");
    runner.CheckEqual(main_cook::HostTypes::SceneCS::scene, std::string_view{ "scene" }, "a constant buffer");
    runner.CheckEqual(main_cook::HostTypesVariants::VariantCS::SomeVariants::tint, std::string_view{ "tint" }, "a binding of some variants");

    runner.BeginSection("the header names the axes that change a binding");
    const std::string header = ReadFileText(LODESTONE_HOST_HEADER_MAIN);
    runner.Check(header.contains("present or absent with the axes: USE_TINT"), "the comment of tint names the axis");
    runner.Check(header.contains("// Slang type: HasIf<vector<float,4>>"),
                 "a generic struct keeps its full Slang name in a comment, and its identifier is a C++ name");

    runner.BeginSection("the uniform buffer of the entry point constants is Params, and has no binding constant");
    runner.Check(!header.contains("entryPointParams"), "no constant names the container of the constants");
    runner.Check(!header.contains("differs between profiles"), "the profile comment of the old rule is gone");
}

template <shim_cook::CookProfile Profile>
void CheckShimProfile(tests::TestRunner& runner, const manifest::BundleView& bundle, std::string_view target_name)
{
    constexpr uint32_t profileIndex = static_cast<uint32_t>(Profile);
    runner.CheckEqual(bundle.FindProfile(target_name),
                      static_cast<int32_t>(profileIndex),
                      "the enumerator value is the index of the profile in the manifest");

    using Material = shim_cook::Material<Profile, LayoutRule::Std140>;
    const std::optional<Located> located = Locate(runner, bundle, profileIndex, "ShimMaterial", "BlockCS");
    if (!located.has_value())
    {
        return;
    }

    CheckUniformBlock<Material>(runner, *located, shim_cook::ShimMaterial::BlockCS::material, { { "Count", offsetof(Material, Count) } });
}

void CheckShimCook(tests::TestRunner& runner)
{
    const std::vector<std::byte> bytes = ReadFileBytes(LODESTONE_HOST_MANIFEST_SHIM);
    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (!runner.CheckOk(bundle, "the manifest of the shim cook opens"))
    {
        return;
    }

    runner.BeginSection("a Bound shim is a binding of its own, so the block holds the plain data only");
    CheckShimProfile<shim_cook::CookProfile::Wgsl>(runner, *bundle, "wgsl");
    runner.CheckEqual(sizeof(shim_cook::Material<shim_cook::CookProfile::Wgsl, LayoutRule::Std140>), size_t{ 16u }, "Material on wgsl");

    runner.BeginSection("a Handle or Pointer shim is a stored value in the block, so the type of Material depends on the profile");
    using Stored = shim_cook::Material<shim_cook::CookProfile::SpirvModernMutable, LayoutRule::Std140>;
    CheckShimProfile<shim_cook::CookProfile::SpirvModernMutable>(runner, *bundle, "spirv-modern-mutable");
    runner.CheckEqual(sizeof(Stored), size_t{ 64u }, "Material with its stored values");
    runner.Check(!std::is_same_v<Stored, shim_cook::Material<shim_cook::CookProfile::Wgsl, LayoutRule::Std140>>,
                 "the two profiles give two types");

    // the stored value of a resource is at the offset that the generated type gives its member
    const std::optional<Located> located = Locate(runner, *bundle, 1u, "ShimMaterial", "BlockCS");
    if (located.has_value())
    {
        const manifest::LayoutRange layout = InstanceOf(*located).Layout();
        const std::optional<manifest::ResolvedResource> points = FindResource(layout, "Points");
        const std::optional<manifest::ResolvedResource> surface = FindResource(layout, "Surface");
        const std::optional<manifest::ResolvedResource> filter = FindResource(layout, "Filter");
        if (runner.Check(points.has_value() && surface.has_value() && filter.has_value(), "the shim resources are bindings"))
        {
            runner.CheckEqual(static_cast<size_t>(points->ValueOffset()), offsetof(Stored, Points), "the address of the buffer");
            runner.CheckEqual(static_cast<size_t>(surface->ValueOffset()), offsetof(Stored, Surface), "the handle of the texture");
            runner.CheckEqual(static_cast<size_t>(filter->ValueOffset()), offsetof(Stored, Filter), "the handle of the sampler");
        }
    }

    runner.BeginSection("a shim buffer that one profile stores as a value is a binding of every variant on the others");
    // the unqualified name compiles only when the constant is in the entry point namespace, and not in SomeVariants
    constexpr std::string_view pointsBuffer = shim_cook::ShimMaterial::BlockCS::Points;
    runner.CheckEqual(pointsBuffer, std::string_view{ "Points" }, "the constant of the shim buffer is the field of the author");
    const std::string shimHeader = ReadFileText(LODESTONE_HOST_HEADER_SHIM);
    runner.Check(!shimHeader.contains("SomeVariants"), "no binding of the shim cook is in SomeVariants");
    runner.Check(shimHeader.contains("// not a binding on profiles: spirv-modern-mutable\ninline constexpr std::string_view Points{"),
                 "the comment names the profile where the shim is a stored value");
    runner.Check(!shimHeader.contains("entryPointParams"), "no constant names the container of the constants");

    using EntryParams = shim_cook::ShimMaterial::EntryCS::Params<shim_cook::CookProfile::SpirvModernMutable>;
    if (const std::optional<Located> entryLocated = Locate(runner, *bundle, 1u, "ShimMaterial", "EntryCS"))
    {
        runner.CheckEqual(InstanceOf(*entryLocated).Constants().ByteSize(),
                          static_cast<uint32_t>(sizeof(EntryParams)),
                          "the push constants of a shim parameter are sizeof of Params");
    }
}

void CheckUnchangedHeaderKeepsItsTimestamp(tests::TestRunner& runner)
{
    runner.BeginSection("the file sink keeps the timestamp of a file whose bytes are unchanged");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / std::format("LodestoneHostHeaderSink_{}", stamp);
    FileOutputSink sink{ directory };

    runner.CheckEqual(sink.WriteArtifact("a.hpp", std::string_view{ "one" }), CookError::Success, "the first write");
    const std::filesystem::path file = directory / "a.hpp";
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours{ 1 };
    std::filesystem::last_write_time(file, old);

    runner.CheckEqual(sink.WriteArtifact("a.hpp", std::string_view{ "one" }), CookError::Success, "a write of the same bytes");
    runner.Check(std::filesystem::last_write_time(file) == old, "the timestamp does not change");

    runner.CheckEqual(sink.WriteArtifact("a.hpp", std::string_view{ "two" }), CookError::Success, "a write of other bytes");
    runner.Check(std::filesystem::last_write_time(file) != old, "the timestamp changes");
    runner.CheckEqual(ReadFileText(file), std::string{ "two" }, "the new bytes are in the file");

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

} // namespace

int main()
{
    tests::TestRunner runner{ "HostHeader" };

    CheckMainCook(runner);
    CheckSelectionByProfileAndRule(runner);
    CheckShimCook(runner);
    CheckUnchangedHeaderKeepsItsTimestamp(runner);

    return runner.Report();
}
