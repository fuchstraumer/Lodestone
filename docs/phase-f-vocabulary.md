# Phase F vocabulary: targets, binding times, and access models

This document gives the words. It is not a plan. It exists so that a later session, or a later person,
starts with the same terms and does not build a second vocabulary next to this one.

Phase F is the long-horizon work: one shader source that serves both a WebGPU target and a desktop
Vulkan target that reaches resources through buffer device addresses. Read
`docs/phase-d-stage-separation-plan.md` and `docs/phase-e-data-driven-permutations.md` first. Phase F
comes after both, and section 8 states what each of them must do to prepare for it.

---

## 1. The word "permutation" holds four different ideas

This is the whole tangle. Four properties are orthogonal, and one word covers all of them. Separate
them and most of the difficulty goes away.

| Property | Question it answers |
|---|---|
| **Axis kind** | What varies? |
| **Binding time** | When must the value be known? |
| **Access model** | How does the shader reach a resource? |
| **Interface contract** | What must the client be told? |

Every axis has a kind and a binding time. Every resource has an access model. The interface contract
is what the cooker emits, and it is a function of the other three.

---

## 2. Axis kind

| Kind | Example | Can a run-time check replace it? |
|---|---|---|
| **Resource presence** | Is there an albedo map? | **Yes.** A null pointer, or an index of −1 |
| **Capability** | Are wave operations available? Is the wave 32 or 64 wide? | **No.** Different instructions are emitted |
| **Tuning** | `IFFT_SIZE`, workgroup dimensions, static array extents | **No.** It sizes `groupshared` and controls unrolling |
| **Technique** | Which BRDF | Only as a uniform branch, and the code size is the cost |

**Buffer device addresses collapse exactly one of these four kinds.** This is the most important
scoping fact in phase F.

`AssignLightsResourcePointers` in `tests/assets/compute/VolumeTiledForwardShading/` collapses sixteen
resources into sixteen pointers because every one of them is a resource-presence or resource-identity
axis. The IFFT axes do not collapse at all, because `IFFT_USE_WAVE_OPS` is a capability axis and
`IFFT_SIZE` is a tuning axis.

So the collapse is large for material and lighting shaders, and it is zero for signal-processing
compute. Expect that, and do not plan for a uniform win.

### The pattern is not new here

`tests/assets/volumetric_forward/Clustered.frag:272` reads:

```glsl
const bool hasAlbedoMap = indices.albedoMapIdx != -1;
```

That is `if (resources.albedoMap != nullptr)` with one more level of indirection. The 2016 GLSL
renderer already moved resource-presence axes to run time, with −1 as the null pointer and a heap
index as the address. Buffer device addresses do not introduce the idea. They remove the indirection
and give the sentinel a type.

---

## 3. Binding time

The binding time of an axis is the latest point at which its value must be known.

| Binding time | Mechanism | Cost of one more value |
|---|---|---|
| **Cook** | Link-time constant. `extern const static` | One more compiled variant |
| **Bind** | Specialization constant, or a WGSL `override` | One more pipeline object |
| **Invocation** | Push constant, then a uniform branch | One branch, uniform across the dispatch |
| **Execution** | A divergent branch | Divergence |

Each name is the **action** that fixes the value, not a stage noun: the cook, the pipeline bind, one
draw or dispatch invocation, and the thread's own execution. This reads as a point on the pipeline
rather than a mix of nouns and verbs. The `EarliestBindingTime` enum uses these four names. They were
formerly cook, pipeline, draw, and thread.

Examples from this repository: `[SpecializationConstant] const uint MaxLights = 2048` in
`VtfAssignLightsToClustersBVH.slang` is bind time. The `!= -1` test in `Clustered.frag` is invocation
time. `IFFT_SIZE` is cook time.

### Earliest sound binding time

**An author declares the earliest binding time at which an axis is sound. The cooker and the client
then move it later when the target permits.**

This is the primitive, and the reason is portability past one API.

The author knows which binding times are correct. An axis that sizes a `groupshared` array is
cook-time and can never be later. An axis that only selects a pointer is sound at invocation time and at
every earlier time as well. That statement is a property of the shader, and it does not change when
the target changes.

The target and the device then decide how late the value really binds. A client queries the live
adapter, gets its feature set, maps that to a target profile, and asks the manifest for the variants
that profile can run. **The client never parses extensions to decide which shader to load. It asks
for a profile, and the manifest answers.**

