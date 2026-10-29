#include "TestHarness.hpp"
#include "model/BufferLayout.hpp"
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

namespace
{

using namespace lodestone;
using lodestone::tests::TestRunner;

LayoutNode Scalar(uint32_t size)
{
    LayoutNode node;
    node.Kind = LayoutNodeKind::Scalar;
    node.ScalarSize = size;

    return node;
}

LayoutNode Vector(uint32_t components)
{
    LayoutNode node;
    node.Kind = LayoutNodeKind::Vector;
    node.ScalarSize = 4u;
    node.Components = components;

    return node;
}

LayoutNode Matrix(uint32_t rows, uint32_t columns, bool row_major)
{
    LayoutNode node;
    node.Kind = LayoutNodeKind::Matrix;
    node.ScalarSize = 4u;
    node.Rows = rows;
    node.Columns = columns;
    node.RowMajor = row_major;

    return node;
}

LayoutNode Array(LayoutNode element, uint32_t count)
{
    LayoutNode node;
    node.Kind = LayoutNodeKind::Array;
    node.ElementCount = count;
    node.Children.push_back(std::move(element));

    return node;
}

LayoutNode Named(std::string name, LayoutNode node)
{
    node.Name = std::move(name);

    return node;
}

LayoutNode Struct(std::string type_name, std::initializer_list<LayoutNode> fields)
{
    LayoutNode node;
    node.Kind = LayoutNodeKind::Struct;
    node.TypeName = std::move(type_name);
    node.Children.assign(fields.begin(), fields.end());

    return node;
}

LayoutNode PaddedVertex()
{
    return Struct("PaddedVertex",
                  { Named("position", Vector(3u)),
                    Named("pad0", Scalar(4u)),
                    Named("normal", Vector(3u)),
                    Named("pad1", Scalar(4u)),
                    Named("tangent", Vector(4u)),
                    Named("uv0", Vector(2u)),
                    Named("uv1", Vector(2u)) });
}

void TestSizes(TestRunner& runner)
{
    const lodestone::tests::ScopedSection section = runner.Section("size and alignment under each rule");

    const BufferLayoutSize scalarVec3 = ComputeBufferLayout(Vector(3u), BufferLayoutRule::Scalar);
    runner.CheckEqual(scalarVec3.Size, 12u, "a scalar-layout vec3 is 12 bytes");
    runner.CheckEqual(scalarVec3.Alignment, 4u, "a scalar-layout vec3 aligns to its component");

    const BufferLayoutSize std430Vec3 = ComputeBufferLayout(Vector(3u), BufferLayoutRule::Std430);
    runner.CheckEqual(std430Vec3.Size, 12u, "a std430 vec3 is 12 bytes");
    runner.CheckEqual(std430Vec3.Alignment, 16u, "a std430 vec3 aligns like a vec4");
    runner.CheckEqual(ComputeBufferLayout(Vector(2u), BufferLayoutRule::Std430).Alignment, 8u, "a std430 vec2 aligns to 8");

    runner.CheckEqual(ComputeElementStride(Vector(3u), BufferLayoutRule::Std430), 16u, "a std430 vec3 array has stride 16");
    runner.CheckEqual(ComputeElementStride(Vector(3u), BufferLayoutRule::Scalar), 12u, "a scalar vec3 array has stride 12");

    runner.CheckEqual(ComputeElementStride(PaddedVertex(), BufferLayoutRule::Std430), 64u, "the padded vertex is 64 bytes in std430");
    runner.CheckEqual(ComputeElementStride(PaddedVertex(), BufferLayoutRule::Scalar), 64u, "the padded vertex is 64 bytes in scalar");

    const LayoutNode uvVertex = Struct("UvVertex", { Named("position", Vector(3u)), Named("uv", Vector(2u)) });
    runner.CheckEqual(ComputeElementStride(uvVertex, BufferLayoutRule::Std430), 32u, "float3 then float2 is 32 bytes in std430");
    runner.CheckEqual(ComputeElementStride(uvVertex, BufferLayoutRule::Scalar), 20u, "float3 then float2 is 20 bytes in scalar");

    runner.CheckEqual(ComputeBufferLayout(Matrix(4u, 4u, false), BufferLayoutRule::Std430).Size, 64u, "a float4x4 is 64 bytes");
    runner.CheckEqual(ComputeBufferLayout(Matrix(4u, 3u, false), BufferLayoutRule::Std430).Size, 48u,
                      "a column-major float4x3 is three columns of four");
    runner.CheckEqual(ComputeBufferLayout(Matrix(4u, 3u, true), BufferLayoutRule::Std430).Size, 64u,
                      "a row-major float4x3 is four rows of three, each padded to 16");
}

void TestAgreement(TestRunner& runner)
{
    const lodestone::tests::ScopedSection section = runner.Section("types whose two layouts agree");

    runner.Check(!FindLayoutDifference(PaddedVertex()).has_value(), "a float3 followed by one 4-byte value agrees");
    runner.Check(!FindLayoutDifference(Vector(4u)).has_value(), "a float4 element agrees");
    runner.Check(!FindLayoutDifference(Scalar(4u)).has_value(), "a float element agrees");
    runner.Check(!FindLayoutDifference(Matrix(4u, 4u, false)).has_value(), "a float4x4 element agrees");
    runner.Check(!FindLayoutDifference(Struct("Pair", { Named("a", Vector(2u)), Named("b", Scalar(4u)), Named("c", Scalar(4u)) })).has_value(),
                 "a float2 followed by two floats agrees");
    runner.Check(!FindLayoutDifference(Struct("Wrap", { Named("inner", PaddedVertex()), Named("w", Vector(4u)) })).has_value(),
                 "a nested struct that agrees keeps its parent in agreement");
    runner.Check(!FindLayoutDifference(Struct("Rows", { Named("rows", Array(Vector(4u), 3u)) })).has_value(),
                 "an array of float4 agrees");
}

void TestDifferences(TestRunner& runner)
{
    const lodestone::tests::ScopedSection section = runner.Section("types whose two layouts differ");

    const std::optional<LayoutDifference> uv =
        FindLayoutDifference(Struct("UvVertex", { Named("position", Vector(3u)), Named("uv", Vector(2u)) }));
    if (runner.Check(uv.has_value(), "float3 then float2 differs"))
    {
        runner.CheckEqual(uv->Kind, LayoutDifferenceKind::MemberOffset, "the difference is a member offset");
        runner.CheckEqual(uv->StructName, "UvVertex", "it names the struct");
        runner.CheckEqual(uv->MemberName, "uv", "it names the member");
        runner.CheckEqual(uv->ScalarValue, 12u, "the scalar offset is 12");
        runner.CheckEqual(uv->Std430Value, 16u, "the std430 offset is 16");
    }

    const std::optional<LayoutDifference> pair =
        FindLayoutDifference(Struct("Pair", { Named("a", Vector(3u)), Named("b", Vector(3u)) }));
    if (runner.Check(pair.has_value(), "float3 then float3 differs"))
    {
        runner.CheckEqual(pair->MemberName, "b", "the second float3 is the member");
        runner.CheckEqual(pair->ScalarValue, 12u, "the scalar offset is 12");
        runner.CheckEqual(pair->Std430Value, 16u, "the std430 offset is 16");
    }

    const std::optional<LayoutDifference> tail =
        FindLayoutDifference(Struct("Tail", { Named("a", Vector(4u)), Named("b", Scalar(4u)) }));
    if (runner.Check(tail.has_value(), "a float4 then a float differs in size"))
    {
        runner.CheckEqual(tail->Kind, LayoutDifferenceKind::StructSize, "the difference is the struct size");
        runner.CheckEqual(tail->ScalarValue, 20u, "the scalar size is 20");
        runner.CheckEqual(tail->Std430Value, 32u, "the std430 size is 32");
    }

    const std::optional<LayoutDifference> array =
        FindLayoutDifference(Struct("Points", { Named("points", Array(Vector(3u), 2u)) }));
    if (runner.Check(array.has_value(), "an array of float3 differs"))
    {
        runner.CheckEqual(array->Kind, LayoutDifferenceKind::ArrayStride, "the difference is the array stride");
        runner.CheckEqual(array->MemberName, "points", "it names the array member");
        runner.CheckEqual(array->ScalarValue, 12u, "the scalar stride is 12");
        runner.CheckEqual(array->Std430Value, 16u, "the std430 stride is 16");
    }

    const std::optional<LayoutDifference> matrix =
        FindLayoutDifference(Struct("Basis", { Named("basis", Matrix(3u, 3u, false)) }));
    if (runner.Check(matrix.has_value(), "a float3x3 differs"))
    {
        runner.CheckEqual(matrix->Kind, LayoutDifferenceKind::ArrayStride, "the difference is the column stride");
        runner.CheckEqual(matrix->MemberName, "basis", "it names the matrix member");
    }

    const std::optional<LayoutDifference> nested = FindLayoutDifference(
        Struct("Outer", { Named("inner", Struct("Inner", { Named("a", Vector(3u)), Named("b", Vector(2u)) })) }));
    if (runner.Check(nested.has_value(), "a nested struct that differs fails its parent"))
    {
        runner.CheckEqual(nested->StructName, "Inner", "it names the inner struct");
        runner.CheckEqual(nested->MemberName, "b", "it names the inner member");
    }

    const std::optional<LayoutDifference> plain = FindLayoutDifference(Vector(3u));
    if (runner.Check(plain.has_value(), "a float3 element differs in stride"))
    {
        runner.CheckEqual(plain->Kind, LayoutDifferenceKind::ArrayStride, "the difference is the element stride");
        runner.Check(plain->StructName.empty() && plain->MemberName.empty(), "it names no struct and no member");
    }
}

void TestDescription(TestRunner& runner)
{
    const lodestone::tests::ScopedSection section = runner.Section("the text of a difference");

    const std::optional<LayoutDifference> uv =
        FindLayoutDifference(Struct("UvVertex", { Named("position", Vector(3u)), Named("uv", Vector(2u)) }));
    if (!runner.Check(uv.has_value(), "float3 then float2 differs"))
    {
        return;
    }

    const std::string text = DescribeLayoutDifference(*uv);
    runner.Check(text.contains("UvVertex"), "the text names the struct");
    runner.Check(text.contains("'uv'"), "the text names the member");
    runner.Check(text.contains("offset 12") && text.contains("offset 16"), "the text gives both offsets");
    runner.Check(text.contains("padding"), "the text tells the author to add padding");
}

} // namespace

int main()
{
    TestRunner runner{ "BufferLayoutTests" };

    TestSizes(runner);
    TestAgreement(runner);
    TestDifferences(runner);
    TestDescription(runner);

    return runner.Report();
}
