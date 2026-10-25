#include "TestHarness.hpp"

#include "slang-com-ptr.h"
#include "slang.h"

#include <cstdint>
#include <cstring>
#include <print>
#include <span>
#include <string_view>
#include <vector>

// Fails when Slang fixes shader-slang/slang#13895: the direct SPIR-V emitter gives an array of write-only storage images
// no NonReadable. Then delete `IsSlangNonReadableArrayFault` in src/target/SpvValidator.cpp and its call,
// `DeclaredBinding::Array`, the sink parameter of `CompareBindings`, and this test.

namespace
{

constexpr const char* k_Source = R"(
WTexture2D<float4> single;
WTexture2D<float4> many[4];

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    single.Store(id.xy, float4(1.0));
    many[id.z].Store(id.xy, float4(1.0));
}
)";

constexpr uint32_t k_OpName = 5u;
constexpr uint32_t k_OpDecorate = 71u;
constexpr uint32_t k_DecorationNonReadable = 25u;
constexpr size_t k_HeaderWords = 5u;

/** Returns the id that OpName gives `name`, or 0. */
uint32_t FindNamedId(std::span<const uint32_t> words, std::string_view name)
{
    for (size_t i = k_HeaderWords; i < words.size(); i += words[i] >> 16u)
    {
        const uint32_t wordCount = words[i] >> 16u;
        if (wordCount == 0u)
        {
            break;
        }
        if ((words[i] & 0xFFFFu) == k_OpName && wordCount > 2u)
        {
            const char* text = reinterpret_cast<const char*>(&words[i + 2u]);
            if (std::string_view{ text, ::strnlen(text, (wordCount - 2u) * 4u) } == name)
            {
                return words[i + 1u];
            }
        }
    }
    return 0u;
}

bool IsNonReadable(std::span<const uint32_t> words, uint32_t id)
{
    for (size_t i = k_HeaderWords; i < words.size(); i += words[i] >> 16u)
    {
        const uint32_t wordCount = words[i] >> 16u;
        if (wordCount == 0u)
        {
            break;
        }
        if ((words[i] & 0xFFFFu) == k_OpDecorate && wordCount >= 3u && words[i + 1u] == id &&
            words[i + 2u] == k_DecorationNonReadable)
        {
            return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "SlangNonReadableCanary" };
    runner.BeginSection("Slang still gives an array of write-only storage images no NonReadable, so the workaround is still needed");

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
    slang::IModule* module = session->loadModuleFromSourceString("SlangNonReadableCanary",
                                                                 "SlangNonReadableCanary.slang",
                                                                 k_Source,
                                                                 diagnostics.writeRef());
    if (!runner.Check(module != nullptr, "the canary module loads"))
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
    Slang::ComPtr<slang::IBlob> code;
    linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef());
    if (!runner.Check(code != nullptr && code->getBufferSize() % 4u == 0u && code->getBufferSize() > k_HeaderWords * 4u,
                      "the entry point compiles to whole SPIR-V words"))
    {
        return runner.Report();
    }

    std::vector<uint32_t> words(code->getBufferSize() / 4u);
    std::memcpy(words.data(), code->getBufferPointer(), code->getBufferSize());

    const uint32_t singleId = FindNamedId(words, "single");
    const uint32_t manyId = FindNamedId(words, "many");
    runner.Check(singleId != 0u && manyId != 0u, "both variables have a name");
    // the scan can see the decoration, so its absence on the array is a measurement
    runner.Check(IsNonReadable(words, singleId), "the single image is NonReadable");

    const bool arrayNonReadable = IsNonReadable(words, manyId);
    if (arrayNonReadable)
    {
        std::println(stderr,
                     "[SlangNonReadableCanary] Slang fixed shader-slang/slang#13895. Delete the "
                     "workaround: see the comment at the top of tests/SlangNonReadableCanaryTests.cpp");
    }
    runner.Check(!arrayNonReadable, "the array of images still has no NonReadable");

    return runner.Report();
}
