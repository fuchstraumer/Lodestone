// Standalone Slang-only probe. Not a unit test. It reflects a parameter-block-of-parameter-blocks
// (the shape KsMaterial's `Surface` has: a block whose element holds only nested blocks and no ordinary
// data) and dumps how Slang represents it: the binding ranges of each scope (name, binding type,
// descriptor set index) and the sub-object ranges, plus the uniform size of each element type.
//
// It answers why the ordinary range walk sees `Surface` as a UniformBuffer with a zero byte size.
//
// Links only against slang. Build the target directly:
//   cmake --build build/<preset> --config <cfg> --target DeclKindProbe
#include <slang.h>
#include <slang-com-ptr.h>
#include <cstdio>
#include <print>

using Slang::ComPtr;

static const char* BindingTypeName(slang::BindingType t)
{
    switch (t)
    {
    case slang::BindingType::Unknown:                 return "Unknown";
    case slang::BindingType::Sampler:                 return "Sampler";
    case slang::BindingType::Texture:                 return "Texture";
    case slang::BindingType::ConstantBuffer:          return "ConstantBuffer";
    case slang::BindingType::ParameterBlock:          return "ParameterBlock";
    case slang::BindingType::TypedBuffer:             return "TypedBuffer";
    case slang::BindingType::RawBuffer:               return "RawBuffer";
    case slang::BindingType::CombinedTextureSampler:  return "CombinedTextureSampler";
    case slang::BindingType::InputRenderTarget:       return "InputRenderTarget";
    case slang::BindingType::InlineUniformData:       return "InlineUniformData";
    case slang::BindingType::MutableFlag:             return "MutableFlag";
    case slang::BindingType::MutableTexture:          return "MutableTexture";
    case slang::BindingType::MutableTypedBuffer:      return "MutableTypedBuffer";
    case slang::BindingType::MutableRawBuffer:        return "MutableRawBuffer";
    case slang::BindingType::VaryingInput:            return "VaryingInput";
    case slang::BindingType::VaryingOutput:           return "VaryingOutput";
    case slang::BindingType::ExistentialValue:        return "ExistentialValue";
    case slang::BindingType::PushConstant:            return "PushConstant";
    default:                                          return "(other)";
    }
}

// Dump one scope's binding ranges and sub-object ranges. Not recursive on its own; the caller descends.
static void DumpScope(const char* label, slang::TypeLayoutReflection* scope)
{
    if (scope == nullptr)
    {
        std::println("[{}] <null layout>", label);
        return;
    }
    std::println("\n[{}]  element uniform size = {} bytes", label,
           scope->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM));

    const SlangInt rangeCount = scope->getBindingRangeCount();
    printf("  binding ranges: %d\n", (int)rangeCount);
    for (SlangInt r = 0; r < rangeCount; ++r)
    {
        slang::BindingType bt = scope->getBindingRangeType(r);
        SlangInt descSet = scope->getBindingRangeDescriptorSetIndex(r);
        slang::VariableReflection* v = scope->getBindingRangeLeafVariable(r);
        slang::TypeLayoutReflection* leaf = scope->getBindingRangeLeafTypeLayout(r);
        std::println("    range[{}] name={:10} type={:16} descSet={}  leafUniformSize={}  elemUniformSize={}",
               static_cast<int>(r),
               ((v != nullptr) && (v->getName() != nullptr)) ? v->getName() : "(none)",
               BindingTypeName(bt),
               static_cast<int>(descSet),
               (leaf != nullptr) ? leaf->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM) : (size_t)0,
               ((leaf != nullptr) && (leaf->getElementTypeLayout() != nullptr))
                   ? leaf->getElementTypeLayout()->getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)
                   : static_cast<size_t>(0));
    }

    const SlangInt subCount = scope->getSubObjectRangeCount();
    std::println("  sub-object ranges: {}", (int)subCount);
    for (SlangInt s = 0; s < subCount; ++s)
    {
        SlangInt br = scope->getSubObjectRangeBindingRangeIndex(s);
        slang::BindingType bt = (br >= 0) ? scope->getBindingRangeType(br) : slang::BindingType::Unknown;
        SlangInt descSet = (br >= 0) ? scope->getBindingRangeDescriptorSetIndex(br) : -99;
        std::println("    sub[{}] -> bindingRange={} type={} descSet={}",
               static_cast<int>(s), static_cast<int>(br), BindingTypeName(bt), static_cast<int>(descSet));
    }
}

