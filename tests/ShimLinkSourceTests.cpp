#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/ShimLinkSource.hpp"
#include "permute/PermutationAxis.hpp"
#include "permute/PermutationSpace.hpp"
#include "permute/PermutationValue.hpp"
#include "target/ShimTable.hpp"
#include "target/StorageFormatTable.hpp"
#include "target/TargetProfile.hpp"

#include <format>
#include <optional>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace lodestone;

RawShimExtern MakeExtern(std::string name,
                         std::string interface_name,
                         std::string type_arguments,
                         std::vector<std::string> argument_modules)
{
    RawShimExtern result;
    result.Name = std::move(name);
    result.Interface = std::move(interface_name);
    result.TypeArguments = std::move(type_arguments);
    result.ArgumentModules = std::move(argument_modules);
    result.SourceFile = "Probe.slang";
    result.SourceLine = 7u;
    return result;
}

RawShimExtern MakeFormatExtern(std::string name, std::string format_axis)
{
    RawShimExtern result = MakeExtern(std::move(name), "IWriteStorageTexture2D", "", {});
    result.FormatAxis = std::move(format_axis);
    return result;
}

constexpr ShimForms k_BoundForms{ .Buffer = AccessModel::Bound,
                                  .Texture = AccessModel::Bound,
                                  .Sampler = AccessModel::Bound,
                                  .StorageTexture = AccessModel::Bound };

CookError ErrorOf(const std::vector<RawShimExtern>& externs, const ShimForms& forms)
{
    RecordingDiagnosticSink sink;
    const CookResult<std::string> source = MakeShimLinkSource(externs, forms, "Probe", "bound", sink);
    return source.has_value() ? CookError::Success : source.error();
}

/** An enum axis over the cases of `enum_name`, declared in `enum_module`, that takes the cases at `ordinals`. */
PermutationAxis MakeEnumAxis(std::string name,
                             std::string enum_name,
                             std::string enum_module,
                             std::string active_when,
                             std::vector<uint32_t> ordinals)
{
    std::vector<RawEnumCase> cases;
    for (const StorageFormatInfo& format : GetStorageFormats())
    {
        cases.push_back(RawEnumCase{ .Name = std::string{ format.EnumCase }, .Value = std::ssize(cases) });
    }

    std::vector<PermutationValue> values;
    for (const uint32_t ordinal : ordinals)
    {
        values.push_back(PermutationValue::MakeEnum(ordinal));
    }

    return PermutationAxis{ std::move(name),
                            std::move(values),
                            AxisKind::Tuning,
                            EarliestBindingTime::Cook,
                            AxisValueDomain::Enum,
                            std::move(active_when),
                            std::move(enum_name),
                            std::move(enum_module),
                            std::move(cases) };
}

CookError FormatErrorOf(const std::vector<RawShimExtern>& externs, const PermutationSpace& space)
{
    RecordingDiagnosticSink sink;
    const CookResult<std::vector<ShimFormatLinkSource>> sources =
        MakeShimFormatLinkSources(externs, space, k_BoundForms, "Probe", sink);
    return sources.has_value() ? CookError::Success : sources.error();
}

} // namespace

