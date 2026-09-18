#include "CookerErrors.hpp"
#include "model/ShaderDataSchema.hpp"
#include "ShaderLibraryTypes.hpp"
#include "target/TargetProfile.hpp"
#include "TestHarness.hpp"
#include "target/WgslBindingScanner.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The cross-check compares what reflection reports against what the emitted WGSL actually declares.
// It is the check that stops a wrong shader from reaching a pipeline, and it rests on two pure
// functions that this file tests without Slang.
//
// The asymmetry matters: the emitted artifact decides the group and the binding number, and
// reflection decides the size and the type. A test that fed the same source to both sides would
// prove nothing, so the WGSL text here is fixed and the reflection records are written by hand.

using lodestone::BindingComparison;
using lodestone::BindingKind;
using lodestone::CompareBindings;
using lodestone::CookResult;
using lodestone::ExpectedDeclaredName;
using lodestone::ReflectedBinding;
using lodestone::ScanWgslBindings;
using lodestone::StripSlangNameMangling;
using lodestone::TargetProfile;
using lodestone::WgslAddressSpace;
using lodestone::WgslDeclaredBinding;

namespace
{

// Written the way Slang emits it, mangled suffixes included.
constexpr std::string_view k_Wgsl = R"(
@group(0) @binding(0) var<uniform> IfftParams_0 : IfftParams_Std140_0;

@group(0) @binding(1) var<storage, read> InputSpectrum_0 : array<vec4<f32>>;

@group(0) @binding(2) var<storage, read_write> OutputSpectrum_0 : array<vec4<f32>>;

@group(1) @binding(0) var HeightTexture_0 : texture_2d<f32>;

@group(1) @binding(1) var LinearSampler_0 : sampler;

@group(2) @binding(0) var<storage> DefaultAccess_0 : array<u32>;

@compute @workgroup_size(64, 1, 1)
fn MainCS(@builtin(global_invocation_id) id : vec3<u32>)
{
    return;
}
)";

ReflectedBinding MakeReflected(std::string_view name, uint32_t group, uint32_t binding, BindingKind kind)
{
    ReflectedBinding reflected;
    reflected.Name = std::string{ name };
    reflected.Placement = lodestone::BoundPlacement{ .Group = group, .Binding = binding };
    reflected.Kind = kind;
    return reflected;
}

std::vector<ReflectedBinding> MakeAgreeingReflection()
{
    std::vector<ReflectedBinding> reflected;
    reflected.push_back(MakeReflected("IfftParams", 0u, 0u, BindingKind::UniformBuffer));
    reflected.push_back(MakeReflected("InputSpectrum", 0u, 1u, BindingKind::ReadOnlyStorageBuffer));
    reflected.push_back(MakeReflected("OutputSpectrum", 0u, 2u, BindingKind::StorageBuffer));
    reflected.push_back(MakeReflected("HeightTexture", 1u, 0u, BindingKind::Texture));
    reflected.push_back(MakeReflected("LinearSampler", 1u, 1u, BindingKind::Sampler));
    reflected.push_back(MakeReflected("DefaultAccess", 2u, 0u, BindingKind::ReadOnlyStorageBuffer));
    return reflected;
}

// `CompareBindings` takes a span of mutable pointers, and a span cannot bind a temporary. Each
// caller must keep the result in a named object that is not const.
std::vector<const ReflectedBinding*> PointersTo(const std::vector<ReflectedBinding>& reflected)
{
    std::vector<const ReflectedBinding*> pointers;
    pointers.reserve(reflected.size());
    for (const ReflectedBinding& binding : reflected)
    {
        pointers.push_back(&binding);
    }

    return pointers;
}

const WgslDeclaredBinding* FindDeclared(const std::vector<WgslDeclaredBinding>& declared,
                                        uint32_t group,
                                        uint32_t binding) noexcept
{
    for (const WgslDeclaredBinding& candidate : declared)
    {
        if (candidate.Group == group && candidate.Binding == binding)
        {
            return &candidate;
        }
    }

    return nullptr;
}

