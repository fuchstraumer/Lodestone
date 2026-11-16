#include "TestHarness.hpp"

#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "ShaderLibraryTypes.hpp"
#include "compile/RawLibrary.hpp"
#include "compile/SlangCompiler.hpp"
#include "driver/CookerDriver.hpp"
#include "driver/CookerOptions.hpp"
#include "driver/CookerSteps.hpp"
#include "driver/steps/BuildModuleStep.hpp"
#include "driver/steps/FinalizeModuleStep.hpp"
#include "driver/steps/PrepareCookStep.hpp"
#include "driver/steps/PrepareModuleStep.hpp"
#include "driver/steps/PreparePermutationSpaceStep.hpp"
#include "ShaderManifest.hpp"
#include "ShaderManifestLayout.hpp"
#include "emit/OutputSink.hpp"
#include "emit/ShaderManifestEmitter.hpp"
#include "model/CookedLibrary.hpp"
#include "model/HostTypeTable.hpp"
#include "permute/PermutationSpace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Every expected number below is computed by hand from the layout rules, and not read back from Slang. std140
// aligns a vector of three or four components, a struct, and an array element to 16 bytes, and rounds a
// struct size up to 16. std430 keeps the alignment of the element: an array of floats has stride 4, and a
// struct size rounds up to its own alignment only.