This is the fix for a real problem: making a Vulkan engine portable used to mean parsing features and
extensions, then changing the RHI code, then changing the shaders, and then maintaining every variant
for every capability by hand. The declaration moves that work into the cooker, once.

### `MaxVariants` and the policy file already hold the skeleton

A lowering pass needs somewhere to write the target-specific decision. `ModulePolicy` and the Phase E
policy file are that place. A per-target section states which axes stay at cook time for that target
and which move later, and `MaxVariants` still caps the result.

---

## 4. Access model

| Model | Shader side | Client contract | Era in this author's work |
|---|---|---|---|
| **Bound** | `Texture2D t` inside a bind group | Bind group layout: group, binding, kind, count | WebGPU, today |
| **Indexed** | `Heap[idx]`, and `idx != -1` | A heap, plus an index carried in data | volumetric_forward, 2016 |
| **Pointer** | `PointLight* p`, and `p != nullptr` | A pointer table, pushed as constants | VolumeTiledForwardShading, now |

### Pointers do not cover textures

Buffer device addresses collapse **buffers**. They do not collapse textures and samplers. Those need
the descriptor-heap mechanism, which is a different extension and a different spelling in the shader:
an index, not a pointer.

So a "Desktop Vulkan [Modern]" resource struct is **heterogeneous**. It holds pointer fields for
buffers and index fields for textures, and both arrive through push constants. That is the same shape
`Clustered.frag` already has. Do not design for a table of pure pointers.

---

## 5. The layout does not disappear. It moves, and it gets less safe.

Under the bound model the layout is a `VkDescriptorSetLayout`. The driver knows it, and a mistake is a
validation error.

Under the pointer model the layout is the field order and the byte offsets of
`AssignLightsResourcePointers`. Something on the CPU must write sixteen pointers in exactly that
order, and must know that `spotLights` points at 96-byte records. **A mistake there is a page fault or
silent bad data. No validation layer sees it.**

So the cost of a cook goes down under phase F, and the value of the cooker goes up. It becomes the
only thing between the engine and an unchecked CPU-to-GPU ABI.

The repository already emits the correct shape. `ReflectedUniformMember`, with `Name`, `Offset`,
`Size`, and `ArrayCount`, is a pointer-table descriptor. It was added for the same reason: a
hand-mirrored CPU struct whose padding is one float wrong writes every field after that point to the
wrong place, and nothing reports it.

Important note: With this expansion in capabilities, `ReflectionUniformMember::Offset` is no longer
a parameter of the struct, it's a paramter of the *target*. Under a target of WebGPU, it may be aligned
to one normalized layout (`std430`/`std130`) - but when using a different target where it becomes viewed
through `GL_EXT_buffer_reference`, the layout rule may be different and the offsets could shift.

We will need to record member offsets per target profile, and then run the round trip testing this per
profile. The unsafe design is one offset table that happens to be right for one target, and then
silently and sneakily wrong for another.

The 2016 system never closed this loop. The YAML generated the GLSL block and the CPU struct, but
nothing checked afterwards that the two still agreed. The cross-check asymmetry rule is that check.

---

## 6. Slang's capability system: what it gives, and what it does not

Checked against `third_party/slang` in this repository:
`source/slang/slang-capabilities.capdef` and `docs/user-guide/05-capabilities.md`.

### What it gives

**A closed, curated set of atoms** for targets, stages, versions, and extensions, with conjunction
(`+`) and disjunction (`|`), and aliases that hide the per-target spelling.

```
alias subgroup_basic = GL_KHR_shader_subgroup_basic | _sm_6_0 | _cuda_sm_7_0 | wgsl | metal;
```
*(capdef:2504)*

**`[require()]` on entry points works.** The user guide is explicit:

> "Slang recommends but does not require explicit declaration of capability requirements for entry
> points. If explicit capability requirements are declared on an entry point, they will be used to
> validate the entry point in the same way as other public methods."

So an entry point can be marked for one target. The earlier reading that only ordinary functions
accept `[require]` was wrong, and the annotation you wanted for per-target entry points exists today.

**`[require()]` on a module declaration** applies to every member, so a whole file can be marked for
one target family.

**Inference.** An `internal` or `private` function gets its requirement inferred from its body. Only
public and interface methods must declare.