bool DeclaredMatches(const std::vector<WgslDeclaredBinding>& declared,
                     uint32_t group,
                     uint32_t binding,
                     WgslAddressSpace address_space,
                     std::string_view name)
{
    const WgslDeclaredBinding* found = FindDeclared(declared, group, binding);
    return found != nullptr && found->AddressSpace == address_space &&
           StripSlangNameMangling(found->Name) == name;
}

} // namespace

int main()
{
    lodestone::tests::TestRunner runner{ "WgslBindingScannerTests" };

    const std::vector<WgslDeclaredBinding> declared = ScanWgslBindings(k_Wgsl);

    runner.BeginSection("the scanner reads every declaration and no more");
    runner.Check(declared.size() == 6u, "six declarations, and the entry point is not one of them");

    runner.BeginSection("the scanner reads the location and the address space");
    runner.Check(DeclaredMatches(declared, 0u, 0u, WgslAddressSpace::Uniform, "IfftParams"),
                 "var<uniform> reads as Uniform");
    runner.Check(DeclaredMatches(declared, 0u, 1u, WgslAddressSpace::StorageRead, "InputSpectrum"),
                 "var<storage, read> reads as StorageRead");
    runner.Check(DeclaredMatches(declared, 0u, 2u, WgslAddressSpace::StorageReadWrite, "OutputSpectrum"),
                 "var<storage, read_write> reads as StorageReadWrite");
    runner.Check(DeclaredMatches(declared, 1u, 0u, WgslAddressSpace::Handle, "HeightTexture"),
                 "a texture has no template list, so it reads as Handle");
    runner.Check(DeclaredMatches(declared, 1u, 1u, WgslAddressSpace::Handle, "LinearSampler"),
                 "a sampler has no template list, so it reads as Handle");
    runner.Check(DeclaredMatches(declared, 2u, 0u, WgslAddressSpace::StorageRead, "DefaultAccess"),
                 "var<storage> with no access word reads as StorageRead, because read is the default");

    runner.BeginSection("the mangled suffix comes off, and nothing else does");
    runner.Check(StripSlangNameMangling("IfftParams_0") == "IfftParams", "a mangled suffix is removed");
    runner.Check(StripSlangNameMangling("IfftParams") == "IfftParams", "a clean name is unchanged");
    runner.Check(StripSlangNameMangling("IfftParams_12") == "IfftParams", "a multi digit suffix is removed");
    runner.Check(StripSlangNameMangling("Buffer0") == "Buffer0",
                 "a trailing digit with no underscore is part of the name");
    runner.Check(StripSlangNameMangling("material_albedoMap_0") == "material_albedoMap",
                 "a scope prefix is not a suffix, and stays");

    runner.BeginSection("a scoped binding states the name the emitted text must declare");
    ReflectedBinding globalBinding;
    globalBinding.Name = "IfftParams";
    runner.Check(ExpectedDeclaredName(globalBinding) == "IfftParams",
                 "a binding at global scope emits under its own name");
    ReflectedBinding scopedBinding;
    scopedBinding.Name = "albedoMap";
    scopedBinding.ScopeName = "material";
    runner.Check(ExpectedDeclaredName(scopedBinding) == "material_albedoMap",
                 "a binding inside a scope emits under the scope chain");

    runner.BeginSection("agreeing reflection passes the cross-check");
    const std::vector<ReflectedBinding> agreeing = MakeAgreeingReflection();
    std::vector<const ReflectedBinding*> agreeingPointers = PointersTo(agreeing);
    const BindingComparison match = CompareBindings(declared, agreeingPointers);
    runner.Check(match.Matches, "reflection that agrees with the emitted text passes");
    runner.Check(match.Report.empty(), "a passing comparison reports nothing");

    runner.BeginSection("an address space mismatch fails the cross-check");
    // A uniform block that reflection calls a storage buffer still emits valid WGSL. WebGPU rejects
    // the bind group layout at run time, so this check is the only place the error is findable.
    std::vector<ReflectedBinding> wrongKind = MakeAgreeingReflection();
    wrongKind[0].Kind = BindingKind::StorageBuffer;
    std::vector<const ReflectedBinding*> wrongKindPointers = PointersTo(wrongKind);
    const BindingComparison kindMismatch = CompareBindings(declared, wrongKindPointers);
    runner.Check(!kindMismatch.Matches, "a uniform declared as a storage buffer fails");
    runner.Check(!kindMismatch.Report.empty(), "a failing comparison says what disagreed");

    runner.BeginSection("a name mismatch fails the cross-check");
    std::vector<ReflectedBinding> wrongName = MakeAgreeingReflection();
    wrongName[1].Name = "SomeOtherBuffer";
    std::vector<const ReflectedBinding*> wrongNamePointers = PointersTo(wrongName);
    const BindingComparison nameMismatch = CompareBindings(declared, wrongNamePointers);
    runner.Check(!nameMismatch.Matches, "a binding whose name disagrees fails");

    runner.BeginSection("a binding that only one side knows fails the cross-check");
    std::vector<ReflectedBinding> missing = MakeAgreeingReflection();
    missing.pop_back();
    std::vector<const ReflectedBinding*> missingPointers = PointersTo(missing);
    const BindingComparison missingBinding = CompareBindings(declared, missingPointers);
    runner.Check(!missingBinding.Matches, "a declaration with no reflection record fails");

    std::vector<ReflectedBinding> extra = MakeAgreeingReflection();
    extra.push_back(MakeReflected("GhostBuffer", 3u, 0u, BindingKind::StorageBuffer));
    std::vector<const ReflectedBinding*> extraPointers = PointersTo(extra);
    const BindingComparison extraBinding = CompareBindings(declared, extraPointers);
    runner.Check(!extraBinding.Matches, "a reflection record the WGSL never declares fails");

    // D7 moved the cross-check behind `ResolvedLibraryValidator` so a second target can supply its
    // own. Moving a seam must not change the thing behind it, so the validator reached through the
    // interface has to give the same answer as the two functions called directly. If these ever
    // disagree, the adapter grew an opinion it is not allowed to have.
    runner.BeginSection("the target profile reaches the same scanner");
    const CookResult<TargetProfile> wgslProfile = lodestone::FindTargetProfile("wgsl");
    runner.Check(wgslProfile.has_value(), "the build has a wgsl profile");
    runner.Check(wgslProfile.has_value() && wgslProfile->Access == lodestone::AccessModel::Bound,
                 "wgsl places a resource by group and binding");
    runner.Check(wgslProfile.has_value() && wgslProfile->Validator != nullptr,
                 "wgsl can read its own output, so it supplies a validator");
    runner.Check(!lodestone::FindTargetProfile("hlsl"),
                 "a target this build does not have resolves to nothing");

    if (wgslProfile.has_value() && wgslProfile->Validator != nullptr)
    {
        const BindingComparison throughInterface =
            wgslProfile->Validator->ValidateEntryPoint(k_Wgsl, agreeingPointers);
        const std::vector<WgslDeclaredBinding> rescanned = ScanWgslBindings(k_Wgsl);
        const BindingComparison direct = CompareBindings(rescanned, agreeingPointers);
        runner.Check(throughInterface.Matches == direct.Matches,
                     "the validator agrees with the scanner on output that matches");

        const BindingComparison mismatchThroughInterface =
            wgslProfile->Validator->ValidateEntryPoint(k_Wgsl, extraPointers);
        runner.Check(mismatchThroughInterface.Matches == extraBinding.Matches &&
                         mismatchThroughInterface.Report == extraBinding.Report,
                     "the validator agrees with the scanner on output that does not, report included");
    }

    return runner.Report();
}
