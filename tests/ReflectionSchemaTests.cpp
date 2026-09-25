#include "model/ShaderDataSchema.hpp"
#include "CookerErrors.hpp"
#include "ShaderLibraryTypes.hpp"
#include "TestHarness.hpp"

#include <cstdint>
#include <string_view>

// The reflection schema is the contract between the compiler and the CPU. This file proves the
// parts of it that are pure data: the `ResourceShape` flag enum, the `ToString` tables an emitter
// and a dump reader print, and the equality that dedup rests on. None of it needs Slang, so a
// change to a field or an enumerator fails here, on the commit that made it, not in a later cook.
//
// The `ResourceShape` flag layout is the load-bearing decision: the base shape sits in the low
// nibble and array, multisample, shadow, and feedback are flags above it. A reader must take the
// base with `GetBaseShape`, never an `==` against a base value, because `Texture2D | ArrayFlag` is
// not `Texture2D`. These tests are what keep that discipline honest.

using lodestone::BindingKind;
using lodestone::CookError;
using lodestone::GetBaseShape;
using lodestone::MatrixLayout;
using lodestone::ReflectedBinding;
using lodestone::ReflectedUniformMember;
using lodestone::ResourceAccess;
using lodestone::ResourceShape;
using lodestone::ResourceShapeIsArray;
using lodestone::ResourceShapeIsMultisample;
using lodestone::ResourceShapeIsShadow;
using lodestone::TextureSampleType;
using lodestone::ToString;