**`__target_switch` and `__stage_switch`** give per-target bodies in one function, and the inferred
requirement becomes the disjunction of the cases. **This is the mechanism for one module that serves
two access models**, and it is more precise than a preprocessor branch, because the compiler checks
each case against its own target.

**The atoms phase F needs already exist:**

| Atom | Meaning | Line |
|---|---|---|
| `GL_EXT_buffer_reference` | Buffer device address. Aliases to `SPV_EXT_physical_storage_buffer` | capdef:1072 |
| `SPV_EXT_descriptor_heap`, `spvDescriptorHeapEXT` | The descriptor heap path for textures | capdef:703, 977 |
| `descriptor_handle` | Targets with `DescriptorHandle` bindless access. **Includes `wgsl`** | capdef:1474 |
| `wgsl` | The WebGPU target | capdef:116 |

`descriptor_handle` is worth a second look. Slang gives a bindless handle type whose supported set
includes WGSL. If that type is expressive enough, it is a portable seam for the indexed access model,
and not only for the desktop target.

### What it does not give

**No user-defined atoms.** The capdef file states it is the single source of truth, and a new
capability means editing that file. So a technique axis such as "which BRDF" can never be a capability
atom. Do not plan to express one that way.

**Capability atoms are resolved at cook time, against a fixed target profile.** They say what a
*target language* can express. They do not say what a *device* supports.

### The line that decides everything

`subgroup_basic` includes `wgsl`. That means WGSL **can express** subgroup operations. It does not
mean the adapter in front of the user supports them. WebGPU subgroups are an optional feature, and the
answer differs per adapter. The same is true of wave width on Vulkan: 32 on one vendor, 64 on another,
one target, one extension set, two answers.

So:

> **A capability that varies between targets belongs to Slang. A capability that varies between
> devices inside one target belongs to Lodestone.**

Slang resolves the first at cook time. Lodestone cooks both forms of the second and lets the client
choose at run time. This is exactly the problem you described in Vulkan, and this sentence is the
division of labour that solves it.

`IFFT_USE_WAVE_OPS` and `IFFT_WAVE_SIZE` therefore stay Lodestone axes. They are not portability
questions. They are device questions.

### Each axis kind gets one mechanism

This is the payoff of the vocabulary.

| Axis kind | Mechanism | Owner |
|---|---|---|
| **Capability**, varying by target | `[require()]`, `__target_switch`, aliases | **Slang** |
| **Capability**, varying by device | A cooked axis, selected at run time | **Lodestone** |
| **Technique** | `interface` and generic specialization | **Slang**, driven by a Lodestone axis |
| **Tuning** | A cooked axis, or a specialization constant | **Lodestone** |
| **Resource presence** | The access model, and a null or −1 test | **Lodestone**, through the target profile |

Your reading was right in part, and the part it is right about is large: the target-varying half of the
capability kind, and all of the technique kind, move into Slang. What stays is the device-varying half,
the tuning kind, and the access model. That is still the majority of what Phase E builds, and Phase E
does not get smaller. It gets better aimed.

---

## 7. Module reuse: the authoring rule

The goal is one set of `.slang` modules shared between the WebGPU project and the desktop engine.

**A raw pointer must never appear in a module that is meant to be shared.** `PointLight* p` cannot
compile for WGSL. The pointer form is a lowering *output*, never an authoring *input*.

The seam that survives both targets is `ParameterBlock<T>` over typed buffer views, not loose globals
and not pointers. `ParameterBlock<T>` is Slang's own statement that a group of parameters binds
together, and it is target-directed by design.

Three rules follow:

1. **Group resources into a `ParameterBlock`.** One block is one bind group on WebGPU, and one
   pointer struct on the desktop target.
2. **Author against typed views.** `StructuredBuffer<PointLight>`, never `PointLight*`.
3. **Use `__target_switch` where the two models really differ**, and keep the switch inside a small
   accessor function, so the shading code above it does not change.

The reuse boundary is the **module**, not the entry point. An entry point may carry `[require()]` to
mark it for one target, and that is the escape hatch when a whole technique only makes sense on one
side. Anything finer than an entry point becomes unmanageable.

---

## 8. What phases D and E must do for phase F

Small now, expensive later.

**The manifest redesign is the vehicle for most of this.** After Phase E, the manifest becomes
multi-module and multi-level (`todo.md`, "Phase F preparation"). That redesign is the cheap moment to
land the items below: the per-variant capability requirement, the access model on the profile, and the
placement change. It splits into three scopes: whole-cook data (strings, source, the axes table, the
target-profile table), per-module logical data (the variant table, profile-invariant), and
per-(module, profile) data (the baked layout, offsets, and capability requirement). Do these here, not
after, or the format hardens twice.