namespace
{

using namespace lodestone;

// a count of 0 means that the dimension is absent
struct ExpectedDimension
{
    uint32_t Count;
    uint32_t Stride;
};

struct ExpectedField
{
    std::string_view Name;
    uint32_t Offset;
    uint32_t Size;
    HostFieldKind Kind;
    HostScalarType ScalarType;
    uint32_t Rows;
    uint32_t Columns;
    // outermost first
    std::array<ExpectedDimension, 2u> Dimensions;
    // a nested struct is named, and its own entry is checked on its own
    std::string_view Nested;
};

struct ExpectedStruct
{
    std::string_view Name;
    LayoutRule Rule;
    uint32_t ByteSize;
    std::span<const ExpectedField> Fields;
};

constexpr ExpectedField Scalar(std::string_view name, uint32_t offset, HostScalarType type)
{
    return ExpectedField{ name, offset, 4u, HostFieldKind::Scalar, type, 1u, 1u, {}, {} };
}

constexpr ExpectedField Vector(
    std::string_view name, uint32_t offset, uint32_t size, HostScalarType type, uint32_t columns)
{
    return ExpectedField{ name, offset, size, HostFieldKind::Vector, type, 1u, columns, {}, {} };
}

constexpr ExpectedField Array(std::string_view name,
                              uint32_t offset,
                              uint32_t size,
                              HostFieldKind kind,
                              HostScalarType type,
                              uint32_t columns,
                              uint32_t count,
                              uint32_t stride)
{
    return ExpectedField{ name, offset, size, kind, type, 1u, columns, { { { count, stride } } }, {} };
}

constexpr ExpectedField Array2D(std::string_view name,
                                uint32_t offset,
                                uint32_t size,
                                HostFieldKind kind,
                                HostScalarType type,
                                uint32_t columns,
                                ExpectedDimension outer,
                                ExpectedDimension inner)
{
    return ExpectedField{ name, offset, size, kind, type, 1u, columns, { { outer, inner } }, {} };
}

// Light: a float3 aligns to 16 and takes 12 bytes, so the float fits in the next 4. The struct size is 16 in
// both rules.
constexpr std::array<ExpectedField, 2u> k_LightFields{
    Vector("position", 0u, 12u, HostScalarType::Float32, 3u),
    Scalar("range", 12u, HostScalarType::Float32),
};

// std140: float samples[3] has stride 16 (an array element aligns to 16), so samples take 48 bytes and bias
// is at 48. The struct is 52 bytes, rounded up to 64.
constexpr std::array<ExpectedField, 2u> k_FalloffStd140Fields{
    Array("samples", 0u, 48u, HostFieldKind::Scalar, HostScalarType::Float32, 1u, 3u, 16u),
    Scalar("bias", 48u, HostScalarType::Float32),
};

// std430: the stride is 4, so samples take 12 bytes and bias is at 12. The struct is 16 bytes.
constexpr std::array<ExpectedField, 2u> k_FalloffStd430Fields{
    Array("samples", 0u, 12u, HostFieldKind::Scalar, HostScalarType::Float32, 1u, 3u, 4u),
    Scalar("bias", 12u, HostScalarType::Float32),
};

// std140 Scene. world: four row vectors of 16 bytes at offset 0. skew (float2x3, two rows of three): each row
// is a float3 with stride 16, so 32 bytes at 64. key (Light): a struct aligns to 16, so it is at 64 + 32 =
// 96, and takes 16. tints (float3[2]): stride 16, so 32 bytes at 112. flags is at 144. The struct is 148
// bytes, rounded up to 160.
constexpr std::array<ExpectedField, 5u> k_SceneFields{
    ExpectedField{ "world", 0u, 64u, HostFieldKind::Matrix, HostScalarType::Float32, 4u, 4u, {}, {} },
    ExpectedField{ "skew", 64u, 32u, HostFieldKind::Matrix, HostScalarType::Float32, 2u, 3u, {}, {} },
    ExpectedField{ "key", 96u, 16u, HostFieldKind::Struct, HostScalarType::Invalid, 1u, 1u, {}, "Light" },
    Array("tints", 112u, 32u, HostFieldKind::Vector, HostScalarType::Float32, 3u, 2u, 16u),
    Scalar("flags", 144u, HostScalarType::UInt32),
};

// std430 entry point params, the push constant block of SPIR-V: weights[3] has stride 4 and takes 12 bytes,
// and count is at 12. The struct is 16 bytes.
constexpr std::array<ExpectedField, 2u> k_ParamsStd430Fields{
    Array("weights", 0u, 12u, HostFieldKind::Scalar, HostScalarType::Float32, 1u, 3u, 4u),
    Scalar("count", 12u, HostScalarType::UInt32),
};

// std140 entry point params, the uniform buffer of WGSL: weights[3] has stride 16 and takes 48 bytes, and
// count is at
// 48. The struct is 52 bytes, rounded up to 64.
constexpr std::array<ExpectedField, 2u> k_ParamsStd140Fields{
    Array("weights", 0u, 48u, HostFieldKind::Scalar, HostScalarType::Float32, 1u, 3u, 16u),
    Scalar("count", 48u, HostScalarType::UInt32),
};

// One struct parameter: the root is the wrapper of the loose parameters, and the parameter is one nested
// field of 16.
constexpr std::array<ExpectedField, 1u> k_NamedFields{
    ExpectedField{
        "light", 0u, 16u, HostFieldKind::Struct, HostScalarType::Invalid, 1u, 1u, {}, "Light" },
};

// 16-bit members. scale is 2 bytes at 0. uv is a vector of two 16-bit floats, which aligns to 4, so it is at
// 4 and takes 4 bytes. tag is at 8. The struct is 12 bytes, rounded up to 16 under std140.
constexpr std::array<ExpectedField, 3u> k_PackedFields{
    ExpectedField{ "scale", 0u, 2u, HostFieldKind::Scalar, HostScalarType::Float16, 1u, 1u, {}, {} },
    Vector("uv", 4u, 4u, HostScalarType::Float16, 2u),
    Scalar("tag", 8u, HostScalarType::UInt32),
};

// Grid, float2 cells[2][3], std140: a float2 has size 8, and an array element rounds to 16, so the inner array has
// stride 16 and takes 48. The outer array has stride 48 and takes 96. tag is at 96. The struct is 100 bytes,
// rounded up to 112.
constexpr std::array<ExpectedField, 2u> k_GridStd140Fields{
    Array2D("cells", 0u, 96u, HostFieldKind::Vector, HostScalarType::Float32, 2u, { 2u, 48u }, { 3u, 16u }),
    Scalar("tag", 96u, HostScalarType::UInt32),
};

// std430: a float2 has stride 8, so the inner array takes 24 and the outer array has stride 24 and takes 48. tag is
// at 48. The struct is 52 bytes, rounded up to its alignment of 8: 56.
constexpr std::array<ExpectedField, 2u> k_GridStd430Fields{
    Array2D("cells", 0u, 48u, HostFieldKind::Vector, HostScalarType::Float32, 2u, { 2u, 24u }, { 3u, 8u }),
    Scalar("tag", 48u, HostScalarType::UInt32),
};

constexpr std::array<ExpectedStruct, 7u> k_CommonStructs{
    ExpectedStruct{ "Light", LayoutRule::Std140, 16u, k_LightFields },
    ExpectedStruct{ "Light", LayoutRule::Std430, 16u, k_LightFields },
    ExpectedStruct{ "Falloff", LayoutRule::Std140, 64u, k_FalloffStd140Fields },
    ExpectedStruct{ "Falloff", LayoutRule::Std430, 16u, k_FalloffStd430Fields },
    ExpectedStruct{ "Scene", LayoutRule::Std140, 160u, k_SceneFields },
    ExpectedStruct{ "Grid", LayoutRule::Std140, 112u, k_GridStd140Fields },
    ExpectedStruct{ "Grid", LayoutRule::Std430, 56u, k_GridStd430Fields },
};

/** The state that a cooked module points into. Keep it alive as long as the module. */
struct CookedFixture
{
    std::unique_ptr<PermutationSpace> Space;
    CookedModule Module;
    std::vector<CompiledVariant> CompiledVariants;
};

/** Runs the per-module steps for one module and one target, the way `RunCook` does, and keeps the
 * intermediate variants. The module is the interned module of stage 6, frozen. */
CookResult<CookedFixture> CookModuleForTarget(const std::filesystem::path& module_path,
                                              std::string_view target,
                                              bool dedupe,
                                              DiagnosticSink& diagnostics)
{
    CookerOptions options;
    options.ModulePaths.push_back(module_path);
    options.TargetNames.emplace_back(target);
    options.OutputPath = "HostTypeTable";
    options.DedupeEnabled = dedupe;

    const PrepareCookStep prepareCook;
    CookResult<PreparedCook> prepared = prepareCook(std::move(options), diagnostics);
    if (!prepared)
    {
        return std::unexpected(prepared.error());
    }

    const SharedCookState& state = prepared->SharedState;
    const PrepareModuleStep prepareModule;
    CookResult<PreparedCompiler> compiler = prepareModule(state, module_path, target);
    if (!compiler)
    {
        return std::unexpected(compiler.error());
    }

    CookResult<std::vector<RawAxisDeclaration>> axes = compiler->Compiler->BuildAxisDeclarations();
    if (!axes)
    {
        return std::unexpected(axes.error());
    }

    const std::string_view moduleName = state.AllModuleNames.front();
    const PreparePermutationSpaceStep preparePermutationSpace;
    CookResult<PreparedPermutationSpace> space = preparePermutationSpace(
        state, moduleName, target, compiler->Compiler->GetSymbolTable(), std::move(axes.value()));
    if (!space)
    {
        return std::unexpected(space.error());
    }

    CookedFixture fixture;
    fixture.Space = std::make_unique<PermutationSpace>(std::move(space->Space));

    const BuildModuleStep buildModule;
    CookResult<BuiltModule> built =
        buildModule(state, moduleName, target, compiler->Compiler.get(), *fixture.Space, space->Variants);
    if (!built)
    {
        return std::unexpected(built.error());
    }

    const FinalizeModuleStep finalizeModule;
    CookResult<FinalizedModule> finalized =
        finalizeModule(state, std::move(built->Module), built->CompiledVariants);
    if (!finalized)
    {
        return std::unexpected(finalized.error());
    }

    fixture.Module = std::move(finalized->Module);
    fixture.CompiledVariants = std::move(built->CompiledVariants);

    return fixture;
}

bool FieldMatches(const HostTypeTable& table, const HostField& field, const ExpectedField& expected)
{
    const bool nestedMatches = expected.Nested.empty()
                                   ? field.StructIndex == k_NoHostStruct
                                   : field.StructIndex < table.Structs.size() &&
                                         table.Structs[field.StructIndex].Name == expected.Nested;

    std::vector<HostArrayDimension> expectedDimensions;
    for (const ExpectedDimension& dimension : expected.Dimensions)
    {
        if (dimension.Count != 0u)
        {
            expectedDimensions.push_back(HostArrayDimension{ .Count = dimension.Count, .Stride = dimension.Stride });
        }
    }

    return field.Name == expected.Name && field.Offset == expected.Offset && field.Size == expected.Size &&
           field.Kind == expected.Kind && field.ScalarType == expected.ScalarType &&
           field.Rows == expected.Rows && field.Columns == expected.Columns &&
           field.ArrayDimensions == expectedDimensions && nestedMatches;
}

void CheckStruct(tests::TestRunner& runner, const HostTypeTable& table, const ExpectedStruct& expected)
{
    const auto context =
        runner.Context(std::format("struct {} ({})", expected.Name, ToString(expected.Rule)));
    const std::optional<uint32_t> index = FindHostStruct(table, expected.Name, expected.Rule);
    if (!runner.Check(index.has_value(), "the table has the struct"))
    {
        return;
    }

    const HostStruct& found = table.Structs[*index];
    runner.CheckEqual(found.ByteSize, expected.ByteSize, "the struct size");
    if (!runner.CheckEqual(found.Fields.size(), expected.Fields.size(), "the field count"))
    {
        return;
    }

    for (size_t fieldIndex = 0u; fieldIndex < expected.Fields.size(); ++fieldIndex)
    {
        const auto fieldContext = runner.Context(std::format("field {}", expected.Fields[fieldIndex].Name));
        runner.Check(FieldMatches(table, found.Fields[fieldIndex], expected.Fields[fieldIndex]),
                     "the field offset, size, type, and array match the hand-computed layout");
    }
}

const ReflectedBinding& BindingOfRoot(const CookedModule& module,
                                      const LibraryVariant& variant,
                                      const HostRoot& root)
{
    return module.Resources[module.ResourceLists[variant.ResourceListIndex][root.Owner]];
}

/** Returns the struct that the named binding stores, or nothing when the binding has no root. */
std::optional<std::pair<std::string_view, LayoutRule>> RootOfBinding(const CookedModule& module,
                                                                     std::string_view binding_name)
{
    const LibraryVariant& variant = module.Variants.front();
    for (const HostRoot& root : variant.HostRoots)
    {
        if (root.Kind == HostRootKind::Binding && BindingOfRoot(module, variant, root).Name == binding_name)
        {
            const HostStruct& hostStruct = module.HostTypes.Structs[root.Struct];

            return std::pair{ std::string_view{ hostStruct.Name }, hostStruct.Rule };
        }
    }

    return std::nullopt;
}

std::optional<std::pair<std::string_view, LayoutRule>> RootOfEntryPoint(const CookedModule& module,
                                                                        std::string_view entry_point_name)
{
    const LibraryVariant& variant = module.Variants.front();
    for (const HostRoot& root : variant.HostRoots)
    {
        if (root.Kind == HostRootKind::EntryPointConstants &&
            module.EntryPoints[root.Owner].Name == entry_point_name)
        {
            const HostStruct& hostStruct = module.HostTypes.Structs[root.Struct];

            return std::pair{ std::string_view{ hostStruct.Name }, hostStruct.Rule };
        }
    }

    return std::nullopt;
}

using Root = std::optional<std::pair<std::string_view, LayoutRule>>;

void CheckHostTypes(tests::TestRunner& runner,
                    const std::filesystem::path& asset_directory,
                    std::string_view target,
                    LayoutRule params_rule)
{
    const auto context = runner.Context(std::format("target {}", target));
    StderrDiagnosticSink diagnostics;
    CookResult<CookedFixture> fixture =
        CookModuleForTarget(asset_directory / "HostTypes.slang", target, true, diagnostics);
    if (!runner.CheckOk(fixture, "the module cooks"))
    {
        return;
    }

    const HostTypeTable& table = fixture->Module.HostTypes;
    for (const ExpectedStruct& expected : k_CommonStructs)
    {
        CheckStruct(runner, table, expected);
    }

    const std::span<const ExpectedField> paramsFields =
        params_rule == LayoutRule::Std430 ? std::span<const ExpectedField>{ k_ParamsStd430Fields }
                                          : std::span<const ExpectedField>{ k_ParamsStd140Fields };
    CheckStruct(
        runner,
        table,
        ExpectedStruct{
            "ParamsCSParams", params_rule, params_rule == LayoutRule::Std430 ? 16u : 64u, paramsFields });
    CheckStruct(runner, table, ExpectedStruct{ "NamedCSParams", params_rule, 16u, k_NamedFields });

    // 7 common structs, and the two params roots
    runner.CheckEqual(table.Structs.size(), 9u, "no other struct is in the table");

    runner.CheckEqual(RootOfBinding(fixture->Module, "scene"),
                      Root{ { "Scene", LayoutRule::Std140 } },
                      "a constant buffer stores its element struct in std140");
    runner.CheckEqual(RootOfBinding(fixture->Module, "falloffBlock"),
                      Root{ { "Falloff", LayoutRule::Std140 } },
                      "the constant buffer of Falloff is std140");
    runner.CheckEqual(RootOfBinding(fixture->Module, "falloffBuffer"),
                      Root{ { "Falloff", LayoutRule::Std430 } },
                      "the structured buffer of Falloff is std430");
    runner.CheckEqual(RootOfBinding(fixture->Module, "lights"),
                      Root{ { "Light", LayoutRule::Std430 } },
                      "the structured buffer of Light is std430");
    runner.CheckEqual(RootOfBinding(fixture->Module, "plain"),
                      Root{},
                      "a structured buffer of float4 has no struct, so no root");
    runner.CheckEqual(RootOfBinding(fixture->Module, "output"), Root{}, "a buffer of float4 has no root");
    runner.CheckEqual(RootOfEntryPoint(fixture->Module, "ParamsCS"),
                      Root{ { "ParamsCSParams", params_rule } },
                      "the loose parameters have a root named after the entry point");
    runner.CheckEqual(RootOfEntryPoint(fixture->Module, "NamedCS"),
                      Root{ { "NamedCSParams", params_rule } },
                      "a struct parameter is a field of the wrapper root");
    runner.CheckEqual(RootOfEntryPoint(fixture->Module, "SceneCS"),
                      Root{},
                      "an entry point with no parameters has no root");

    const HostTypeTable& variantTable = fixture->CompiledVariants.front().HostTypes;
    runner.Check(variantTable == table, "a module with one variant has the table of that variant");
}

void CheckNoDedupeGivesTheSameTable(tests::TestRunner& runner, const std::filesystem::path& asset_directory)
{
    StderrDiagnosticSink diagnostics;
    CookResult<CookedFixture> deduped =
        CookModuleForTarget(asset_directory / "HostTypes.slang", "spirv", true, diagnostics);
    CookResult<CookedFixture> separate =
        CookModuleForTarget(asset_directory / "HostTypes.slang", "spirv", false, diagnostics);
    if (!runner.CheckOk(deduped, "the deduplicated cook succeeds") ||
        !runner.CheckOk(separate, "the --no-dedupe cook succeeds"))
    {
        return;
    }

    runner.Check(deduped->Module.HostTypes == separate->Module.HostTypes,
                 "the host type table does not depend on deduplication");
    runner.Check(deduped->Module.Variants.front().HostRoots == separate->Module.Variants.front().HostRoots,
                 "the roots do not depend on deduplication");
}

void CheckHalf(tests::TestRunner& runner, const std::filesystem::path& asset_directory)
{
    for (const std::string_view target : { std::string_view{ "wgsl" }, std::string_view{ "spirv" } })
    {
        const auto context = runner.Context(std::format("target {}", target));
        StderrDiagnosticSink diagnostics;
        CookResult<CookedFixture> fixture =
            CookModuleForTarget(asset_directory / "HostTypesHalf.slang", target, true, diagnostics);
        if (runner.CheckOk(fixture, "the module with 16-bit members cooks"))
        {
            CheckStruct(runner,
                        fixture->Module.HostTypes,
                        ExpectedStruct{ "Packed", LayoutRule::Std140, 16u, k_PackedFields });
        }
    }
}

void CheckAxisSizedArrayConflicts(tests::TestRunner& runner, const std::filesystem::path& asset_directory)
{
    for (const std::string_view target : { std::string_view{ "wgsl" }, std::string_view{ "spirv" } })
    {
        const auto context = runner.Context(std::format("target {}", target));
        CookerOptions options;
        options.ModulePaths.push_back(asset_directory / "HostTypesSized.slang");
        options.TargetNames.emplace_back(target);
        options.OutputPath = "HostTypeTable";
        MemoryOutputSink sink;
        RecordingDiagnosticSink diagnostics;
        const CookResult<CookStatistics> cooked = RunCook(options, sink, diagnostics);
        runner.CheckError(cooked,
                          CookError::HostTypeLayoutConflict,
                          "an array that an axis sizes gives two layouts, so the cook fails");

        bool namesTheStruct = false;
        bool namesBothVariants = false;
        for (const Diagnostic& record : diagnostics.Records())
        {
            namesTheStruct = namesTheStruct || record.Message.contains("'Sized'");
            namesBothVariants = namesBothVariants ||
                                (record.Message.contains("COUNT=2") && record.Message.contains("COUNT=4") &&
                                 record.Message.contains("32 bytes and 64 bytes"));
        }
        runner.Check(namesTheStruct, "the message names the struct");
        runner.Check(namesBothVariants, "the message names both variants and both sizes");
    }
}

HostStruct MakeStruct(std::string name, LayoutRule rule, uint32_t byte_size, uint32_t offset)
{
    HostField field;
    field.Name = "value";
    field.Offset = offset;
    field.Size = 4u;
    field.Kind = HostFieldKind::Scalar;
    field.ScalarType = HostScalarType::Float32;

    return HostStruct{ .Name = std::move(name), .Rule = rule, .ByteSize = byte_size, .Fields = { field } };
}

HostStruct MakeStoredValueStruct(std::string name, LayoutRule rule, uint32_t byte_size)
{
    HostField field;
    field.Name = "handle";
    field.Size = 8u;
    field.Kind = HostFieldKind::Vector;
    field.ScalarType = HostScalarType::UInt32;
    field.Columns = 2u;
    field.StoredValue = true;

    return HostStruct{ .Name = std::move(name), .Rule = rule, .ByteSize = byte_size, .Fields = { field } };
}

HostStruct MakeParent(std::string name, LayoutRule rule, uint32_t nested_index, uint32_t byte_size)
{
    HostField field;
    field.Name = "child";
    field.Size = 4u;
    field.Kind = HostFieldKind::Struct;
    field.StructIndex = nested_index;

    return HostStruct{ .Name = std::move(name), .Rule = rule, .ByteSize = byte_size, .Fields = { field } };
}

HostTypeIndex IndexOf(const HostTypeTable& table)
{
    HostTypeIndex index;
    for (size_t structIndex = 0u; structIndex < table.Structs.size(); ++structIndex)
    {
        const HostStruct& hostStruct = table.Structs[structIndex];
        index.emplace(std::pair{ hostStruct.Name, hostStruct.Rule }, static_cast<uint32_t>(structIndex));
    }

    return index;
}

void CheckMerge(tests::TestRunner& runner)
{
    runner.BeginSection("merging tables keys a struct by name and rule, and remaps nested indices");
    HostTypeTable destination;
    destination.Structs.push_back(MakeStruct("A", LayoutRule::Std140, 16u, 0u));
    HostTypeIndex destinationIndex = IndexOf(destination);

    HostTypeTable source;
    source.Structs.push_back(MakeStruct("B", LayoutRule::Std140, 16u, 0u));
    source.Structs.push_back(MakeParent("P", LayoutRule::Std140, 0u, 16u));
    source.Structs.push_back(MakeStruct("A", LayoutRule::Std140, 16u, 0u));

    std::vector<uint32_t> remap;
    const std::optional<HostTypeConflict> conflict = MergeHostTypes(destination, destinationIndex, source, remap);
    runner.Check(!conflict.has_value(), "equal layouts do not conflict");
    runner.CheckEqual(remap,
                      std::vector<uint32_t>{ 1u, 2u, 0u },
                      "A merges into the existing struct, and B and P are appended");
    if (destination.Structs.size() == 3u)
    {
        runner.CheckEqual(destination.Structs[2].Fields.front().StructIndex,
                          1u,
                          "the parent points to the merged index of B");
    }

    runner.BeginSection("the same name in another rule is another struct");
    HostTypeTable otherRule;
    otherRule.Structs.push_back(MakeStruct("A", LayoutRule::Std430, 4u, 0u));
    const std::optional<HostTypeConflict> ruleConflict = MergeHostTypes(destination, destinationIndex, otherRule, remap);
    runner.Check(!ruleConflict.has_value(), "std140 and std430 layouts of one name never conflict");
    runner.CheckEqual(destination.Structs.size(), 4u, "the std430 layout is appended");

    runner.BeginSection("one name and rule with another layout conflicts");
    HostTypeTable changed;
    changed.Structs.push_back(MakeStruct("A", LayoutRule::Std140, 16u, 8u));
    const std::optional<HostTypeConflict> offsetConflict = MergeHostTypes(destination, destinationIndex, changed, remap);
    if (runner.Check(offsetConflict.has_value(), "a different offset conflicts"))
    {
        runner.CheckEqual(offsetConflict->Existing, 0u, "the conflict names the existing struct");
        runner.Check(offsetConflict->Difference.contains("offset 0") &&
                         offsetConflict->Difference.contains("offset 8"),
                     "the difference names both offsets");
    }

    HostTypeTable bigger;
    bigger.Structs.push_back(MakeStruct("A", LayoutRule::Std140, 32u, 0u));
    const std::optional<HostTypeConflict> sizeConflict = MergeHostTypes(destination, destinationIndex, bigger, remap);
    if (runner.Check(sizeConflict.has_value(), "a different size conflicts"))
    {
        runner.Check(sizeConflict->Difference.contains("16 bytes and 32 bytes"),
                     "the difference names both sizes");
    }
}

CookedLibrary MakeLibrary(const HostStruct& first, const HostStruct& second, const PermutationSpace& space)
{
    CookedLibrary library;
    library.ModuleNames = { "ModuleA", "ModuleB" };
    library.Profiles.resize(1u);
    library.Profiles.front().TargetName = "spirv";
    library.Environments.resize(2u);

    CookedModule moduleA;
    moduleA.Name = "ModuleA";
    moduleA.Space = &space;
    moduleA.HostTypes.Structs.push_back(first);
    CookedModule moduleB;
    moduleB.Name = "ModuleB";
    moduleB.Space = &space;
    moduleB.HostTypes.Structs.push_back(second);
    library.Environments[0] = std::move(moduleA);
    library.Environments[1] = std::move(moduleB);

    return library;
}

void CheckAcrossModules(tests::TestRunner& runner)
{
    runner.BeginSection("one struct name and rule has one layout across the modules of a cook");
    const PermutationSpace space{ {} };
    RecordingDiagnosticSink diagnostics;

    const CookedLibrary equal = MakeLibrary(MakeStruct("Shared", LayoutRule::Std140, 16u, 0u),
                                            MakeStruct("Shared", LayoutRule::Std140, 16u, 0u),
                                            space);
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(equal, diagnostics),
                      CookError::Success,
                      "equal layouts of one name pass");