int main()
{
    ComPtr<slang::IGlobalSession> globalSession;
    if (SLANG_FAILED(slang::createGlobalSession(globalSession.writeRef())))
    {
        std::println("createGlobalSession failed");
        return 1;
    }

    slang::TargetDesc target{};
    target.format = SLANG_WGSL;
    target.profile = globalSession->findProfile("spirv_1_4");

    slang::SessionDesc sessionDesc{};
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(globalSession->createSession(sessionDesc, session.writeRef())))
    {
        std::println("createSession failed");
        return 1;
    }

    // The KsMaterial shape: a block whose element holds only nested blocks (no ordinary data).
    const char* source =
        "struct Mat { Texture2D<float4> Albedo; SamplerState S; };\n"
        "struct Shad { Texture2DArray<float> Cascades; SamplerComparisonState CS; };\n"
        "struct SurfaceResources { ParameterBlock<Mat> Material; ParameterBlock<Shad> Shadow; float4x4 Matrix; };\n"
        "ParameterBlock<SurfaceResources> Surface;\n"
        "RWStructuredBuffer<float4> Out;\n"
        "[shader(\"compute\")][numthreads(1,1,1)]\n"
        "void cs(uint3 t : SV_DispatchThreadID)\n"
        "{\n"
        "    float4 a = Surface.Material.Albedo.SampleLevel(Surface.Material.S, float2(0,0), 0);\n"
        "    float4 m = Surface.Matrix[0];\n"
        "    float  s = Surface.Shadow.Cascades.SampleLevel(Surface.Material.S, float3(0,0,0), 0);\n"
        "    Out[t.x] = a + m + s;\n"
        "}\n";

    ComPtr<slang::IBlob> diag;
    slang::IModule* module = session->loadModuleFromSourceString("Probe", "Probe.slang", source, diag.writeRef());
    if (diag && diag->getBufferSize() > 0)
    {
        std::println("diagnostics:\n{}", static_cast<const char*>(diag->getBufferPointer()));
    }
    if (module == nullptr)
    {
        printf("module load failed\n");
        return 1;
    }

    ComPtr<slang::IBlob> layoutDiag;
    slang::ProgramLayout* pl = module->getLayout(0, layoutDiag.writeRef());
    if (layoutDiag && layoutDiag->getBufferSize() > 0)
    {
        std::println("layout diagnostics:\n{}", static_cast<const char*>(layoutDiag->getBufferPointer()));
    }
    if (pl == nullptr)
    {
        std::println("getLayout failed");
        return 1;
    }

    slang::TypeLayoutReflection* global = pl->getGlobalParamsTypeLayout();
    DumpScope("global scope", global);

    // Emit the WGSL, so the actual @group/@binding numbers can be read out. Reflection reports space
    // offsets; the emitted text decides the real group numbers, so this is the ground truth for whether
    // the empty 'Surface' container consumes a group.
    {
        slang::IEntryPoint* entry = nullptr;
        module->findEntryPointByName("cs", &entry);
        if (entry != nullptr)
        {
            slang::IComponentType* parts[2] = { module, entry };
            ComPtr<slang::IComponentType> composed;
            ComPtr<slang::IBlob> composeDiag;
            if (SLANG_SUCCEEDED(session->createCompositeComponentType(
                    parts, 2, composed.writeRef(), composeDiag.writeRef())))
            {
                ComPtr<slang::IComponentType> linked;
                ComPtr<slang::IBlob> linkDiag;
                composed->link(linked.writeRef(), linkDiag.writeRef());
                if (linked != nullptr)
                {
                    ComPtr<slang::IBlob> code;
                    ComPtr<slang::IBlob> codeDiag;
                    if (SLANG_SUCCEEDED(linked->getEntryPointCode(
                            0, 0, code.writeRef(), codeDiag.writeRef())) &&
                        code != nullptr)
                    {
                        std::println("\n=== emitted WGSL ===\n{:.{}}",
                               static_cast<const char*>(code->getBufferPointer()),
                               (int)code->getBufferSize());
                    }
                    else if (codeDiag && codeDiag->getBufferSize() > 0)
                    {
                        std::println("\ngetEntryPointCode diag:\n{}",
                               static_cast<const char*>(codeDiag->getBufferPointer()));
                    }
                }
                else if (linkDiag && linkDiag->getBufferSize() > 0)
                {
                    std::println("\nlink diag:\n{}", static_cast<const char*>(linkDiag->getBufferPointer()));
                }
            }
        }
    }

    // Descend one level into the Surface block's element, to show its nested blocks.
    if (global != nullptr)
    {
        for (SlangInt s = 0; s < global->getSubObjectRangeCount(); ++s)
        {
            SlangInt br = global->getSubObjectRangeBindingRangeIndex(s);
            if (br < 0)
            {
                continue;
            }
            slang::TypeLayoutReflection* blockLayout = global->getBindingRangeLeafTypeLayout(br);
            slang::VariableReflection* v = global->getBindingRangeLeafVariable(br);
            if (blockLayout != nullptr && blockLayout->getElementTypeLayout() != nullptr)
            {
                char lbl[64];
                snprintf(lbl, sizeof(lbl), "element of block '%s'", (v && v->getName()) ? v->getName() : "?");
                DumpScope(lbl, blockLayout->getElementTypeLayout());
            }
        }
    }

    return 0;
}