**D7 — the target profile carries an access model from the first commit.** Add the field even while
`Bound` is the only value. The profile is already being built as a stub, so a second field costs
nothing.

**D4 — `Group` and `Binding` must not be mandatory on a raw binding.** They are bound-model concepts.
Under the pointer model the placement is a byte offset in a struct. Model placement as a variant, or
at least make the fields optional. This is the field most likely to harden into a WebGPU assumption.
The handoff already records the smell: *"`BindingKind` is WebGPU shaped, and the schema has no push
constants and no specialization constants."*

**E2 — an axis carries `AxisKind` and `EarliestBindingTime`.** Two fields, and they are the whole of
section 2 and section 3.

**E5 — the policy file has per-target sections.** This is where the lowering decision is written down.

**E6 — add push constants and specialization constants to the schema** while the attribute vocabulary
is open.

**New, and phase F cannot work without it: the manifest must record, for each variant, the capability
requirement it was cooked for.** A client that queries the adapter and asks the manifest for a
runnable variant needs that field to exist. It is a small schema addition, and it is much cheaper to
add while the manifest is already changing.

---

## 9. Open questions

Answer these before phase F becomes a plan. Each is a spike with a written result.

1. **Can Slang lower a `StructuredBuffer<T>` inside a `ParameterBlock` to a buffer device address
   pointer, or must the source spell an explicit pointer type?** This decides whether one module
   serves both targets unchanged, or whether `__target_switch` must appear in every accessor.

   **Answered on 2026-08-20. The source must declare the pointer.** See §9a.
2. **How far does `DescriptorHandle` reach?** `descriptor_handle` includes `wgsl` (capdef:1474). If
   that type covers textures on both targets, the indexed access model is portable and the access
   model question shrinks to buffers alone.

   **Answered on 2026-09-24. SPIR-V only. The WGSL output is not valid.** See §9b.
3. **What can a WGSL `override` legally size?** Workgroup size, workgroup array extents? This decides
   whether bind time is a real third binding time on WebGPU, and several current cook-time axes
   may move.

   **Answered on 2026-09-24. Through Slang, a WGSL `override` sizes nothing.** See §9c.
4. **Confirm the exact Vulkan extension names and driver coverage** for the address-command and
   descriptor-heap features. That set moves quickly, and no phase should rest on a half-remembered
   name.

   **Answered on 2026-09-24.** See §9d.

---

## 9a. The answer to question 1

`slangc` 2026.14.1 compiled two modules. Each module holds the same shading code. Only the resource
declaration differs.

| Declaration | Memory model | How the buffer arrives |
|---|---|---|
| `ParameterBlock<Lights>` that holds `StructuredBuffer<PointLight>` | `Logical GLSL450` | descriptor set 1, binding 1 |
| `PointLight*` in a push constant | `PhysicalStorageBuffer64 GLSL450` | `OpPtrAccessChain`, array stride 32 |

No compiler option changes this result. Slang states the same rule in
`docs/user-guide/a2-01-spirv-target-specific.md:295`. A module that uses a pointer type gets
`PhysicalStorageBuffer64`. A module that uses no pointer type gets `Logical`.

The bound form gives the same shape that WGSL gives for one source. The block takes a space of its
own. The two members take binding 0 and binding 1. **One module therefore serves both targets with no
change, while both targets use the bound access model.**

> **Correction, 2026-09-24.** The two subsections below are wrong. Link time specialization *can*
> select the access model. §9e holds the measurement. Keep the text below as the record of the error.

### Link time specialization cannot select the access model

Each module has one `OpMemoryModel` instruction. The comparison above shows that the instruction
changes when a pointer type appears anywhere in the module. The addressing model is a property of the
module.

Link time specialization selects an implementation inside a module. The memory model of that module is
already set. The pointer form therefore needs the memory model before the step that decides the memory
model. The mechanism cannot express the choice.

**Lodestone must generate the parameter block for each access model before compilation.**

### `__target_switch` is not the mechanism

A source level switch on the target selects the pointer form for every SPIR-V device. Some SPIR-V
devices support no buffer device address. Section 6 states the rule that decides this:

