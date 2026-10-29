#include "TestHarness.hpp"

#include "ShaderManifestFormat.hpp"
#include "slang-com-ptr.h"
#include "slang.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Compiles a probe with descriptor handles of every descriptor type, once for each heap lowering, and reads the binding of
// each heap variable from the SPIR-V. The bindings must equal the constants in ShaderManifestFormat.hpp, so a change of
// DefaultVkBindlessBindings or VkMutableBindlessBindings in Slang's hlsl.meta.slang fails here and not in a renderer.

namespace
{

using namespace lodestone;

constexpr const char* k_Probe = R"(
struct Tint
{
    float4 value;
};

struct Handles
{
    DescriptorHandle<Texture2D<float4>> sampled;
    DescriptorHandle<RWTexture2D<float4>> storage;
    DescriptorHandle<SamplerState> filter;
    DescriptorHandle<StructuredBuffer<float4>> readBuffer;
    DescriptorHandle<RWStructuredBuffer<float4>> writeBuffer;
    DescriptorHandle<Buffer<float4>> uniformTexel;
    DescriptorHandle<RWBuffer<float4>> storageTexel;
    DescriptorHandle<ConstantBuffer<Tint>> uniform;
};

ConstantBuffer<Handles> handles;

[shader("compute")]
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float4 total = handles.sampled.SampleLevel(handles.filter, float2(0.5), 0.0);
    total += handles.readBuffer[id.x];
    total += handles.uniformTexel[id.x];
    total += handles.uniform.value;
    handles.storage[id.xy] = total;
    handles.writeBuffer[id.x] = total;
    handles.storageTexel[id.x] = total;
}
)";

constexpr uint32_t k_HeapSet = 3u;
constexpr size_t k_HeaderWords = 5u;
constexpr uint32_t k_OpTypeImage = 25u;
constexpr uint32_t k_OpTypeSampler = 26u;
constexpr uint32_t k_OpTypeSampledImage = 27u;
constexpr uint32_t k_OpTypeRuntimeArray = 29u;
constexpr uint32_t k_OpTypeStruct = 30u;
constexpr uint32_t k_OpTypePointer = 32u;
constexpr uint32_t k_OpVariable = 59u;
constexpr uint32_t k_OpDecorate = 71u;
constexpr uint32_t k_DecorationBinding = 33u;
constexpr uint32_t k_DecorationDescriptorSet = 34u;
constexpr uint32_t k_DimBuffer = 5u;
constexpr uint32_t k_StorageClassUniform = 2u;
constexpr uint32_t k_StorageClassStorageBuffer = 12u;

struct Instruction
{
    uint32_t Opcode{ 0u };
    std::span<const uint32_t> Words;
};

std::vector<Instruction> Decode(std::span<const uint32_t> words)
{
    std::vector<Instruction> instructions;
    for (size_t i = k_HeaderWords; i < words.size();)
    {
        const uint32_t wordCount = words[i] >> 16u;
        if (wordCount == 0u || i + wordCount > words.size())
        {
            break;
        }
        instructions.push_back(Instruction{ .Opcode = words[i] & 0xFFFFu, .Words = words.subspan(i, wordCount) });
        i += wordCount;
    }
    return instructions;
}

/** Returns the descriptor type that the heap variable holds, named as Slang's table names it, or an empty string. */
std::string DescriptorTypeOf(const std::map<uint32_t, Instruction>& definitions, const Instruction& variable)
{
    const auto pointer = definitions.find(variable.Words[1]);
    if (pointer == definitions.end() || pointer->second.Opcode != k_OpTypePointer)
    {
        return {};
    }

    const uint32_t storageClass = pointer->second.Words[2];
    const auto array = definitions.find(pointer->second.Words[3]);
    if (array == definitions.end() || array->second.Opcode != k_OpTypeRuntimeArray)
    {
        return {};
    }

    const auto element = definitions.find(array->second.Words[2]);
    if (element == definitions.end())
    {
        return {};
    }

    const Instruction& type = element->second;
    switch (type.Opcode)
    {
    case k_OpTypeSampler:
        return "sampler";
    case k_OpTypeSampledImage:
        return "combined image sampler";
    case k_OpTypeImage:
    {
        const bool texelBuffer = type.Words[3] == k_DimBuffer;
        const bool sampled = type.Words[7] == 1u;
        if (texelBuffer)
        {
            return sampled ? "uniform texel buffer" : "storage texel buffer";
        }
        return sampled ? "sampled image" : "storage image";
    }
    case k_OpTypeStruct:
        if (storageClass == k_StorageClassStorageBuffer)
        {
            return "storage buffer";
        }
        return storageClass == k_StorageClassUniform ? "uniform buffer" : std::string{};
    default:
        return {};
    }
}