    const CookedLibrary otherRule = MakeLibrary(MakeStruct("Shared", LayoutRule::Std140, 16u, 0u),
                                                MakeStruct("Shared", LayoutRule::Std430, 4u, 0u),
                                                space);
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(otherRule, diagnostics),
                      CookError::Success,
                      "one name in two rules is two keys, so no conflict");

    const CookedLibrary different = MakeLibrary(MakeStruct("Shared", LayoutRule::Std140, 16u, 0u),
                                                MakeStruct("Shared", LayoutRule::Std140, 16u, 4u),
                                                space);
    diagnostics.Reset();
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(different, diagnostics),
                      CookError::HostTypeLayoutConflict,
                      "two modules with one struct name and different layouts conflict");
    bool namesModules = false;
    for (const Diagnostic& record : diagnostics.Records())
    {
        namesModules =
            namesModules || (record.Message.contains("ModuleA") && record.Message.contains("ModuleB") &&
                             record.Message.contains("spirv") && record.Message.contains("'Shared'"));
    }
    runner.Check(namesModules, "the message names the struct, both modules, and the profile");
}

CookedLibrary MakeProfileLibrary(HostTypeTable bound, HostTypeTable modern, const PermutationSpace& space)
{
    CookedLibrary library;
    library.ModuleNames = { "ModuleA" };
    library.Profiles.resize(2u);
    library.Profiles[0].TargetName = "wgsl";
    library.Profiles[1].TargetName = "spirv-modern-mutable";
    library.Environments.resize(2u);

    CookedModule boundModule;
    boundModule.Name = "ModuleA";
    boundModule.Space = &space;
    boundModule.HostTypes = std::move(bound);
    CookedModule modernModule;
    modernModule.Name = "ModuleA";
    modernModule.Space = &space;
    modernModule.HostTypes = std::move(modern);
    library.Environments[0] = std::move(boundModule);
    library.Environments[1] = std::move(modernModule);

    return library;
}