> A capability that varies between targets belongs to Slang. A capability that varies between devices
> inside one target belongs to Lodestone.

Buffer device address support varies between devices inside one target. The access model is therefore
a Lodestone dimension. One cook must produce a bound form and a pointer form of one SPIR-V module.
The client selects one of them at run time.

A switch inside an accessor does compile, and both arms give clean output. Slang removes the unused
declaration, so the WGSL arm holds no pointer and the SPIR-V arm takes no descriptor set for the
unused block. The mechanism works. It answers the wrong question.

### Reflection supplies what a generator needs

The pointer form reflects with a full layout.

| Field | Kind | Offset | Size | Alignment |
|---|---|---|---|---|
| `Points` | `pointer`, value type `PointLight` | 0 | 8 | 8 |
| `Count` | `uint32` | 8 | 4 | 4 |
| the structure | — | — | 16 | 8 |

`Count` starts at byte 8. A CPU structure that gives the handle 4 bytes writes each later field to the
wrong address. Section 5 states that failure, and no validation layer reports it. Reflection gives the
correct offsets, so the cooker can own the table that section 5 asks for.

The bound form reflects the same block with different offsets. **An offset is a property of the
target, and not a property of the structure.** Section 5 states this too.

---

## 9b. The answer to question 2

Measured on 2026-09-24. Slang at `6eb89786c`. Tint ran through `ls_cooker_console --target=wgsl`.
`spirv-val` ran with `--target-env vulkan1.3`.

The probe is one compute entry point. A `ConstantBuffer` holds four handles:
`Texture2D<float4>.Handle`, `SamplerState.Handle`, `StructuredBuffer<float4>.Handle`, and
`RWTexture2D<float4>.Handle`. The shader samples, loads, and stores through them.

| Target | Slang | Validator | Result |
|---|---|---|---|
| WGSL | compiles | Tint rejects | invalid for each kind |
| SPIR-V, default | compiles | `spirv-val` passes | a mutable descriptor heap in set 1 |
| SPIR-V, `-capability spvDescriptorHeapEXT` | compiles | `spirv-val` passes | `SPV_EXT_descriptor_heap`, no heap set |

Tint rejects each kind on its own. A probe with one handle kind gave each error below.

| Handle | Tint error |
|---|---|
| sampled texture | `texture_2d<f32> cannot be used as an element type of an array` |
| storage texture | `texture_storage_2d<...> cannot be used as an element type of an array` |
| sampler | `sampler cannot be used as an element type of an array` |
| structured buffer | `an array element type cannot contain a runtime-sized array` |

The WGSL has two more faults. The uniform struct holds the texture type and the sampler type as
members, not a `vec2<u32>`. Four heap variables share `@group(1) @binding(0)`.

The cause is in `hlsl.meta.slang`, `defaultGetDescriptorFromHandle`. The `wgsl` case reads
`__getDynamicResourceHeap<T>()[handle.x]`. That is a runtime-sized array of a resource type. Core
WGSL has no binding array, so no emitter change can make this valid today.

**The capability atom states what Slang accepts, not what the target validates.** `descriptor_handle`
includes `wgsl`, and the WGSL output is still invalid.

Consequences:

- **The Indexed access model is not portable to WebGPU.** WebGPU stays Bound. Section 4 does not change.
- **On SPIR-V, the Indexed model has two lowerings.** The default uses `VK_EXT_mutable_descriptor_type`
  (the `VkMutable` bindless option). The heap form uses `VK_EXT_descriptor_heap` and
  `SPV_KHR_untyped_pointers`. These are two device capabilities inside one target, so the section 6
  rule makes them a Lodestone dimension.
- **Reflection names the heap.** `getBindlessSpaceIndex()` gave 1 for the default form.
  `IBindlessResourceMetadata` on the target metadata states whether a heap path survived codegen. The
  handle members reflect as ordinary data in the constant buffer. A SPIR-V validator must accept the
  heap set, which no reflected parameter names.

## 9c. The answer to question 3

Measured on 2026-09-24. Slang at `6eb89786c`. Each probe declares
`[SpecializationConstant] const uint N`, and uses it in one place.

| Use of `N` | WGSL | SPIR-V |
|---|---|---|
| A value in code | `@id(0) override`. Tint accepts it. | `OpSpecConstant`. `spirv-val` passes. |
| `[numthreads(N, 1, 1)]` | Slang error E55205 | `LocalSizeId`. `spirv-val` passes. |
| `groupshared float Tile[N]` | Slang internal error E99999 during emit | `OpSpecConstantOp` extent. `spirv-val` passes. |
| A function-local array `float a[N]` | Slang internal error E99999 during emit | not measured |