int main()
{
    tests::TestRunner runner{ "ShimLinkSource" };
    RecordingDiagnosticSink sink;

    runner.BeginSection("the link module imports what its type arguments need, and exports each extern once");
    const std::vector<RawShimExtern> externs{
        MakeExtern("Points", "IReadBuffer", "PointLight", { "Lighting" }),
        MakeExtern("Tints", "IReadBuffer", "float4", { "" }),
        MakeExtern("Albedo", "ITexture2DRef", "", {}),
        MakeExtern("Linear", "ISamplerRef", "", {}),
        MakeExtern("Scratch", "IReadWriteBuffer", " Light ", { "Lighting" }),
    };
    const CookResult<std::string> source = MakeShimLinkSource(externs, k_BoundForms, "Probe", "bound", sink);
    runner.CheckOk(source, "a table with every Bound cell links");
    if (runner.CheckOk(source, "one import for each distinct module, none for a builtin type, and the exports in source order"))
    {
        runner.CheckEqual(*source,
                          "import LodestoneShims;\n"
                          "import Lighting;\n"
                          "export struct Points : IReadBuffer<PointLight> = BoundReadBuffer<PointLight>;\n"
                          "export struct Tints : IReadBuffer<float4> = BoundReadBuffer<float4>;\n"
                          "export struct Albedo : ITexture2DRef = BoundTexture2D;\n"
                          "export struct Linear : ISamplerRef = BoundSampler;\n"
                          "export struct Scratch : IReadWriteBuffer<Light> = BoundReadWriteBuffer<Light>;\n",
                          "one import for each distinct module, none for a builtin type, and the exports in source order");
    }

    const CookResult<std::string> none = MakeShimLinkSource({}, k_BoundForms, "Probe", "bound", sink);
    runner.Check(none.has_value() && none->empty(), "a module with no shim externs has no link source");

    runner.BeginSection("an empty cell and malformed type arguments fail, each with its own error");
    runner.CheckEqual(ErrorOf({ MakeExtern("Albedo", "ITexture2DRef", "", {}) },
                              ShimForms{ .Buffer = AccessModel::Bound,
                                         .Texture = AccessModel::Pointer,
                                         .Sampler = AccessModel::Bound,
                                         .StorageTexture = AccessModel::Bound }),
                      CookError::ShimFormUnavailable,
                      "a texture has no Pointer form");
    runner.CheckEqual(ErrorOf(externs, ShimForms{}), CookError::ShimFormUnavailable, "an invalid access model has no cell");
    runner.CheckEqual(ErrorOf({ MakeExtern("Points", "IReadBuffer", "", {}) }, k_BoundForms), CookError::ShimExternInvalid,
                      "a buffer with no type argument");
    runner.CheckEqual(ErrorOf({ MakeExtern("Albedo", "ITexture2DRef", "float4", { "" }) }, k_BoundForms),
                      CookError::ShimExternInvalid,
                      "a texture with a type argument");
    runner.CheckEqual(ErrorOf({ MakeExtern("Points", "IReadBuffer", "Foo<Bar>", { "" }) }, k_BoundForms),
                      CookError::ShimExternInvalid,
                      "a nested generic is not a plain name");
    runner.CheckEqual(ErrorOf({ MakeExtern("Points", "IReadBuffer", "A, B", { "", "" }) }, k_BoundForms),
                      CookError::ShimExternInvalid,
                      "two type arguments for one type parameter");
    runner.CheckEqual(ErrorOf({ MakeExtern("Points", "IReadBuffer", "Light", {}) }, k_BoundForms), CookError::ShimExternInvalid,
                      "a type argument with no module entry");
    runner.CheckEqual(ErrorOf({ MakeExtern("Points", "IUnrelated", "Light", { "" }) }, k_BoundForms), CookError::ShimExternInvalid,
                      "an interface that is not in the table");
    runner.CheckEqual(ErrorOf({ MakeExtern("Dst", "IWriteStorageTexture2D", "", {}) }, k_BoundForms),
                      CookError::ShimStorageFormatMissing,
                      "a storage texture with no [ls_storage_format]");
    RawShimExtern formattedBuffer = MakeExtern("Points", "IReadBuffer", "float4", { "" });
    formattedBuffer.FormatAxis = "LEVEL_FORMAT";
    runner.CheckEqual(ErrorOf({ formattedBuffer }, k_BoundForms), CookError::ShimExternInvalid,
                      "[ls_storage_format] on a buffer, which takes no format");

    runner.BeginSection("a storage extern links once for each value of its format axis, and not in the fixed module");
    std::vector<RawShimExtern> withFormats = externs;
    withFormats.insert(withFormats.begin() + 1, MakeFormatExtern("Dst", "LEVEL_FORMAT"));
    withFormats.push_back(MakeFormatExtern("Half", "LEVEL_FORMAT"));
    const CookResult<std::string> fixedSource = MakeShimLinkSource(withFormats, k_BoundForms, "Probe", "bound", sink);
    runner.Check(fixedSource.has_value() && source.has_value() && *fixedSource == *source,
                 "the fixed module does not change when format externs join it");
    const std::vector<RawShimExtern> formatOnly{ MakeFormatExtern("Dst", "LEVEL_FORMAT") };
    const CookResult<std::string> onlyFormats = MakeShimLinkSource(formatOnly, k_BoundForms, "Probe", "bound", sink);
    runner.Check(onlyFormats.has_value() && onlyFormats->empty(), "a module with format externs only has no fixed module");

    // Rgba16Float and R32Float, as a policy subset or an ls_axis_values list leaves them
    std::vector<PermutationAxis> formatAxes;
    formatAxes.push_back(MakeEnumAxis("LEVEL_FORMAT", "StorageFormat", "LodestoneShims", "", { 2u, 3u }));
    const PermutationSpace formatSpace{ std::move(formatAxes) };
    const CookResult<std::vector<ShimFormatLinkSource>> formatSources =
        MakeShimFormatLinkSources(withFormats, formatSpace, k_BoundForms, "Probe", sink);
    if (runner.CheckOk(formatSources, "one link module for each value of the axis"))
    {
        runner.CheckEqual(formatSources->size(), 2u, "one link module for each value of the axis");
    }
    if (formatSources.has_value() && formatSources->size() == 2u)
    {
        const ShimFormatLinkSource& half = (*formatSources)[0];
        const ShimFormatLinkSource& single = (*formatSources)[1];
        const char* halfClaim = "Rgba16Float links both externs of the axis to RGBA16F, in source order";
        runner.CheckEqual(half.AxisName, "LEVEL_FORMAT", halfClaim);
        runner.CheckEqual(half.CaseOrdinal, 2u, halfClaim);
        runner.CheckEqual(half.Source,
                          "import LodestoneShims;\n"
                          "export struct Dst : IWriteStorageTexture2D = BoundWriteStorageTexture2D<RGBA16F>;\n"
                          "export struct Half : IWriteStorageTexture2D = BoundWriteStorageTexture2D<RGBA16F>;\n",
                          halfClaim);
        const char* singleClaim = "R32Float links both to R32F";
        runner.CheckEqual(single.CaseOrdinal, 3u, singleClaim);
        runner.CheckEqual(single.Source,
                          "import LodestoneShims;\n"
                          "export struct Dst : IWriteStorageTexture2D = BoundWriteStorageTexture2D<R32F>;\n"
                          "export struct Half : IWriteStorageTexture2D = BoundWriteStorageTexture2D<R32F>;\n",
                          singleClaim);
    }

    runner.BeginSection("spirv-modern links each class in its own form: Pointer buffers, Handle textures and samplers");
    const CookResult<TargetProfile> modern = FindTargetProfile("spirv-modern");
    if (runner.CheckOk(modern, "the build has a spirv-modern profile"))
    {
        const CookResult<std::string> modernSource = MakeShimLinkSource(externs, modern->Forms, "Probe", "spirv-modern", sink);
        if (runner.CheckOk(modernSource, "every cell of spirv-modern has an implementation"))
        {
            runner.CheckEqual(*modernSource,
                              "import LodestoneShims;\n"
                              "import Lighting;\n"
                              "export struct Points : IReadBuffer<PointLight> = PointerReadBuffer<PointLight>;\n"
                              "export struct Tints : IReadBuffer<float4> = PointerReadBuffer<float4>;\n"
                              "export struct Albedo : ITexture2DRef = HandleTexture2D;\n"
                              "export struct Linear : ISamplerRef = HandleSampler;\n"
                              "export struct Scratch : IReadWriteBuffer<Light> = PointerReadWriteBuffer<Light>;\n",
                              "buffers link the Pointer form, and the texture and the sampler the Handle form");
        }
        const CookResult<std::vector<ShimFormatLinkSource>> modernFormats =
            MakeShimFormatLinkSources(withFormats, formatSpace, modern->Forms, "Probe", sink);
        if (runner.CheckOk(modernFormats, "the format modules of spirv-modern link") && !modernFormats->empty())
        {
            runner.CheckEqual(modernFormats->front().Source,
                              "import LodestoneShims;\n"
                              "export struct Dst : IWriteStorageTexture2D = HandleWriteStorageTexture2D<RGBA16F>;\n"
                              "export struct Half : IWriteStorageTexture2D = HandleWriteStorageTexture2D<RGBA16F>;\n",
                              "a storage texture links the Handle form with the format of the variant");
        }
        runner.CheckEqual(SingleForm(modern->Forms), AccessModel::Invalid, "spirv-modern has no single form for every class");
        runner.Check(modern->DescriptorHeapSet == std::optional<uint32_t>{ 3u }, "spirv-modern puts the descriptor heap in set 3");
    }

    runner.BeginSection("the format axis must exist, and be an ungated enum axis over StorageFormat");
    runner.CheckEqual(FormatErrorOf({ MakeFormatExtern("Dst", "MISSING") }, formatSpace), CookError::ShimStorageFormatAxisUnknown,
                      "an axis the module does not declare");
    std::vector<PermutationAxis> otherAxes;
    otherAxes.push_back(MakeEnumAxis("OTHER_ENUM", "Quality", "LodestoneShims", "", { 0u, 1u }));
    otherAxes.push_back(MakeEnumAxis("FOREIGN_FORMAT", "StorageFormat", "Elsewhere", "", { 0u, 1u }));
    otherAxes.push_back(PermutationAxis{ "FOG",
                                         { PermutationValue{ false }, PermutationValue{ true } },
                                         AxisKind::Technique,
                                         EarliestBindingTime::Cook,
                                         AxisValueDomain::Boolean });
    otherAxes.push_back(MakeEnumAxis("GATED_FORMAT", "StorageFormat", "LodestoneShims", "FOG == 1", { 0u, 1u }));
    const PermutationSpace otherSpace{ std::move(otherAxes) };
    for (const std::string_view axisName : { "OTHER_ENUM", "FOREIGN_FORMAT", "FOG", "GATED_FORMAT" })
    {
        runner.CheckEqual(FormatErrorOf({ MakeFormatExtern("Dst", std::string{ axisName }) }, otherSpace),
                          CookError::ShimStorageFormatAxisInvalid,
                          std::format("{} is not an ungated StorageFormat axis", axisName));
    }

    const char* convertClaim = "the access model and the placement kind convert both ways";
    runner.CheckEqual(AccessModelFromPlacementKind(PlacementKindFromAccessModel(AccessModel::Bound)), AccessModel::Bound, convertClaim);
    runner.CheckEqual(AccessModelFromPlacementKind(PlacementKindFromAccessModel(AccessModel::Pointer)), AccessModel::Pointer, convertClaim);
    runner.CheckEqual(AccessModelFromPlacementKind(PlacementKindFromAccessModel(AccessModel::Handle)), AccessModel::Handle, convertClaim);
    runner.CheckEqual(AccessModelFromPlacementKind(PlacementKind::None), AccessModel::Invalid, convertClaim);

    return runner.Report();
}
