#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/ShimLinkSource.hpp"
#include "target/ShimTable.hpp"
#include "target/TargetProfile.hpp"

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

CookError ErrorOf(const std::vector<RawShimExtern>& externs, AccessModel access)
{
    RecordingDiagnosticSink sink;
    const CookResult<std::string> source = MakeShimLinkSource(externs, access, "Probe", "bound", sink);
    return source.has_value() ? CookError::Success : source.error();
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
    const CookResult<std::string> source = MakeShimLinkSource(externs, AccessModel::Bound, "Probe", "bound", sink);
    runner.Check(source.has_value(), "a table with every Bound cell links");
    runner.Check(source.has_value() && *source ==
                     "import LodestoneShims;\n"
                     "import Lighting;\n"
                     "export struct Points : IReadBuffer<PointLight> = BoundReadBuffer<PointLight>;\n"
                     "export struct Tints : IReadBuffer<float4> = BoundReadBuffer<float4>;\n"
                     "export struct Albedo : ITexture2DRef = BoundTexture2D;\n"
                     "export struct Linear : ISamplerRef = BoundSampler;\n"
                     "export struct Scratch : IReadWriteBuffer<Light> = BoundReadWriteBuffer<Light>;\n",
                 "one import for each distinct module, none for a builtin type, and the exports in source order");

    const CookResult<std::string> none = MakeShimLinkSource({}, AccessModel::Bound, "Probe", "bound", sink);
    runner.Check(none.has_value() && none->empty(), "a module with no shim externs has no link source");

    runner.BeginSection("an empty cell and malformed type arguments fail, each with its own error");
    runner.Check(ErrorOf(externs, AccessModel::Pointer) == CookError::ShimFormUnavailable,
                 "the Pointer cell of the table is empty");
    runner.Check(ErrorOf(externs, AccessModel::Invalid) == CookError::ShimFormUnavailable,
                 "an invalid access model has no cell");
    runner.Check(ErrorOf({ MakeExtern("Points", "IReadBuffer", "", {}) }, AccessModel::Bound) == CookError::ShimExternInvalid,
                 "a buffer with no type argument");
    runner.Check(ErrorOf({ MakeExtern("Albedo", "ITexture2DRef", "float4", { "" }) }, AccessModel::Bound) ==
                     CookError::ShimExternInvalid,
                 "a texture with a type argument");
    runner.Check(ErrorOf({ MakeExtern("Points", "IReadBuffer", "Foo<Bar>", { "" }) }, AccessModel::Bound) ==
                     CookError::ShimExternInvalid,
                 "a nested generic is not a plain name");
    runner.Check(ErrorOf({ MakeExtern("Points", "IReadBuffer", "A, B", { "", "" }) }, AccessModel::Bound) ==
                     CookError::ShimExternInvalid,
                 "two type arguments for one type parameter");
    runner.Check(ErrorOf({ MakeExtern("Points", "IReadBuffer", "Light", {}) }, AccessModel::Bound) == CookError::ShimExternInvalid,
                 "a type argument with no module entry");
    runner.Check(ErrorOf({ MakeExtern("Points", "IUnrelated", "Light", { "" }) }, AccessModel::Bound) == CookError::ShimExternInvalid,
                 "an interface that is not in the table");
    runner.Check(ErrorOf({ MakeExtern("Dst", "IWriteStorageTexture2D", "", {}) }, AccessModel::Bound) == CookError::ShimExternInvalid,
                 "a storage texture needs a format, and nothing names one until step C11.3");

    runner.Check(AccessModelFromPlacementKind(PlacementKindFromAccessModel(AccessModel::Bound)) == AccessModel::Bound &&
                     AccessModelFromPlacementKind(PlacementKindFromAccessModel(AccessModel::Pointer)) == AccessModel::Pointer &&
                     AccessModelFromPlacementKind(PlacementKind::None) == AccessModel::Invalid,
                 "the access model and the placement kind convert both ways");

    return runner.Report();
}