HostTypeTable TableOf(std::vector<HostStruct> structs)
{
    return HostTypeTable{ .Structs = std::move(structs) };
}

void CheckAcrossProfiles(tests::TestRunner& runner)
{
    const PermutationSpace space{ {} };
    RecordingDiagnosticSink diagnostics;

    runner.BeginSection("a struct with a stored value in any profile may differ across profiles");
    const CookedLibrary storedValue =
        MakeProfileLibrary(TableOf({ MakeStruct("Material", LayoutRule::Std140, 16u, 0u) }),
                           TableOf({ MakeStoredValueStruct("Material", LayoutRule::Std140, 64u) }),
                           space);
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(storedValue, diagnostics),
                      CookError::Success,
                      "the profile with no stored value is exempt too, because another profile has one");

    runner.BeginSection("a struct that nests a struct with a stored value may differ across profiles");
    const CookedLibrary nested =
        MakeProfileLibrary(TableOf({ MakeStruct("Inner", LayoutRule::Std140, 4u, 0u),
                                     MakeParent("Outer", LayoutRule::Std140, 0u, 16u) }),
                           TableOf({ MakeStoredValueStruct("Inner", LayoutRule::Std140, 8u),
                                     MakeParent("Outer", LayoutRule::Std140, 0u, 32u) }),
                           space);
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(nested, diagnostics),
                      CookError::Success,
                      "the outer struct is exempt through its nested struct");

    runner.BeginSection("a plain struct has one layout across profiles");
    const CookedLibrary plain =
        MakeProfileLibrary(TableOf({ MakeStruct("Plain", LayoutRule::Std140, 16u, 0u) }),
                           TableOf({ MakeStruct("Plain", LayoutRule::Std140, 32u, 0u) }),
                           space);
    diagnostics.Reset();
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(plain, diagnostics),
                      CookError::HostTypeLayoutConflict,
                      "a plain struct that differs across profiles conflicts");
    bool namesProfiles = false;
    for (const Diagnostic& record : diagnostics.Records())
    {
        namesProfiles = namesProfiles || (record.Message.contains("'Plain'") && record.Message.contains("'wgsl'") &&
                                          record.Message.contains("'spirv-modern-mutable'"));
    }
    runner.Check(namesProfiles, "the message names the struct and both profiles");

    runner.BeginSection("a struct with a stored value has one layout within one profile");
    const CookedLibrary sameProfile = MakeLibrary(MakeStoredValueStruct("Material", LayoutRule::Std140, 64u),
                                                  MakeStoredValueStruct("Material", LayoutRule::Std140, 96u),
                                                  space);
    diagnostics.Reset();
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(sameProfile, diagnostics),
                      CookError::HostTypeLayoutConflict,
                      "two modules of one profile cannot give one struct two layouts, even with a stored value");

    runner.BeginSection("the same name in another layout rule is not compared across profiles");
    const CookedLibrary otherRule =
        MakeProfileLibrary(TableOf({ MakeStruct("Plain", LayoutRule::Std140, 16u, 0u) }),
                           TableOf({ MakeStruct("Plain", LayoutRule::Std430, 32u, 0u) }),
                           space);
    runner.CheckEqual(VerifyHostTypesAcrossProfiles(otherRule, diagnostics),
                      CookError::Success,
                      "a key holds the layout rule");
}