The WGSL language accepts an override expression in `@workgroup_size` and in the extent of a
`workgroup` array. Tint states both rules in `third_party/dawn/src/tint/lang/wgsl/resolver/resolver.cc`,
lines 3913 and 4801. The limit is in Slang. The WGSL emitter calls the two-argument
`getComputeThreadGroupSize` in `slang-emit-c-like.cpp:293`, and that function raises E55205 for each
specialization constant. A function-local array needs a constant extent in WGSL, so that row is a
language limit too.

Consequences:

- **Bind time on WebGPU is real only for a value that sizes nothing.** A tuning axis that sizes a
  workgroup or a `groupshared` array stays cook time on WGSL.
- **On SPIR-V, bind time covers all three uses.** The lowering pass must therefore decide per target.
  One axis can be bind time on SPIR-V and cook time on WGSL. Section 3 predicted this.
- **Upstream issues to file:** E55205 on WGSL, and the internal error for a `groupshared` extent. The
  second is a crash, not a diagnostic.

## 9d. The answer to question 4

Read on 2026-09-24 from `third_party/dawn/third_party/vulkan-headers/src/registry/vk.xml`, header
version 363. That is `v1.4.363`, the newest Vulkan-Headers tag (2026-09-18). Slang's copy is at 347,
and it agrees on every name below.

| Mechanism | Vulkan extension | Feature bit | SPIR-V |
|---|---|---|---|
| Pointer (buffer device address) | `VK_KHR_buffer_device_address`, core in 1.2 | `VkPhysicalDeviceVulkan12Features::bufferDeviceAddress` | `SPV_KHR_physical_storage_buffer`, capability `PhysicalStorageBufferAddresses` |
| Descriptor heap | `VK_EXT_descriptor_heap` (number 136) | `VkPhysicalDeviceDescriptorHeapFeaturesEXT::descriptorHeap` | `SPV_EXT_descriptor_heap`, capability `DescriptorHeapEXT` |
| Untyped pointers, which the heap form needs | `VK_KHR_shader_untyped_pointers` | `VkPhysicalDeviceShaderUntypedPointersFeaturesKHR::shaderUntypedPointers` | `SPV_KHR_untyped_pointers`, capability `UntypedPointersKHR` |
| Mutable descriptors, the default Indexed form | `VK_EXT_mutable_descriptor_type` | `VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT::mutableDescriptorType` | none |
| Runtime descriptor arrays | `VK_EXT_descriptor_indexing`, core in 1.2 | `VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray` | capability `RuntimeDescriptorArray` |
| Address commands | `VK_KHR_device_address_commands` (number 319) | — | — |

Registry facts:

- `VK_EXT_descriptor_heap` depends on (`VK_KHR_extended_flags` or `VK_KHR_maintenance5`) and buffer
  device address, or on Vulkan 1.4. It deprecates `VK_EXT_descriptor_buffer`.
- `VK_EXT_buffer_device_address` is deprecated by the KHR form. Do not name it.
- The address-command extension is `VK_KHR_device_address_commands`. No `EXT` form exists.

Driver coverage, from the gpuinfo.org extension list on 2026-09-24. These are shares of submitted
reports, not of devices in use. Treat them as a snapshot.

| Extension | Reports |
|---|---|
| `VK_KHR_buffer_device_address` | 91% (low, because a 1.2 driver can omit the extension name) |
| `VK_EXT_descriptor_indexing` | 83% |
| `VK_EXT_mutable_descriptor_type` | 36% |
| `VK_EXT_descriptor_heap` | 9% |
| `VK_KHR_device_address_commands` | 8% |

`VK_EXT_descriptor_heap` first appeared in Vulkan 1.4.340 (January 2026). NVIDIA ships it from driver
610 on RTX 30 and later. AMD ships it in Windows Adrenalin 25.30.17.02 but not on by default, and in
Mesa RADV from 26.1.

Consequences:

- **The Pointer model is broadly available.** Buffer device address is core in 1.2.
- **The descriptor heap is too new to be the only Indexed form.** Mutable descriptors reach about four
  times as many reports. Slang's default lowering targets them.