int main()
{
    lodestone::tests::TestRunner runner{ "ReflectionSchemaTests" };

    runner.BeginSection("GetBaseShape masks the flags away");
    runner.Check(GetBaseShape(ResourceShape::Texture2D | ResourceShape::ArrayFlag | ResourceShape::ShadowFlag) ==
                     ResourceShape::Texture2D,
                 "an array shadow texture has base Texture2D");
    runner.Check(GetBaseShape(ResourceShape::StructuredBuffer) == ResourceShape::StructuredBuffer,
                 "a shape with no flags is its own base");
    runner.Check(GetBaseShape(ResourceShape::Invalid) == ResourceShape::Invalid,
                 "the base of Invalid is Invalid");

    runner.BeginSection("the flag predicates read the high bits");
    const ResourceShape arrayShadow =
        ResourceShape::Texture2D | ResourceShape::ArrayFlag | ResourceShape::ShadowFlag;
    runner.Check(ResourceShapeIsArray(arrayShadow), "the array flag reads as set");
    runner.Check(ResourceShapeIsShadow(arrayShadow), "the shadow flag reads as set");
    runner.Check(!ResourceShapeIsMultisample(arrayShadow), "the multisample flag reads as clear");
    runner.Check(!ResourceShapeIsArray(ResourceShape::Texture2D), "a plain texture is not an array");
    runner.Check(!ResourceShapeIsShadow(ResourceShape::Texture2D), "a plain texture is not a shadow");
    runner.Check(ResourceShapeIsMultisample(ResourceShape::Texture2D | ResourceShape::MultisampleFlag),
                 "the multisample flag reads as set");

    runner.BeginSection("ToString(ResourceShape) composes base then flags in order");
    // The suffix order is fixed: Multisample, then Array, then Shadow, then Feedback. A reader that
    // reorders them changes the emitted name, so these strings are the contract.
    runner.Check(ToString(ResourceShape::Texture2D) == "Texture2D", "a bare base is just its name");
    runner.Check(ToString(ResourceShape::StructuredBuffer) == "StructuredBuffer", "a buffer base has no flags");
    runner.Check(ToString(ResourceShape::Texture2D | ResourceShape::ArrayFlag) == "Texture2DArray",
                 "the array flag appends Array");
    runner.Check(ToString(ResourceShape::Texture2D | ResourceShape::ShadowFlag) == "Texture2DShadow",
                 "the shadow flag appends Shadow");
    runner.Check(ToString(ResourceShape::Texture2D | ResourceShape::MultisampleFlag) == "Texture2DMultisample",
                 "the multisample flag appends Multisample");
    runner.Check(ToString(ResourceShape::Texture2D | ResourceShape::ArrayFlag | ResourceShape::ShadowFlag) ==
                     "Texture2DArrayShadow",
                 "array before shadow, whatever order the bits were set");
    runner.Check(ToString(ResourceShape::Texture2D | ResourceShape::MultisampleFlag | ResourceShape::ArrayFlag |
                          ResourceShape::ShadowFlag | ResourceShape::FeedbackFlag) ==
                     "Texture2DMultisampleArrayShadowFeedback",
                 "every flag appends in the fixed order");

    runner.BeginSection("ToString(ResourceShape) names an unknown base Invalid");
    runner.Check(ToString(ResourceShape::Invalid) == "Invalid", "the zero shape is Invalid");
    // 0x05 was the TextureBuffer slot, now dropped. A value in the base nibble with no enumerator
    // must read as Invalid, not as a stale name or an out-of-range table read.
    runner.Check(ToString(static_cast<ResourceShape>(0x05u)) == "Invalid",
                 "a base with no enumerator is Invalid");

    runner.BeginSection("ToString(BindingKind) names every kind");
    // A new enumerator with no case falls through to "Invalid". Walking the contiguous range catches
    // that: every real kind must give its own name, and only Invalid may be "Invalid".
    runner.Check(ToString(BindingKind::Invalid) == "Invalid", "Invalid names itself");
    runner.Check(ToString(BindingKind::UniformBuffer) == "UniformBuffer", "a spot check on the spelling");
    runner.Check(ToString(BindingKind::PushConstant) == "PushConstant", "the last kind has its own name");
    for (uint8_t value = static_cast<uint8_t>(BindingKind::Sampler);
         value <= static_cast<uint8_t>(BindingKind::PushConstant);
         ++value)
    {
        runner.Check(ToString(static_cast<BindingKind>(value)) != "Invalid",
                     "a valid binding kind has a name of its own");
    }

    runner.BeginSection("ToString(TextureSampleType) names every type");
    runner.Check(ToString(TextureSampleType::Invalid) == "Invalid", "Invalid names itself");
    runner.Check(ToString(TextureSampleType::Depth) == "Depth", "a spot check on the spelling");
    for (uint8_t value = static_cast<uint8_t>(TextureSampleType::Float);
         value <= static_cast<uint8_t>(TextureSampleType::UnsignedInteger);
         ++value)
    {
        runner.Check(ToString(static_cast<TextureSampleType>(value)) != "Invalid",
                     "a valid sample type has a name of its own");
    }

    runner.BeginSection("ToString(CookError) names an error above 127");
    // magic_enum reads names only up to 127 by default. Each error in the bands below printed as an
    // empty string until the range was widened.
    runner.Check(ToString(CookError::ReflectionMismatch) == "ReflectionMismatch", "a low error names itself");
    runner.Check(ToString(CookError::PolicyAxisNotDeclared) == "PolicyAxisNotDeclared", "the policy band has names");
    runner.Check(ToString(CookError::FileNotFound) == "FileNotFound", "the system band has names");
    runner.Check(ToString(CookError::SlangCachedModuleWriteFailed) == "SlangCachedModuleWriteFailed",
                 "the Slang band has names");
    runner.Check(ToString(CookError::TargetValidationEntryPointParseFailed) ==
                     "TargetValidationEntryPointParseFailed",
                 "the highest error has a name");

    runner.BeginSection("a uniform member's layout and stride take part in equality");
    // A CPU packer that transposes a matrix or misindexes an array produces wrong pixels, not a
    // crash. The guard is that two members which differ only in layout or stride are not equal, so
    // dedup keeps them apart and the field survives to the packer.
    ReflectedUniformMember base;
    base.Name = "world";
    base.Data.Offset = 0u;
    base.Data.Size = 64u;
    base.Data.ArrayCount = 1u;
    base.Data.ElementStride = 16u;
    base.Data.MatrixLayout = MatrixLayout::RowMajor;

    ReflectedUniformMember same = base;
    runner.Check(same == base, "two members with identical fields are equal");

    ReflectedUniformMember otherLayout = base;
    otherLayout.Data.MatrixLayout = MatrixLayout::ColumnMajor;
    runner.Check(!(otherLayout == base), "a different matrix layout makes a different member");

    ReflectedUniformMember otherStride = base;
    otherStride.Data.ElementStride = 32u;
    runner.Check(!(otherStride == base), "a different element stride makes a different member");

    runner.BeginSection("DescribeBinding reads the shape, stride, size, sample, and array fields");
    // The dump reader is a second pair of eyes on a binding. It must render the new fields, so a
    // regression that drops one shows up as a shorter description here.
    ReflectedBinding texture;
    texture.Name = "albedo";
    texture.Placement = lodestone::BoundPlacement{ .Group = 1u, .Binding = 2u };
    texture.Kind = BindingKind::Texture;
    texture.Shape = ResourceShape::Texture2D | ResourceShape::ArrayFlag;
    texture.SampleType = TextureSampleType::Float;
    texture.ArrayCount = 4u;
    runner.Check(lodestone::DescribeBinding(texture) ==
                     "@group(1) @binding(2) albedo : Texture Texture2DArray sample=Float array=4",
                 "a texture description carries shape, sample type, and array count");

    ReflectedBinding buffer;
    buffer.Name = "lights";
    buffer.Placement = lodestone::BoundPlacement{ .Group = 0u, .Binding = 3u };
    buffer.Kind = BindingKind::StorageBuffer;
    buffer.Shape = ResourceShape::StructuredBuffer;
    buffer.ElementStride = 32u;
    buffer.ByteSize = 320u;
    runner.Check(lodestone::DescribeBinding(buffer) ==
                     "@group(0) @binding(3) lights : StorageBuffer StructuredBuffer stride=32 bytes=320",
                 "a structured buffer description carries the stride and the byte size");

    return runner.Report();
}