/** Returns the bindings of the heap variables, grouped by descriptor type. */
std::map<std::string, std::set<uint32_t>> ReadHeapBindings(std::span<const uint32_t> words)
{
    const std::vector<Instruction> instructions = Decode(words);
    std::map<uint32_t, Instruction> definitions;
    std::map<uint32_t, uint32_t> setOf;
    std::map<uint32_t, uint32_t> bindingOf;
    for (const Instruction& instruction : instructions)
    {
        if (instruction.Opcode == k_OpDecorate && instruction.Words.size() >= 4u)
        {
            if (instruction.Words[2] == k_DecorationDescriptorSet)
            {
                setOf[instruction.Words[1]] = instruction.Words[3];
            }
            else if (instruction.Words[2] == k_DecorationBinding)
            {
                bindingOf[instruction.Words[1]] = instruction.Words[3];
            }
        }
        else if (instruction.Opcode >= k_OpTypeImage && instruction.Opcode <= k_OpTypePointer && instruction.Words.size() >= 2u)
        {
            definitions[instruction.Words[1]] = instruction;
        }
    }

    std::map<std::string, std::set<uint32_t>> bindings;
    for (const Instruction& instruction : instructions)
    {
        if (instruction.Opcode != k_OpVariable || instruction.Words.size() < 4u)
        {
            continue;
        }

        const uint32_t id = instruction.Words[2];
        if (!setOf.contains(id) || setOf[id] != k_HeapSet)
        {
            continue;
        }

        const std::string type = DescriptorTypeOf(definitions, instruction);
        bindings[type.empty() ? "unknown" : type].insert(bindingOf[id]);
    }
    return bindings;
}

