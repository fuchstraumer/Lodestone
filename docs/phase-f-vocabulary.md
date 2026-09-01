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
3. **What can a WGSL `override` legally size?** Workgroup size, workgroup array extents? This decides
   whether bind time is a real third binding time on WebGPU, and several current cook-time axes
   may move.
4. **Confirm the exact Vulkan extension names and driver coverage** for the address-command and
   descriptor-heap features. That set moves quickly, and no phase should rest on a half-remembered
   name.

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

## 10. Glossary

**Axis kind** — resource presence, capability, tuning, or technique. Section 2.

**Binding time** — cook, bind, invocation, or execution. The latest point at which an axis value must
be known. Section 3.

**Earliest sound binding time** — the earliest binding time at which an axis is correct, declared by
the author. The cooker and the client may move the value later. Section 3.

**Access model** — bound, indexed, or pointer. How a shader reaches a resource. Section 4.

**Interface contract** — what the cooker tells the client: a bind group layout, a pointer table with
offsets, a push constant range. It follows from the other three.

**Target profile** — a named set of capabilities and one access model. A client maps a live adapter
onto a profile, and asks the manifest for what that profile can run.

**Lowering** — the pass that takes an axis with its earliest sound binding time, plus a target
profile, and decides the real binding time for that target.

**Capability atom** — a Slang term. A target, a stage, a version, or an extension. A closed set. It
answers what a target language can express, never what a device supports.