- **These names fill the capability string table** (handoff section 3, item 3). Name the feature bit a
  client checks, not only the extension. A 1.2 driver has the feature without the extension name.

Sources: `vk.xml` above; <https://github.com/KhronosGroup/Vulkan-Headers/tags>;
<https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_heap.html>;
<https://vulkan.gpuinfo.org/listextensions.php>;
<https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/>;
<https://www.phoronix.com/news/RADV-Merges-Descriptor-Heap>;
<https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-25-30-17-02-EXPANDED-VLK-SUPPORT.html>.

## 9e. Correction to §9a: link time specialization selects the access model

The decisions that follow from this section are in `docs/phase-f-plan.md` section 2.

Measured on 2026-09-24. Slang at `6eb89786c`. `spirv-val` ran with `--target-env vulkan1.2`.

**Slang sets the SPIR-V addressing model at emit time, for the linked program.**
`requirePhysicalStorageAddressing()` in `slang-emit-spirv.cpp:2151` switches the model when the emitter
writes a physical storage pointer. The switch does not look at the source module. §9a measured a module
that used its pointer, and it then assumed that a declared pointer was enough.

The probe: a shim module holds `IReadBuffer<T>` and two generic implementations.
`BoundReadBuffer<T>` holds a `StructuredBuffer<T>`. `PointerReadBuffer<T>` holds a
`Ptr<T, Access.Read>`. The shading module declares `extern struct PointBuffer : IReadBuffer<PointLight>`
and `extern struct TintBuffer : IReadBuffer<float4>`, and puts both in a `ParameterBlock`. A second
module, compiled to a `.slang-module`, supplies the `export struct` lines.

| Linked form | `OpMemoryModel` | `spirv-val` | Block layout |
|---|---|---|---|
| Bound | `Logical` | passes | `Points.Data` slot 0, `Tints.Data` slot 1, `Count` at offset 0 |
| Pointer | `PhysicalStorageBuffer64` | passes | `Points` at 0, `Tints` at 16, `Count` at 32. Each pointer is 8 bytes. |

The shim module defines the pointer type in both runs. The Bound run still gives `Logical`. The Bound
form also cooks for WGSL, and it passes Tint and the reflection cross-check.

Consequences:

- **The access model is an interface axis.** The existing `extern struct` mechanism selects it, and it
  needs no generated source. The cooker writes one `export struct` line for each access-model extern,
  as it does for an interface axis today.
- **The shading source holds no pointer.** The pointer lives only in the shim module, so the §7 rule
  holds: the author writes `extern struct X : IReadBuffer<T>` and calls `Load`.
- **One extern for each element type.** A generic associated type (`associatedtype ReadBuffer<T>`)
  does not parse (E20001), so one model type cannot map every element type. A module declares one
  extern for each buffer, and the cooker selects every one of them from the profile.
- **The wrapper costs padding in a uniform buffer.** In a `ParameterBlock` each pointer wrapper takes
  16 bytes, not 8, because a struct member in a uniform buffer aligns to 16. In push constants (std430)
  it takes 8. See below. The offset table must come from
  reflection, per target, as §5 states.
- **The Pointer form is not valid WGSL, and Slang does not report it.** Slang emits `ptr<, T>` in a
  uniform struct with no diagnostic. The policy must keep the Pointer form off `wgsl`. Tint is the net.
### Texture and sampler shims, and a mixed block

The shim module adds `ITexture2DRef` and `ISamplerRef`. Each has a Bound implementation (a
`Texture2D<float4>` or a `SamplerState`) and a Handle implementation (a `.Handle`). Each exposes
`Get()`, which returns the resource. One `Material` block holds a buffer, a texture, a sampler, and a
count. Three link modules select the forms.

| Form | Memory model | Extensions | `Material` layout |
|---|---|---|---|
| Bound | `Logical` | — | three descriptor slots, `LightCount` at 0 |
| Pointer buffer, handle texture and sampler | `PhysicalStorageBuffer64` | `SPV_KHR_physical_storage_buffer` | pointer at 0, texture handle at 16, sampler handle at 32, count at 48. A mutable heap takes its own set. |
| The same, `-capability spvDescriptorHeapEXT` | `PhysicalStorageBuffer64` | adds `SPV_KHR_untyped_pointers`, `SPV_EXT_descriptor_heap` | the same layout, and no heap set |

`spirv-val --target-env vulkan1.3` passes all three. **One source gives the heterogeneous block of
§4**: pointer fields for buffers and handle fields for textures.

