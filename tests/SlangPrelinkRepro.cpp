// Standalone Slang-only probe. Not a unit test. It reproduces a Debug-only Slang assert:
// `unexpected: duplicate global instruction`, from `checkIRDuplicate` inside `prelinkIR`.
// The modules are in tests/assets/SlangPrelinkRepro. The call sequence is the one in the upstream
// issue, so a run of this probe checks the issue text.
//
// Links only against slang. Build the target directly:
//   cmake --build build/<preset> --config Debug --target SlangPrelinkRepro
// Run it with the directory that holds Material.slang and Shading.slang:
//   SlangPrelinkRepro.exe <repro directory>
// Exit code 0: the module loaded. Exit code 1: the module did not load, and the diagnostics print.
#include <slang.h>
#include <slang-com-ptr.h>
#include <cstdio>
#include <print>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::println(stderr, "usage: SlangPrelinkRepro <directory with Material.slang>");
        return 2;
    }

    Slang::ComPtr<slang::IGlobalSession> globalSession;
    slang::createGlobalSession(globalSession.writeRef());

    slang::TargetDesc target{};
    target.format = SLANG_WGSL;

    const char* searchPaths[] = { argv[1] };
    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    sessionDesc.searchPaths = searchPaths;
    sessionDesc.searchPathCount = 1;

    Slang::ComPtr<slang::ISession> session;
    globalSession->createSession(sessionDesc, session.writeRef());

    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModule("Material", diagnostics.writeRef());

    if (diagnostics != nullptr)
    {
        std::println(stderr, "{}", static_cast<const char*>(diagnostics->getBufferPointer()));
    }

    std::println("loadModule(\"Material\") {}", module != nullptr ? "succeeded" : "returned null");
    return module != nullptr ? 0 : 1;
}