void CheckFlatArrayCountOfLinkTimeConstant(tests::TestRunner& runner, const std::filesystem::path& asset_directory)
{
    runner.BeginSection("an array that a link-time constant sizes has its declared count in the manifest member");
    CookerOptions options;
    options.ModulePaths.push_back(asset_directory / "HostTypesDefault.slang");
    options.TargetNames.emplace_back("spirv");
    options.OutputPath = "HostTypeTable";
    MemoryOutputSink sink;
    RecordingDiagnosticSink diagnostics;
    const CookResult<CookStatistics> cooked = RunCook(options, sink, diagnostics);
    if (!runner.CheckOk(cooked, "the module cooks"))
    {
        return;
    }

    std::vector<std::byte> bytes;
    for (const auto& [name, text] : sink.GetArtifacts())
    {
        if (name == k_ManifestFileName)
        {
            bytes.resize(text.size());
            std::memcpy(bytes.data(), text.data(), bytes.size());
        }
    }

    const manifest::ManifestResult<manifest::BundleView> bundle = manifest::BundleView::Open(bytes);
    if (!runner.CheckOk(bundle, "the bundle opens"))
    {
        return;
    }

    const manifest::ManifestResult<manifest::EnvironmentView> environment = bundle->OpenEnvironment(0u, 0u);
    if (!runner.CheckOk(environment, "the environment opens"))
    {
        return;
    }

    std::optional<uint32_t> arrayCount;
    std::optional<uint32_t> elementStride;
    const manifest::EntryPointInstanceView instance = environment->VariantAt(0u).EntryPoint(0u);
    for (const manifest::ResolvedResource resource : instance.Layout())
    {
        for (const manifest::UniformMemberView member : resource.Members())
        {
            if (resource.Name() == "sized" && member.Name() == "data")
            {
                arrayCount = member.ArrayCount();
                elementStride = member.ElementStride();
            }
        }
    }

    if (runner.Check(arrayCount.has_value(), "the block has the member"))
    {
        runner.CheckEqual(*arrayCount, 3u, "the member array count is the declared count");
        runner.CheckEqual(*elementStride, 16u, "the std140 stride of a float element");
    }
}

} // namespace