std::vector<uint32_t> Compile(bool typed_lowering, std::string_view override_source, lodestone::tests::TestRunner& runner)
{
    Slang::ComPtr<slang::IGlobalSession> global;
    slang_createGlobalSession(SLANG_API_VERSION, global.writeRef());

    slang::CompilerOptionEntry heapSet{};
    heapSet.name = slang::CompilerOptionName::BindlessSpaceIndex;
    heapSet.value.kind = slang::CompilerOptionValueKind::Int;
    heapSet.value.intValue0 = static_cast<int32_t>(k_HeapSet);

    slang::TargetDesc target = {};
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile("spirv_1_5");
    target.compilerOptionEntries = &heapSet;
    target.compilerOptionEntryCount = 1u;
    slang::SessionDesc sessionDesc = {};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    Slang::ComPtr<slang::ISession> session;
    global->createSession(sessionDesc, session.writeRef());

    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModuleFromSourceString("HeapBindingProbe", "HeapBindingProbe.slang", k_Probe, diagnostics.writeRef());
    if (!runner.Check(module != nullptr, "the probe loads"))
    {
        if (diagnostics)
        {
            std::println(stderr, "{}", static_cast<const char*>(diagnostics->getBufferPointer()));
        }
        return {};
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    module->findAndCheckEntryPoint("main", SLANG_STAGE_COMPUTE, entryPoint.writeRef(), diagnostics.writeRef());

    std::vector<slang::IComponentType*> components{ module, entryPoint.get() };
    if (typed_lowering)
    {
        const std::string source{ override_source };
        slang::IModule* overrideModule = session->loadModuleFromSourceString("LodestoneHeapTyped",
                                                                             "LodestoneHeapTyped.slang",
                                                                             source.c_str(),
                                                                             diagnostics.writeRef());
        if (!runner.Check(overrideModule != nullptr, "the override module loads"))
        {
            return {};
        }
        components.push_back(overrideModule);
    }

    Slang::ComPtr<slang::IComponentType> composite;
    Slang::ComPtr<slang::IComponentType> linked;
    session->createCompositeComponentType(components.data(), static_cast<SlangInt>(components.size()), composite.writeRef(), diagnostics.writeRef());
    if (!runner.Check(composite != nullptr && SLANG_SUCCEEDED(composite->link(linked.writeRef(), diagnostics.writeRef())),
                      "the probe links"))
    {
        if (diagnostics)
        {
            std::println(stderr, "{}", static_cast<const char*>(diagnostics->getBufferPointer()));
        }
        return {};
    }

    Slang::ComPtr<slang::IBlob> code;
    linked->getEntryPointCode(0, 0, code.writeRef(), diagnostics.writeRef());
    if (!runner.Check(code != nullptr && code->getBufferSize() % 4u == 0u, "the probe compiles to SPIR-V"))
    {
        return {};
    }

    std::vector<uint32_t> words(code->getBufferSize() / 4u);
    std::memcpy(words.data(), code->getBufferPointer(), code->getBufferSize());
    return words;
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream stream{ path, std::ios::binary };
    return std::string{ std::istreambuf_iterator<char>{ stream }, std::istreambuf_iterator<char>{} };
}

void CheckBinding(lodestone::tests::TestRunner& runner,
                  const std::map<std::string, std::set<uint32_t>>& bindings,
                  const std::string& descriptor_type,
                  uint32_t expected)
{
    const auto found = bindings.find(descriptor_type);
    if (runner.Check(found != bindings.end(), descriptor_type + " has a heap variable"))
    {
        runner.Check(found->second == std::set<uint32_t>{ expected }, descriptor_type + " is at the binding of the constant");
    }
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "HeapBindingTable" };
    const std::string overrideSource = ReadFile(std::filesystem::path{ LODESTONE_BUILTIN_DIR } / "LodestoneHeapTyped.slang");

    runner.BeginSection("the mutable descriptor lowering (Slang's default) has the bindings of the k_MutableHeap constants");
    const std::vector<uint32_t> mutableWords = Compile(false, overrideSource, runner);
    if (runner.Check(!mutableWords.empty(), "the probe compiles with the default lowering"))
    {
        const std::map<std::string, std::set<uint32_t>> bindings = ReadHeapBindings(mutableWords);
        runner.Check(!bindings.contains("unknown"), "every heap variable has a known descriptor type");
        CheckBinding(runner, bindings, "sampler", manifest::k_MutableHeapSamplerBinding);
        for (const char* type : { "sampled image", "storage image", "uniform texel buffer", "storage texel buffer", "uniform buffer", "storage buffer" })
        {
            CheckBinding(runner, bindings, type, manifest::k_MutableHeapResourceBinding);
        }
    }

    runner.BeginSection("the per-descriptor-type lowering has the bindings of the k_TypedHeap constants");
    const std::vector<uint32_t> typedWords = Compile(true, overrideSource, runner);
    if (runner.Check(!typedWords.empty(), "the probe compiles with the override module"))
    {
        const std::map<std::string, std::set<uint32_t>> bindings = ReadHeapBindings(typedWords);
        runner.Check(!bindings.contains("unknown"), "every heap variable has a known descriptor type");
        CheckBinding(runner, bindings, "sampler", manifest::k_TypedHeapSamplerBinding);
        CheckBinding(runner, bindings, "sampled image", manifest::k_TypedHeapSampledImageBinding);
        CheckBinding(runner, bindings, "storage image", manifest::k_TypedHeapStorageImageBinding);
        CheckBinding(runner, bindings, "uniform texel buffer", manifest::k_TypedHeapUniformTexelBufferBinding);
        CheckBinding(runner, bindings, "storage texel buffer", manifest::k_TypedHeapStorageTexelBufferBinding);
        CheckBinding(runner, bindings, "uniform buffer", manifest::k_TypedHeapUniformBufferBinding);
        // the read and the read-write view are two variables at one binding
        CheckBinding(runner, bindings, "storage buffer", manifest::k_TypedHeapStorageBufferBinding);
    }

    return runner.Report();
}
