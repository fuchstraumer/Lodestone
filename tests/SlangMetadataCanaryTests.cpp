#include "TestHarness.hpp"

#include "slang-com-ptr.h"
#include "slang.h"

#include <print>

// Fails when Slang fixes shader-slang/slang#13789. Then delete `ApplySlangWorkaround13789` and its call,
// `ConstantsElementBindings`, `EmittedCodeFacts::DeclaredPlacements`, and this test.

namespace
{

// Output is set 0 binding 0, usedBuf binding 1, and unusedBuf binding 2. The push constant block takes no binding.
constexpr const char* k_Source = R"(
RWStructuredBuffer<float4> Output;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uniform float4 tint,
          uniform StructuredBuffer<float4> usedBuf,
          uniform StructuredBuffer<float4> unusedBuf,
          uint3 tid : SV_DispatchThreadID)
{
    Output[tid.x] = usedBuf[tid.x] * tint;
}
)";

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "SlangMetadataCanary" };
    runner.BeginSection("shader-slang/slang#13789 still exists, so its workaround is still needed");

    Slang::ComPtr<slang::IGlobalSession> global;
    slang_createGlobalSession(SLANG_API_VERSION, global.writeRef());
    slang::TargetDesc target = {};
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile("spirv_1_5");
    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    Slang::ComPtr<slang::ISession> session;
    global->createSession(sessionDesc, session.writeRef());

    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module =
        session->loadModuleFromSourceString("SlangMetadataCanary", "SlangMetadataCanary.slang", k_Source, diagnostics.writeRef());
    runner.Check(module != nullptr, "the canary module loads");
    if (module == nullptr)
    {
        return runner.Report();
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    module->findAndCheckEntryPoint("main", SLANG_STAGE_COMPUTE, entryPoint.writeRef(), diagnostics.writeRef());
    slang::IComponentType* components[] = { module, entryPoint.get() };
    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IComponentType> linked;
    session->createCompositeComponentType(components, 2, composite.writeRef(), diagnostics.writeRef());
    composite->link(linked.writeRef(), diagnostics.writeRef());
    Slang::ComPtr<slang::IMetadata> metadata;
    linked->getEntryPointMetadata(0, 0, metadata.writeRef(), diagnostics.writeRef());
    runner.Check(metadata != nullptr, "the metadata is available");
    if (metadata == nullptr)
    {
        return runner.Report();
    }

    bool unusedReportedUsed = false;
    metadata->isParameterLocationUsed(SLANG_PARAMETER_CATEGORY_DESCRIPTOR_TABLE_SLOT, 0, 2, unusedReportedUsed);
    if (!unusedReportedUsed)
    {
        std::println(stderr,
                     "[SlangMetadataCanary] Slang fixed shader-slang/slang#13789. Delete the workaround: see the "
                     "comment at the top of tests/SlangMetadataCanaryTests.cpp");
    }
    runner.Check(unusedReportedUsed, "Slang's metadata still reports the unused resource as used");

    return runner.Report();
}