int main(int argc, char** argv)
{
    // run-tests.bat gives no arguments, so CMake sets LODESTONE_TEST_ASSET_DIR as the fallback
    const std::filesystem::path assetDirectory{ argc > 1 ? argv[1] : LODESTONE_TEST_ASSET_DIR };
    tests::TestRunner runner{ "HostTypeTable" };

    runner.BeginSection("wgsl: entry point params are a uniform buffer, so std140");
    CheckHostTypes(runner, assetDirectory, "wgsl", LayoutRule::Std140);

    runner.BeginSection("spirv: entry point params are a push constant block, so std430");
    CheckHostTypes(runner, assetDirectory, "spirv", LayoutRule::Std430);

    runner.BeginSection("--no-dedupe gives the same table as the deduplicated cook");
    CheckNoDedupeGivesTheSameTable(runner, assetDirectory);

    runner.BeginSection("16-bit members read back on both targets");
    CheckHalf(runner, assetDirectory);

    runner.BeginSection("an array that an axis sizes fails the cook with HostTypeLayoutConflict");
    CheckAxisSizedArrayConflicts(runner, assetDirectory);

    CheckMerge(runner);
    CheckAcrossModules(runner);
    CheckAcrossProfiles(runner);
    CheckFlatArrayCountOfLinkTimeConstant(runner, assetDirectory);

    return runner.Report();
}
