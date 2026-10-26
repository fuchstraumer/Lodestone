#include "CookerErrors.hpp"
#include "Diagnostics.hpp"
#include "TestHarness.hpp"
#include "model/DeliveredForm.hpp"
#include "model/ShaderDataSchema.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace lodestone;
using lodestone::tests::TestRunner;

// a base form with three bindings, one pair of bindings 0 and 1, a capability, and workgroup memory
EntryPointCode MakeBase()
{
    EntryPointCode base;
    base.UsedBindingIndices = { 0u, 1u, 2u };
    base.SamplerTexturePairs = { SamplerTexturePair{ .Texture = 0u, .Sampler = 1u } };
    base.CodeFacts.CapabilityNames = { "Float16" };
    base.CodeFacts.WorkgroupMemoryBytes = 256u;

    return base;
}

CompiledVariant MakeVariant(EntryPointCode delivered, std::optional<EntryPointCode> base)
{
    CompiledEntryPoint entryPoint;
    entryPoint.Name = "ReduceCS";
    entryPoint.Delivered = std::move(delivered);
    entryPoint.Base = std::move(base);

    CompiledVariant variant;
    variant.VariantDescription = "test";
    variant.EntryPoints.push_back(std::move(entryPoint));

    return variant;
}

CookError Verify(const CompiledVariant& variant)
{
    RecordingDiagnosticSink sink;
    return VerifyDeliveredForm(variant, "spirv", sink);
}

void TestAccepted(TestRunner& runner)
{
    const lodestone::tests::ScopedSection section = runner.Section("a delivered form that needs no more passes");

    runner.CheckEqual(Verify(MakeVariant(MakeBase(), MakeBase())), CookError::Success, "an equal form passes");
    runner.CheckEqual(Verify(MakeVariant(MakeBase(), std::nullopt)), CookError::Success, "one compile has nothing to compare");

    // the optimizer removed binding 1, and with it the pair, and the workgroup memory
    EntryPointCode smaller = MakeBase();
    smaller.UsedBindingIndices = { 0u, 2u };
    smaller.SamplerTexturePairs.clear();
    smaller.CodeFacts.WorkgroupMemoryBytes = 0u;
    runner.CheckEqual(Verify(MakeVariant(smaller, MakeBase())), CookError::Success, "a form with less passes");

    // the pair keeps its bindings when its positions move
    EntryPointCode moved = MakeBase();
    moved.UsedBindingIndices = { 0u, 1u };
    runner.CheckEqual(Verify(MakeVariant(moved, MakeBase())), CookError::Success, "a pair compares by binding, not position");

    EntryPointCode debug = MakeBase();
    debug.CodeFacts.CapabilityNames = { "Float16", "SPV_KHR_non_semantic_info" };
    runner.CheckEqual(Verify(MakeVariant(debug, MakeBase())), CookError::Success,
                      "debug information may add SPV_KHR_non_semantic_info");
}

void TestRejected(TestRunner& runner)
{
    const lodestone::tests::ScopedSection section = runner.Section("a delivered form that needs more fails");

    EntryPointCode extraBinding = MakeBase();
    extraBinding.UsedBindingIndices = { 0u, 1u, 2u, 3u };
    runner.CheckEqual(Verify(MakeVariant(extraBinding, MakeBase())), CookError::DeliveredFormExceedsBase,
                      "a binding the base does not use fails");

    EntryPointCode extraPair = MakeBase();
    extraPair.SamplerTexturePairs.push_back(SamplerTexturePair{ .Texture = 2u, .Sampler = 1u });
    runner.CheckEqual(Verify(MakeVariant(extraPair, MakeBase())), CookError::DeliveredFormExceedsBase,
                      "a pair the base does not sample fails");

    EntryPointCode extraCapability = MakeBase();
    extraCapability.CodeFacts.CapabilityNames = { "Float16", "Int64" };
    runner.CheckEqual(Verify(MakeVariant(extraCapability, MakeBase())), CookError::DeliveredFormExceedsBase,
                      "a capability the base does not need fails");

    EntryPointCode moreMemory = MakeBase();
    moreMemory.CodeFacts.WorkgroupMemoryBytes = 512u;
    runner.CheckEqual(Verify(MakeVariant(moreMemory, MakeBase())), CookError::DeliveredFormExceedsBase,
                      "more workgroup memory than the base fails");

    EntryPointCode constants = MakeBase();
    constants.Constants = EntryPointConstants{ .Delivery = EntryPointConstantsDelivery::PushConstant, .ByteSize = 16u };
    runner.CheckEqual(Verify(MakeVariant(constants, MakeBase())), CookError::DeliveredFormExceedsBase,
                      "push constants the base does not deliver fail");
}

} // namespace

int main()
{
    TestRunner runner{ "DeliveredFormTests" };

    TestAccepted(runner);
    TestRejected(runner);

    return runner.Report();
}