### Push constants

`[vk::push_constant] ConstantBuffer<Material>` with the pointer form uses std430. Each wrapper is 8
bytes: 0, 8, 16, and the count at 24. The block is 28 bytes. With the Bound form, Slang moves the three
resources to set 0, bindings 0 to 2, and keeps only the count in the push range. `spirv-val` passes
both.

On WGSL, `[vk::push_constant]` with resources fails. Slang raises E31106 and E31107, and the cook
stops. Core WebGPU has no push constants.

**An entry point `uniform` parameter is the portable seam.** `void main(uniform Material m, ...)`
gives one `PushConstant` variable on SPIR-V. On WGSL it gives a `var<uniform> entryPointParams` plus
the resources, and Tint accepts it. The author writes no `vk::` attribute.

### Measurement notes

- **`slangc` with separate inputs doubles the global layout** when the export module imports the
  shading module. Every global moved down one set. A separate types module removes the shift. The
  cooker's interface-axis cook does not show it: `InterfaceAxisCookTest` puts `gPixels` at group 0,
  binding 0. Check it again when the shim cooks through the real path.
- **The entry point `uniform` form gave a reflection mismatch** on WGSL: the cross-check expects an
  empty name for `entryPointParams`. The cook still exited 0. Handoff section 3, item 1 holds that
  fault.

### What the interface-axis cook path needs

1. **Allow a resource member when the extern sits in a shader parameter.** `rejectResourceMembers`
   rejects every impl that holds a resource. The Phase E spike measured an extern used as a local, where
   a resource has no binding. Inside a `ParameterBlock` or a `uniform` parameter, the resource becomes
   a parameter. Keep the rejection for a technique axis.
2. **Couple the access-model externs to the profile, not to the variant key.** One interface axis for
   each extern multiplies the space (2^N forms, most of them mixed). The access model belongs to the
   profile, and each (module, profile) environment already has its own extent. So an access-model
   extern needs no key digit. The cooker writes its `export` line from the profile.
3. **Build the implementation name from the extern.** An impl is generic (`BoundReadBuffer<T>`). The
   cooker reads the type argument from the extern's interface (`IReadBuffer<PointLight>`) and writes
   `BoundReadBuffer<PointLight>`. Tagging each instantiation by hand does not scale.
4. **Import what the export line names.** `MakeExportedConstantSource` imports only the impl module.
   The export line also names the element type, so it must import the module that declares it.
5. **Reflect the pointer and handle members.** A pointer field reflects as kind `pointer`, and a handle
   reflects as a `uint2` vector. The manifest needs a placement for each: a byte offset in a block.
6. **A SPIR-V target**, because the Pointer and Handle forms exist only there.

Items 1 to 4 can be proved on WGSL now, with the Bound form alone.

---

## 10. Glossary

**Axis kind** — resource presence, capability, tuning, or technique. Section 2.

**Binding time** — cook, bind, invocation, or execution. The latest point at which an axis value must
be known. Section 3.

**Earliest sound binding time** — the earliest binding time at which an axis is correct, declared by
the author. The cooker and the client may move the value later. Section 3.

**Access model** — bound, indexed, or pointer. How a shader reaches a resource. Section 4.

**Interface contract** — what the cooker tells the client: a bind group layout, a pointer table with
offsets, a push constant range. It follows from the other three.

**Target profile** — a target, an access model, and a capability floor. It is a cooked *form*, not a
device power tier. A client maps a live adapter onto a profile, and asks the manifest for what that
profile can run. Device power varies inside one profile, and an axis carries it (section 6).

Three concepts wear two words, and they must stay separate:

- **Target profile** (run time) — the cooked form above. `(target, access model, capability floor)`.
- **Device preset** (cook policy) — a budget in the policy file, such as `minspec` or `mobile`. It
  decides which variants and how many the cook bakes for a tier.
- **Query preset** (client) — a reusable, module-agnostic bundle of query constraints. It decides which
  variants a run-time query selects.

"How powerful is the device" splits across the last two. The policy decides what is cooked. The query
decides what is picked. Neither is the profile.

**Lowering** — the pass that takes an axis with its earliest sound binding time, plus a target
profile, and decides the real binding time for that target.

**Capability atom** — a Slang term. A target, a stage, a version, or an extension. A closed set. It
answers what a target language can express, never what a device supports.
