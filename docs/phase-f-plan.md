# Phase F plan: a second target, access models, and binding times

This is the plan for phase F. `docs/phase-f-vocabulary.md` gives the words and the evidence. This file
gives the decisions, the order of the work, and the progress. Text follows ASD-STE100.

`docs/` is in `.gitignore`. Force-add this file: `git add -f docs/phase-f-plan.md`.

**This work crosses several sessions.** At the start of a session, read section 7 (progress) first.
At the end of a step, update section 7 before you do anything else.

---

## 1. Goal

One set of `.slang` modules serves WebGPU and desktop Vulkan. The author writes one form. The cooker
lowers it for each target profile. On Vulkan, a profile can reach buffers through buffer device
addresses and textures through descriptor handles. The client maps its device to a profile and asks
the manifest for what that profile can run.

## 2. Decisions in force

The author made each decision below on 2026-09-24, unless the row says otherwise.

| # | Decision | Evidence or reason |
|---|---|---|
| D1 | **WebGPU stays Bound.** No Indexed or Pointer form on `wgsl`. | Vocabulary §9b: `DescriptorHandle` gives WGSL that Tint rejects. §9e: the Pointer form gives invalid WGSL. |
| D2 | **The access model selects through link-time specialization.** No source generation. | Vocabulary §9e. The SPIR-V memory model follows the linked program. |
| D3 | **The access model lives on the profile.** It adds no axis and no key digit. Each (module, profile) environment links one form. | A profile is (target, access model, capability floor). The environment extent already exists per profile. |
| D4 | **Shim interfaces carry the access model.** The author writes `extern struct X : IReadBuffer<T>`. The interface states the element type and the access. | The author does not write a pointer. §7 of the vocabulary holds. |
| D5 | **Two tables in code own the lowering.** A shim table maps each shim interface to a resource class and to one impl for each form. A profile table gives one form for each resource class. | Only this repository writes shims, so the mapping is engineer structure, and it belongs in code, not in an attribute or in the policy file. |
| D6 | **A test proves that the shim table agrees with the shim module.** Each cell names a type that exists, takes one type parameter, and conforms to its interface. | The table names Slang types as strings. A comparison replaces trust. |
| D7 | **One form per resource class inside one profile.** A module cannot mix Bound and Pointer buffers in one profile. | Simple first. The future direction is "at least this form": a profile may lower to a form below its own when the row supports it. |
| D8 | **The Handle form uses mutable descriptors first.** The descriptor heap follows soon after as its own profile. | Vocabulary §9d: mutable descriptors in about 36% of reports, the heap in about 9%. Slang lowers to mutable descriptors by default. |
| D9 | **The lowering of binding time is per target.** One axis can bind at pipeline time on SPIR-V and at cook time on WGSL. | Vocabulary §9c: through Slang, a WGSL `override` sizes nothing. |
| D10 | **An entry point `uniform` parameter is the portable push-constant seam.** The author writes no `vk::` attribute. | Vocabulary §9e: push constants on SPIR-V, a uniform buffer on WGSL. |
| D11 | **A device-info layer comes later, above the two tables.** A client passes a device description, and the layer picks the profile and the form for each class. End users do not see the tables. | `todo.md`, "Resource Layer" (`SetDeviceLimits`). Not in this plan's slices. |

### The two tables (D5)

Shim table, one row for each shim interface:

| Interface | Class | Bound | Pointer | Handle |
|---|---|---|---|---|
| `IReadBuffer<T>` | Buffer | `BoundReadBuffer` | `PointerReadBuffer` | — |
| `IReadWriteBuffer<T>` | Buffer | `BoundReadWriteBuffer` | `PointerReadWriteBuffer` | — |
| `ITexture2DRef` | Texture | `BoundTexture2D` | — | `HandleTexture2D` |
| `ISamplerRef` | Sampler | `BoundSampler` | — | `HandleSampler` |

Profile table, one form for each class:

| Profile | Target | Buffer | Texture | Sampler |
|---|---|---|---|---|
| `wgsl` | WGSL | Bound | Bound | Bound |
| `spirv` | SPIR-V | Bound | Bound | Bound |
| `spirv-modern` | SPIR-V | Pointer | Handle (mutable) | Handle (mutable) |

The rows above are the first set, not the final set. The Pointer impl takes the access of its row:
`IReadBuffer` gives `Ptr<T, Access.Read>`, so the Pointer form cannot widen the access that the Bound
form grants.

The cooker writes one `export struct` line for each extern whose interface is in the shim table:
interface and type argument, then class, then the profile's form, then the impl. An empty cell fails
the cook, and the message names the module, the extern, and the profile.

The shim table also identifies an access-model extern. No attribute marks one.

## 3. Slices

Each slice ends green: `scripts\run-tests.bat RelWithDebInfo`, and `python scripts\check-known-good.py`
when stage output changes. Each step names the comparison that proves it.

### F0 — Correctness prerequisites (done on 2026-09-24)

A second target makes each of these faults more likely to fire. Section 7 records what each step found.

| Step | Work | Proof |
|---|---|---|
| F0.1 | Restore the failure on a nonzero `ReflectionMismatches` count. The check was lost in `df028ca`. | A module that must fail the cross-check fails the cook. |
| F0.2 | A failed `getEntryPointCode` returns a `CookError`. Today it gives an empty variant and exit 0. | A module that must fail codegen fails the cook. Use the Q3 workgroup probe (vocabulary §9c). |
| F0.3 | Remove the compiled-in path in `SlangModuleContext.cpp`. The builtins are compiled in and served by `EmbeddedFileSystem`. | A cook from another working directory succeeds. |
| F0.4 | Guard `WgslValidator` and the `wgsl` profile behind `LODESTONE_ENABLE_WGSL`. | A configure with the switch OFF builds and links. |

F0.1 and F0.2 add must-fail rows. `AccessModelRejectTest` holds that pattern: a table of modules with a
control row that must cook.

### F1 — Second target: `spirv`, Bound only

| Step | Work | Proof |
|---|---|---|
| F1.1 | Remove the WGSL assumptions in `compile/impl`: `k_WgslTargetIndex` and `target.format = SLANG_WGSL`. The profile supplies the Slang target. | WGSL output stays byte-identical. Save the KitchenSink bundle hash first. |
| F1.2 | Add the `spirv` profile row. Remove `assert(target_name == "wgsl")` in `TargetUtils.cpp`. | `--target=spirv` cooks KitchenSink. |
| F1.3 | Store the SPIR-V payload (decision O1). | The library round trip compares the bytes. |
| F1.4 | Add a SPIR-V validator (decision O2). The same walk collects each entry point's `OpCapability` list. The dedupe report prints the collated set for each environment. | It must find a planted mismatch, and pass KitchenSink. The report shows `GroupNonUniformArithmetic` for KsVolume only. |
| F1.5 | Per-target policy (decision O4). A module with no policy entry cooks with no limits. A module with an entry for another target, and none for this one, gets a warning that names the module and the target. | `PolicyDocumentTest` rows. |
| F1.6 | Cook KitchenSink for both targets in one cook. Add SPIR-V known-good dumps. | The bundle holds two real profiles. The known-good check covers both. |

### F2 — The shim module and the two tables, proved on WGSL

**F2.0, the spike (done 2026-10-22).** Handoff section 4 said an interface conformance cannot carry a resource (E56003),
and vocabulary §9e had a shim impl hold a `StructuredBuffer` and pass. Both are true. Probe: a shim module with
`IReadBuf` and impls `NullBuf` (no member), `BoundBuf` (`StructuredBuffer<float>`), and `PtrBuf` (`Ptr<float, Access.Read>`);
an app module with `extern struct Buf : IReadBuf = <default>`; a link module `export struct Buf : IReadBuf = BoundBuf`
compiled to a `.slang-module`; `slangc app.slang link.slang-module`, for `spirv` and `wgsl`.

| Use of `Buf` in the app | Result |
|---|---|
| Field of a `ParameterBlock` (default `NullBuf`, `BoundBuf`, or none) | Links on both targets. WGSL gets `@group(1) @binding(1) var<storage, read>`. The `PtrBuf` link also emits for `spirv`. |
| A global (`Buf gBuf;`) | Links. The resource takes the declaration order (`gBuf` binding 0, `Output` binding 1). |
| Entry point `uniform Buf b` | Links. WGSL names it `entryPointParams_b_data_0`. |
| Reflection (`-reflection-json`) of the linked program | Reports `buf` at descriptor slot 1. The unlinked program reports no slot. The cooker reflects the linked program, so the cross-check sees the binding. |
| Function-local (`Buf b = {}`), default `NullBuf`, linked `BoundBuf` | Unusable. slangc emits a local `var<storage, read> _S1` with no binding. The 2026-09 spike got E56003 from the cooker. |
| Function-local with a default that holds a resource | Front-end error E30513: no initializer list for the type. |

The rule: a resource-bearing impl works where the extern is a parameter type, and fails where it is a local. The default
does not decide the layout; the linked impl does. So the shim externs of F2 are parameter types, and an interface axis
(a technique, used as a local) stays behaviour only. Not measured: the Handle form (F4), a shim extern in an array, and
`ConstantBuffer<Mat>` with a shim field.

The Bound form works on WGSL, so this slice needs no SPIR-V. It can run beside F1.

| Step | Work | Proof |
|---|---|---|
| F2.1 | **Done 2026-10-22.** Add the shim module to `builtins/`, beside `LodestoneAttributes.slang`. | `ShimModuleTest` cooks `ShimDirect.slang` for both targets. |
| F2.2 | **Done 2026-10-22.** Add the shim table in `target/`. The profile form table waits for F4.1: every profile is Bound, so `TargetProfile::Access` is enough. | `ShimTableTest`, the agreement test of D6. |
| F2.3 | **Done 2026-10-22.** Recognize an extern whose interface is in the shim table. Keep it out of the axis space. | `ShimExternTest`: the axis list of a shim module is unchanged. |
| F2.4 | **Done 2026-10-22.** Allow a resource member in a shim impl, which the spike (F2.0) found works for a parameter type. Keep `rejectResourceMembers` for a technique axis. A shim extern used as a local fails the cook, but through Slang and not through a check of ours. | The technique-axis rejection test still fails the cook. |
| F2.5 | **Done 2026-10-22.** Build the impl name from the extern (`IReadBuffer<PointLight>` gives `BoundReadBuffer<PointLight>`). Import the module that declares the element type. | `FindDeclaringModule` gives the import. `todo.md` holds the related import item. |
| F2.6 | **Done 2026-10-22.** A cook test asset: a material that uses buffer, texture, and sampler shims in a `ParameterBlock` and in an entry point `uniform` parameter. | The cross-check agrees. Check the binding numbers: no global may move down a set (vocabulary §9e, measurement notes). |
| F2.7 | **Done by rendergraph task L14 (2026-10-19):** the container is the `entryPointParams` binding on WGSL and entry point constants on SPIR-V, and the mismatch row is now `EntryPointUnusedResource.slang`. Fix the cross-check for an entry point `uniform` container. Today it expects an empty name for `entryPointParams`. | F2.6 passes with F0.1 in place. `AccessModelRejectTest` uses `EntryPointResourceStruct.slang` as its mismatch row, so replace that row with another mismatch in the same step. |

### F3 — The capability set and the device limits

Revised on 2026-10-21 with the rendergraph tasks L5 and L6. The author decided the points marked (A).

**What the cook stores, for each entry point instance:**

- **The capability names that the code needs, in the names of its target** (A). `spirv`: each `OpCapability` and
  `OpExtension` name. `wgsl`: each `enable` directive (Tint `Inspector::GetUsedExtensionNames`). Each environment
  holds a table of these names, and each entry point instance holds a bitmask over it, in the word layout of the
  axis masks. A name that the profile guarantees is not stored: for `spirv`, a capability that a Vulkan version at
  or below 1.2 enables alone (`Shader`). The grain is the entry point instance, not the variant: KsVolume `BlurCS`
  needs no wave operation, also in a wave variant.
- **The workgroup memory bytes.** Each workgroup variable counts `RoundUp(16, RoundUp(align, size))`, with the
  std430 size and alignment, and the sum is stored. This is the rule of Tint (`core::ir::GetWorkgroupInfo`, which
  Dawn checks against `maxComputeWorkgroupStorageSize`), and it is an upper bound for the Vulkan rule (Vulkan-Docs
  `shaders.adoc`, `[[workgroup-padding]]`: implementation-dependent, but at most std430 in the worst order). So
  one number serves both targets, and the two profiles of one variant must agree on it.
- `Variant::CapabilityRequirement` is removed (A). `Profile::CapabilityFloor` is removed: the profile names its
  floor, and the cook drops the guaranteed names.

**Why names and not Vulkan feature bits.** The design note of 2026-09-25 said to store Vulkan feature names. The
`<spirvcapabilities>` section of `vk.xml` shows that a capability needs **any one** of its `<enable>` entries: a
feature (`struct` and `feature`), an extension, a property bit (`property`, `member`, `value`), or a version.
`Float16` is `VkPhysicalDeviceVulkan12Features::shaderFloat16` or `VK_AMD_gpu_shader_half_float`. A bitmask of
feature names cannot hold an "or". The SPIR-V name is also what the reader measures, so the stored value is a
measurement and not a translation.

**The Vulkan table** (A: a checked-in file and its generator script). `scripts/generate-vulkan-capability-table.py`
reads the `<spirvcapabilities>` and `<spirvextensions>` sections of `vk.xml` and writes
`client/include/VulkanCapabilityTable.hpp`: for each SPIR-V name, its `<enable>` alternatives as strings. It names
no Vulkan type, so the client needs no Vulkan header. The cook reads it to drop the guaranteed names. A renderer
reads it to test a device and to list what to enable. DiamondDogs' `ExtensionWrangler` works in extension names and
feature structs, which are the same terms, so it resolves the dependencies and Lodestone does not. The WebGPU table
is small and written by hand: `f16` is `shader-f16`, `subgroups` is `subgroups`.

**The device filter (F3.4).** The client takes a device description in two halves:

- Features. The renderer gives a predicate over the capability names of one environment, and the client builds the
  device mask once. The Vulkan and WebGPU tables help a renderer write the predicate. The filter itself names no API.
- Limits, each compared less-or-equal for each entry point instance: workgroup memory bytes, invocations (the
  product of the workgroup size), the size on each side, push constant bytes. Bindings per stage and the bind group
  count come from the merged layout of a pipeline.

The query removes each (variant, entry point) instance that the device cannot run. It is the base filter, and every
other query builds on it.

**L6 is closed without an axis bound to a device property** (A). A shader author does not always know which axis
matches which device value. The renderer knows, and it already has the tool: a query preset (L4), such as a pin of
`IFFT_WAVE_SIZE` to the subgroup size. A required subgroup size is a pipeline creation fact
(`VK_EXT_subgroup_size_control`), so no cook can measure it. Revisit with specialization constants.

| Step | Work | Proof |
|---|---|---|
| F3.1 | The Vulkan table and its generator. The capability names of each entry point instance, from each target's reader. The workgroup memory bytes, from each target's reader. Format 14: the name table, the masks, and the byte count. | `ShaderManifestRejectTest` reads them back, and rejects a mask bit past the name table. `SpvValidatorTest` and `WgslValidatorTest` read the names and the bytes of fixed code. |
| F3.2 | Drop the names that the profile guarantees. | The `spirv` cook of KitchenSink stores no `Shader`. |
| F3.3 | **Done 2026-10-22.** Validators: the workgroup memory bytes and the portable capabilities (16-bit floats, subgroups) agree across the profiles of one variant. | `WorkgroupMemoryCookTest` cooks a probe module with `float3`, struct, `float`, and `half4` workgroup arrays on both targets (144 bytes each). `ShaderManifestRejectTest` rows for each mismatch. |
| F3.4 | **a to c done 2026-10-22, d deferred (O10).** The device filter. | `ManifestIndexTest` rows: a device without subgroups gets no wave instance of `GenerateCS`, and still gets `BlurCS`. A device below the workgroup memory or invocation limit loses only the instances above it. |
| F3.5 | **Done 2026-10-22.** Cross-check each axis of kind `capability` against the measured names. The shader declares nothing beyond the kind (O12 closed). | `CapabilityAxisTest`: a must-fail module whose "off" path still calls a wave operation, on both targets. |

Measured on 2026-10-21, DiamondDogs OceanFft, `spirv`, 62 unique modules:

- `IfftRadix2CS` and `IfftRadix4CS` run `IFFT_SIZE` threads and use `16 * IFFT_SIZE` bytes of workgroup memory
  (`half4[IFFT_SIZE]` twice). At 1024 that is 1024 invocations and 16384 bytes: the Vulkan minimum of
  `maxComputeSharedMemorySize`, the WebGPU default of `maxComputeWorkgroupStorageSize`, and four times the WebGPU
  default of `maxComputeInvocationsPerWorkgroup` (256). `OceanMipsCS` uses 4096 bytes.
- Every IFFT module needs `Float16` and `UniformAndStorageBuffer16BitAccess`. The wave variants add
  `GroupNonUniform` and `GroupNonUniformShuffle`. `IFFT_WAVE_SIZE` leaves no trace in the module.
- The `wgsl` cook fails with E56106: `rgba16float` has no `read_write` storage access in WGSL. That is a target
  language limit, so it is Slang's to reject.
- KsVolume `float g_tile[64]` is 256 bytes on both targets.

### F4 — `spirv-modern`: Pointer buffers and Handle textures

| Step | Work | Proof |
|---|---|---|
| F4.1 | Add the Pointer and Handle impls to the shim module, and the `spirv-modern` profile row. | `spirv-val` passes. The D6 agreement test covers the new cells. |
| F4.2 | Reflect a pointer member (kind `pointer`) and a handle member (a `uint2` vector) as a placement: a byte offset in a block. | The layout round trip covers the new placement. |
| F4.3 | Add the placement to the manifest, and to `ResolvedResource`. | The manifest round trip, and `ManifestIndexTest`. |
| F4.4 | Teach the SPIR-V validator the heap set. Reflection gives it through `getBindlessSpaceIndex()`. | A cook of the F2.6 asset for `spirv-modern`. |
| F4.5 | Record the offset table for each profile. Vocabulary §5 asks for this. A wrapper takes 16 bytes in a uniform buffer and 8 in push constants. | The offsets in the manifest equal the offsets in the SPIR-V `Offset` decorations. |

### F5 — Binding-time lowering

| Step | Work | Proof |
|---|---|---|
| F5.1 | Fill the specialization-constant table from reflection. | The manifest dump shows it. |
| F5.2 | The policy states, per target, which axes move from cook time to bind time. The lowering refuses a move the target cannot express (a WGSL size). | A must-fail row for a WGSL workgroup size. |
| F5.3 | A moved axis leaves the variant key and enters the specialization table. | The variant count drops by the axis radix. The cross-check agrees. |

### F6 — `spirv-modern-heap` (early adopter, D8)

A second Handle form on `VK_EXT_descriptor_heap`. A new profile row, and the Slang capability
`spvDescriptorHeapEXT`. Plan it in detail when F4 is done.

### Not in phase F

- The device-info layer (D11).
- Portable geometry. `todo.md` ("Phase F: portable geometry") holds the design.

## 4. Open decisions

Each is the author's. Ask before the step that needs it. An open row holds a recommendation. A decided
row holds the decision and its date.

| # | Needed by | Question | Decision or recommendation |
|---|---|---|---|
| O1 | F1.3 | How is the SPIR-V payload stored? | **Decided 2026-09-25.** Binary words, aligned to 4 bytes. `--dump-sources` writes `.spv`. The validator takes bytes. The client gets a byte accessor beside `Source()`. The two renderers fork, so a function for each language is correct. |
| O2 | F1.4 | What is the SPIR-V second opinion? | **Decided 2026-09-25.** SPIRV-Tools `spvBinaryParse` reads `DescriptorSet`, `Binding`, and variable types. `spirv-val` checks legality. A facade in `target/`. Keep the walk linear, as the WGSL validator does. The first validator was O(n^2). |
| O3 | F1.1 | One Slang session with two targets, or one session for each profile? | **Decided 2026-09-25.** One session for each profile. Shared work needs the same access model and capability floor on two targets. That case is rare: a build seldom holds two languages. |
| O4 | F1.5 | A module has no policy section for a requested target. | **Decided 2026-09-25, revised the same day.** The policy is opt-in. An absent section means no limits. A module with a section for another target, and none for this one, gets a warning, not a failure. Inheritance from another section is in `todo.md`. |
| O5 | F1.2 | Which Vulkan environment does the `spirv` profile target? | **Decided 2026-09-25.** Vulkan 1.2 (SPIR-V 1.5) for now. At the end of phase F, measure the device coverage and decide the range again, from 1.0 up to 1.4. |
| O6 | F3.3 | Where does a variant's capability requirement come from? | **Decided 2026-10-21.** The emitted code of each target. Slang's `IMetadata` has no capability query (`isParameterLocationUsed` and `getDebugBuildIdentifier` only). |
| O7 | F1.2 | An entry point needs a capability above the profile floor. Slang raises E41012, and our warnings-as-errors fails the module. | **Decided 2026-09-25.** E41012 is off for every target. A device capability is ours to record, not Slang's to reject. F3.3 and F3.4 must record it: until then a cook states no requirement. |

## 5. Upstream Slang issues to file

Write each in the format of the prelink issue (handoff section 5). Ask the author first.

1. E55205: a specialization constant in `[numthreads]` fails for WGSL. WGSL accepts an override there.
2. A specialization constant as a `groupshared` array extent crashes the WGSL emitter (E99999).
3. `DescriptorHandle` gives invalid WGSL, and the capability atom `descriptor_handle` lists `wgsl`.
4. The Pointer form gives `ptr<, T>` in WGSL with no diagnostic.

## 6. Evidence

| Topic | Where |
|---|---|
| Pointer declaration, the first measurement | vocabulary §9a (corrected by §9e) |
| `DescriptorHandle` reach | vocabulary §9b |
| WGSL `override` sizing | vocabulary §9c |
| Vulkan names, feature bits, coverage | vocabulary §9d |
| Link-time selection, shims, push constants, the cook-path gaps | vocabulary §9e |

The probe modules lived in the session scratchpad and are gone. §9b, §9c, and §9e describe each probe
well enough to write it again.

## 7. Progress

Update this section at the end of each step. One line for each step: the date, the step, and the
commit.

- 2026-09-24. Vocabulary §9 answered (Q2 to Q4), §9a corrected by §9e. Decisions D1 to D11 made. No
  code change yet. Next: F0.1.
- 2026-09-24. **F0 done**, not committed. The KitchenSink bundle is byte-identical to the baseline
  (`2dfbe8ce...`). 22 of 22 tests, 30 of 30 known-good dumps.
  - F0.1: the driver fails the cook on a nonzero mismatch count again. Must-fail row:
    `EntryPointResourceStruct.slang`.
  - F0.2: codegen fails on a failed call, empty text, or any failure record. Slang returned success with
    E55205 and emitted `@workgroup_size(1, 1, 1)`. `ParseSlangDiagnostics` returns the failure count.
    Must-fail row: `SpecConstantWorkgroupSize.slang`. `ThreadPool` no longer replaces every compile
    error with `VariantModuleCreationFailed`.
  - F0.3: `builtins/*.slang` are compiled in (`cmake/EmbedBuiltins.cmake`). `EmbeddedFileSystem`
    implements `ISlangFileSystemExt` and serves them under `lodestone-builtin`. The module now declares
    `module LodestoneAttributes`. `loadModuleFromSourceString` was tried first and broke every worker
    session (handoff section 5).
  - F0.4: `LODESTONE_ENABLE_WGSL=OFF` builds and links (`build/ninja-msvc-nowgsl`). The `wgsl` profile
    then has no validator, and a cook fails with `TargetValidatorUnavailable` unless it passes
    `--no-validate`.
  - Found on the way: `run-tests.bat` reported a crash as a pass (`if errorlevel 1` misses a negative
    code). Every `CookError` from 128 up printed as an empty name (magic_enum range). Both fixed.
  - Next: F1.1.
- 2026-09-25. O1 to O5 decided (section 4).
- 2026-09-25. **F1.1 done**, not committed. The profile row supplies `TargetLanguage` and the Slang
  profile name. `SlangCompilerCreateInfo` carries both. `k_WgslTargetIndex` is `k_TargetIndex`: one
  session holds one target. The KitchenSink bundle and the `--with-sources` JSON are byte-identical to
  the baseline (`2dfbe8ce...`). 22 of 22 tests, 30 of 30 known-good dumps.
  - The WGSL row sets no Slang profile. Slang has no WGSL profile, and it ignores a profile that does not
    imply the target (`TargetRequest::getTargetCaps`). The old `spirv_1_4` changed no byte.
  - An unknown Slang profile name fails the session with `SessionCreationFailed`.
  - Next: F1.2.
- 2026-09-25. **F1.2 in progress**, not committed. The `spirv` row exists (`spirv_1_5`, no validator).
  `MakeScopedName` lost its target parameter and its assert. The console prints "KiB of shader code".
  WGSL output is unchanged (`2dfbe8ce...`). 22 of 22 tests.
  - Was blocked on O7, now decided. `--target=spirv` fails KsVolume: `GenerateCS` uses `WaveActiveSum` behind the
    capability axis `USE_WAVE_OPS`, and Slang raises E41012 (profile implicitly upgraded). Slang raises it
    only when a profile is set, and it checks the entry point before specialization. So it condemns every
    variant, also the variants with `USE_WAVE_OPS=false`.
  - Probe with E41012 disabled: all 616 variants cook, deterministic, all round trips pass.
    `GroupNonUniformArithmetic` is in `GenerateCS` of the 216 wave variants only. `spirv-val
    --target-env vulkan1.2` passes all 147 unique blobs. Other capabilities seen: `Shader`,
    `StorageImageReadWithoutFormat`, `StorageImageWriteWithoutFormat`. The probe change is reverted.
  - Fixed on the way (handoff section 3, item 6): `--dump-sources` wrote nothing without
    `--dump-stage`, because its block sat inside the Cooked dump branch. The folder is now
    `<module>_<target>_sources`. The dumper still writes `.wgsl` and a text comment header, so a SPIR-V
    dump is not valid SPIR-V until F1.3.
- 2026-09-25. **F1.2 done**, not committed. E41012 is off (O7). `--target=spirv --no-validate` cooks
  KitchenSink: 616 variants, deterministic, all round trips pass. WGSL unchanged (`2dfbe8ce...`). 22 of 22
  tests, 30 of 30 known-good dumps. Next: F1.3.
- 2026-09-25. **F1.3 done**, not committed. Manifest format version 7.
  - `ShaderCodeFormat` (`Wgsl`, `Spirv`) sits in the old `Profile::Reserved0` byte. The record size
    did not change. `Open` rejects an unknown format, and a SPIR-V source that is not whole words.
  - Client: `SpirvWords()` returns `std::span<const uint32_t>` on `EnvironmentView`,
    `EntryPointInstanceView`, and `ShaderSourceProvider`. `Source()` and `SpirvWords()` each assert the
    format. The cooker keeps `std::string` as the byte container (the author's decision).
  - Codegen fails a SPIR-V payload that is not whole words or has no magic number. The sources are packed
    with no padding, because SPIR-V is always whole words.
  - The round trip reads SPIR-V back through `SpirvWords()`. `--dump-sources` writes `.spv` with no
    header. `manifest_dump` prints `codeFormat`, and `spirvBytes` in place of a SPIR-V source.
  - Fixed: `DumpShaderSources` created its folder from `sink.Describe()`, so `--verify-deterministic`
    left `<output>_first` and `<output>_second` on disk, and the real write then failed. The file sink now
    creates the parent folder of each artifact.
  - Proof: KitchenSink for `spirv` is deterministic, and all 325 unique `.spv` files pass
    `spirv-val --target-env vulkan1.2`. The WGSL bundle differs from `2dfbe8ce...` in two bytes only (the
    version and the format byte). New WGSL baseline: `ae72f733...`. 22 of 22 tests, 30 of 30 known-good
    dumps. Debug builds. `ShaderManifestRejectTest` and `ManifestIndexTest` pass in Debug.
  - Next: F1.4.
- 2026-09-25. **F1.4 is the author's to write.** `CMakeLists.txt` links `SPIRV-Tools-static` and
  `SPIRV-Headers::SPIRV-Headers` to `lodestone`, with no switch. `docs/phase-f-f14-guide.md` holds the
  guide: three decisions (scope separator, byte parameter, capability path), the API calls, and the
  data-format facts, each checked with a scratch probe on KitchenSink SPIR-V.
- 2026-09-28. **F1.4 done**, not committed. The author wrote the parse (`src/target/SpvValidator.cpp`),
  and the agent finished it on her request. The first half is staged, the rest is not.
  - The parse: one flat record for each id, sized from the header's id bound. It stops at the first
    `OpFunction`. Member names and decorations are deferred and applied after the parse, because they
    come before `OpTypeStruct`. Constants and spec constants record their raw bits. `OpSpecConstantOp`
    is marked derived and not evaluated.
  - The check: `spvValidateBinary` (Vulkan 1.2 rules) first, then the parse, then a merge walk against
    reflection on (set, binding). It compares the kind always, the shape of an image, the access of a
    storage buffer or storage image, and the last segment of the name (the scope separator is in
    `todo.md`). A spirv-val rejection is `TargetValidationEntryPointInvalid` (302).
  - Capabilities: `BindingComparison::Capabilities`, merged into `InternedModule::ObservedCapabilities`,
    frozen onto `CookedModule`, and printed in the dedupe report for each environment. `--no-validate`
    gives none.
  - Proof: `SpvValidatorTest` (15 checks). A validated `--target=spirv` cook of KitchenSink,
    `EntryPointParams`, `ParameterBlocks`, `InterfaceAxisTest`, and `EnumAxisTest` finds no mismatch. Both
    bundles are byte-identical (`ae72f733...`, `e060e442...`). 23 of 23 tests, 30 of 30 known-good dumps.
    Debug builds.
  - Found by the report: KsGeometry needs `Float16` and `StorageInputOutput16` (16-bit varyings), and
    KsPost needs `DrawParameters`. Both are device features, not Vulkan 1.2 guarantees.
  - Deferred (they need the `CompiledEntryPoint` in the validator signature): the LocalSize check, the
    member layout check (F4.5), vertex inputs and color targets, and the vertex-to-fragment link check.
  - Next: F1.5.
- 2026-09-30. **F1.5 done.** The author wrote the code (`42cd23d`). The agent wrote the tests and the docs,
  not committed.
  - `PolicyDocument::FindTargetCoverage` gives `NoModuleEntry`, `NoTargetSections`, `Present`, or
    `OtherTargetsOnly`. `PreparePermutationSpaceStep` warns on `OtherTargetsOnly`, and the cook continues
    with no limit (decision O4).
  - `PolicyDocument::ValidateTargetNames` rejects a target key the build does not know, with
    `PolicyUnknownTargetName`. `PrepareCookStep` calls it after the load.
  - Proof: `PolicyDocumentTest` (36 checks, 10 new). KitchenSink for both targets gives no warning, with
    both hashes unchanged. A copy with only `[KsMaterial.targets.wgsl]` gives one warning on `spirv`, none
    on `wgsl`. A `targets.spriv` section fails the load. 23 of 23 tests, 30 of 30 known-good dumps.
  - Open, small: `ValidateTargetNames` does not yet name the module or suggest the nearest name.
  - Next: F1.6.
- 2026-10-01. **F1.6 done. F1 is closed.** The agent did the step, not committed.
  - One cook of KitchenSink with `--target=wgsl --target=spirv` writes a bundle with two profiles (`Wgsl`
    and `Spirv`, both `Bound`). `manifest_dump` shows that each environment, with its sources, is equal to
    the environment of the single-target cook. A fresh cache, a warm shared cache, and
    `--verify-deterministic` give the same bundle (`c690979d...`). One cache directory for two targets is
    safe.
  - Cost (decision O3), RelWithDebInfo, warm caches, three runs: one two-target cook takes 8.6 to 9.0 s,
    and two single cooks take 8.7 to 9.4 s. The difference is about one process start. One session for
    each profile costs nothing that we can see.
  - All five cook tests pass `--target=spirv` too. `KitchenSinkCookTest` takes about 18 s in
    RelWithDebInfo and about 19 minutes in Debug.
  - `check-known-good.py` cooks each module once for each target. 30 new `<module>_spirv_<Stage>.json`
    dumps. Before the accept, each one was compared with its WGSL partner. Only the code length and the
    code hash differ. The reflection is equal for every variant, and each module dedups to the same count
    of unique sources on both targets.
  - Proof: 23 of 23 tests (RelWithDebInfo), 60 of 60 known-good dumps. Both single-target hashes are
    unchanged (`ae72f733...`, `e060e442...`).
  - Next: F2.
- 2026-10-12 to 2026-10-14. **Phase F paused.** The author moves to the VeloxRhi rendergraph first,
  because the graph will shape the client API and parts of F2 to F5. `docs/rendergraph-integration-tasks.md`
  holds that work. Its section 5 lists the tasks that touch phase F.
  - Done in the pause: the merged layout of one pipeline (`MergedLayoutView`, `01dc278`). The cooker now
    sorts each visibility list by placement, and `EnvironmentView::Open` checks the order
    (`InvalidVisibilityListOrder`) and that each entry point stage is one bit. `MergedLayoutTest` (21
    checks). No known-good dump changed.
  - The merge rejects a placement that is not Bound. F4.2 and F4.3 must add a reference from a pointer
    or a handle to its containing record, because `PlacementPayload` gives only a byte offset.
  - Measured: `KitchenSinkCookTest` crashed on 2026-10-12 from commit exhaustion on a PC with no page
    file, not from a code fault. `todo.md` holds the thread-count item.
  - Next, when phase F resumes: F2, with the rendergraph lessons applied.
- 2026-10-21. **F3 resumes with the rendergraph tasks L5 and L6.** F3 rewritten (section 3): names in the target's
  terms, a generated Vulkan table, workgroup memory in F3, L6 closed. O6 decided. OceanFft measured. Next: F3.1.
- 2026-10-21. **F3.1 and F3.2 done** (committed as 900881e). Format 14. 30 of 30 tests, 60 of 60 known-good dumps (8 Cooked
  dumps gained `workgroupMemoryBytes` and `capabilities` on each source, additions only).
  - The facts belong to the source, not the variant: the code alone decides them. `SourceRef::WorkgroupMemoryBytes`,
    `EnvironmentHeader::CapabilityNames` and `SourceCapabilityTableOffset`. Reserved: `Profile::CapabilityFloor`,
    `Variant::CapabilityRequirement`. Round trip `CheckSourceFacts` (`ManifestSourceFactsMismatch` = 116). `Open`
    rejects a name past the string table (`InvalidCapabilityNames` = 60) and a mask bit past the names
    (`InvalidSourceCapabilities` = 61).
  - Measured: the WGSL reader (Tint `core::ir::GetWorkgroupInfo`, Dawn's own value) and the SPIR-V reader (std430,
    written here) give 144 bytes for the same four variables: a `vec3` array, a struct of a float and a `vec3`, a
    float array, and a `half4` array. So the padding agrees on those types, and F3.3 can compare equality.
  - Found: SPIRV-Headers `CapabilityToString` returned the alias `StorageUniform16` for OceanFft's
    `UniformAndStorageBuffer16BitAccess`, which `vk.xml` does not list. The generator reads the 54 aliases from
    `spirv.core.grammar.json`, and the reader stores the primary name.
  - KitchenSink: KsVolume `subgroups` / `GroupNonUniformArithmetic`, KsGeometry `f16` / `Float16` and
    `StorageInputOutput16`, KsPost `DrawParameters` (SPIR-V only). So the F3.3 cross-check compares a portable subset
    through a map, not the whole name list.
  - Next: F3.3 (cross-profile validators), then F3.4 (the device filter).
- 2026-10-22. **F3.3 done**, not committed. `VerifySourceFactsAcrossProfiles` (`model/CookedLibrary.cpp`, error
  `SourceFactsTargetMismatch` = 53) beside `VerifySamplerPairsAcrossProfiles`. Both now use `VisitSharedVariants`, which
  walks the variants that the first reading profile and a later one both cooked. The portable map is `f16` with
  `Float16`, and `subgroups` with any `GroupNonUniform*`. `CookedProfile::CodeFactsRead` marks a profile whose reader
  ran, because a profile with no reader stores empty facts. 31 of 31 tests. KitchenSink agrees on both targets (256 bytes
  in KsVolume, `f16` in KsGeometry, `subgroups` in KsVolume).
  - Found: nothing in the manifest says that a profile read its facts. F3.4 must close that, or a profile with no reader
    would read as "needs nothing" and pass every device filter.
  - **F3.4 proposal decided the same day** (below, O8 to O11). Next: F3.4a to c, done below.

#### F3.4 design (decided and built 2026-10-22)

The consumers are DiamondDogs OceanFft (Vulkan) and VeloxRhi (WebGPU). Both give the client a device description once,
then query. The shape:

1. **The facts flag (format 15).** `ProfileFlags::CodeFactsRead`, set from `profile.Validator != nullptr`.
   `EnvironmentView::HasCodeFacts()`. A device query on an environment without it fails with `DeviceFactsUnavailable`.
   Nothing silent.
2. **The fit of one instance.** `EntryPointInstanceView::Fit(const DeviceLimits&, device mask)` returns a `DeviceFit`:
   fits, or the first reason (missing capability name, workgroup memory, invocations, size on one side, push constant
   bytes, storage buffers per stage, bind groups). The reasons use the stored facts and the layout.
3. **The device in the builder.** `ManifestQueryBuilder::ForDevice(predicate, limits)`. The predicate takes a capability
   name and returns whether the device supports it. The builder builds the mask from its own environment at once, so a
   mask for the wrong environment cannot exist. `DeviceLimits` is an aggregate with no default values: a limit left at 0
   rejects everything, which is loud.
4. **Grain.** `SingleFor(entry_points)` keeps a variant when every named instance fits. `Keys`, `Variants`, and `Single`
   keep a variant when every entry point of the module fits.
5. **The answer to "why empty".** `Notes()` gains a `DeviceRemovedVariants` note with the count and the first reason for
   each reason. `Single` with no match returns `NoVariantFitsDevice` with the nearest reason.

Slices: F3.4a (the flag, format 15), F3.4b (`Fit` for capabilities and the instance limits), F3.4c (the builder and the
notes), F3.4d (the layout limits: bindings of each kind per stage, and bind groups).

| ID | Question | Recommendation |
|---|---|---|
| O8 | Does `Keys` with a device mean every entry point of the module, or must the caller name them? | **Decided 2026-10-22: as recommended.** Every entry point for `Keys`, `Variants`, and `Single`. `SingleFor` names them. A caller that wants fewer uses `SingleFor`. |
| O9 | Is a new flag worth a format bump (15)? | **Decided 2026-10-22: yes**, and bump often, since DiamondDogs builds manifests for its tests. Yes. The alternative is a device filter that cannot tell a profile with no reader from a shader that needs nothing. |
| O10 | Do the layout limits go in F3.4, or wait for a consumer? | **Decided 2026-10-22: wait.** DiamondDogs could name limits at any time, but bound samplers will not reach them at the current scope. F3.4d waits until VeloxRhi names the limits it checks. WebGPU rejects a pipeline layout above `maxStorageBuffersPerShaderStage`, so it will. The instance-level counts are enough: each instance is one stage. |
| O11 | Does the client give a Vulkan helper that tests the alternatives of `VulkanCapabilityTable.hpp` against a renderer predicate? | **Decided 2026-10-22: no helper, and the name predicate is a C function pointer** with a user pointer, because a lambda or a template across the library boundary is brittle. No. `ExtensionWrangler` already resolves features and extensions. The renderer writes the name predicate. Revisit if a second Vulkan consumer appears. |
- 2026-10-22. **F3.4a to c done**, not committed. Format 15: `ProfileFlags::CodeFactsRead`, `EnvironmentView::HasCodeFacts`.
  `ShaderManifestDevice.hpp` (`CapabilitySupportFn`, `DeviceLimits`, `DeviceMismatch`, `DeviceFit`),
  `EntryPointInstanceView::Fit`, `EnvironmentView::MakeDeviceCapabilities`, `ManifestQueryBuilder::ForDevice`, and the query
  errors `DeviceFactsUnavailable` and `NoVariantFitsDevice`. Proof: `CheckDeviceFilter` in `ManifestIndexTest`, and the
  flag rows in `ShaderManifestRejectTest`. 31 of 31 tests.
  - Not built, on purpose: a `DeviceRemovedVariants` note with counts (no consumer yet; the first rejection reason is in
    `NoVariantFitsDevice`), and F3.4d.
  - Next: F3.5 (the capability-axis check), or F3.4d when VeloxRhi names the limits it checks.
- 2026-10-22. **DiamondDogs triage, between F3.4 and F3.5.** Done outside the slices: C13 and C8 (`--include-dir`), C7
  (`UniformMemberRange::Find`), C9 (diagnostic text), and C12 (fixed-size binding arrays on SPIR-V, with the
  `...ArrayDynamicIndexing` capabilities that Slang does not state). 33 of 33 tests, 60 of 60 known-good dumps. Placed in
  the phase: **C11** (`ls_format` per variant) goes with **F2**, because the format must reach the emitted image type, which
  is a per-variant type choice that link-time specialization (the shim) provides. **Unbounded binding arrays** and
  WebGPU's `binding_array` go with **F4**. **C6** (generated host layout headers) waits until phase F is complete.
  Next: F3.5.
- 2026-10-22. **F3.5 done**, not committed. `VerifyCapabilityAxes` (`model/AxisInfluence.cpp`, run by the driver beside
  `VerifySourceFactsAcrossProfiles`), errors `CapabilityAxisInert` = 54 and `CapabilityAxisInverted` = 55. 34 of 34 tests.
  - **Design changed in review.** The first proposal added an attribute that names the capabilities of an axis
    (`ls_axis_capability`). The author refused: naming capabilities in shader source is renderer knowledge, and she had
    refused the same idea for L6. The check uses the kind alone. A measurement cannot say what an axis should turn on, but
    it can say that an axis of kind `capability` turns on nothing, and that an off path needs more than an on path.
  - Rule: for an ungated axis of kind `capability`, some one-axis pair must differ in the names of some entry point, and a
    boolean axis must have off names that are a subset of its on names. A gated axis (`IFFT_WAVE_SIZE`) is not checked: it
    refines its parent and changes no capability. The pair scan is shared with the axis influence (`ForEachOneAxisGroup`).
  - Known gap: an axis that controls two capabilities and leaves one in the off path passes. Only a declaration could
    catch it.
  - Next: F2.0, a spike on E56003 against the vocabulary §9e probe, then F2.
- 2026-10-22. **F2.0 done** (a spike, no code). See the F2 section for the table. A resource impl links as a parameter type
  on both targets, and fails as a local. F2.4 stands, with one added rule: reject a shim extern that the shader uses as a
  local. Next: F2.1.
- 2026-10-22. **F2.1 done**, not committed. `builtins/LodestoneShims.slang`: `IReadBuffer<T>` (`Load`), `IReadWriteBuffer<T>`
  (`Load`, `Store`), `ITexture2DRef` and `ISamplerRef` (`Get`), and the four Bound implementations (O17 as recommended).
  35 of 35 tests. No known-good dump can change: no existing module imports the new builtin.
  - Found: the embedded builtin list in `cmake/LodestoneCooker.cmake` was one file, not the glob the comment and CLAUDE.md
    described, so a new file in `builtins/` was never embedded. It is now `file(GLOB ... CONFIGURE_DEPENDS)`.
  - Measured for O16: a shim resource is identified by a scope and a name. The scope is the path to the shim
    (`material_Points`, `entryPointParams_Scratch`) and the name is the member the shim holds (`data`, `texture`,
    `sampler`). Two shims of one block therefore do not collide, and no renaming is needed. Both targets agree, and the
    block's ordinary data is the uniform buffer `material` at binding 0 with the resources after it. Recommendation stands:
    no renaming in F2.
  - Next: F2.2 (the shim table and its agreement test).
- 2026-10-22. **F2.2 done**, not committed. `target/ShimTable.hpp` and `.cpp`: `ShimInterface` (name, resource class,
  type parameter count, and one implementation name for each of Bound, Indexed, Pointer; an empty name is an empty cell),
  `GetShimInterfaces`, `FindShimInterface`, `ShimImplName`. 36 of 36 tests.
  - `ShimTableTest` (D6) generates a module from a table: for each non-empty cell, a struct constrained to the cell's
    interface and an alias that instantiates it with the cell's type. Slang checks the constraint at load, so the module
    cooks only when the type exists, takes the arity, and conforms. Three modified tables fail, each with its own Slang
    error (E30015 undefined identifier, E38029 type argument does not conform, E30401 and E30408 for a wrong arity).
  - Next: F2.3 (recognize an extern whose interface is in the table).
- 2026-10-22. **F2.3 done**, not committed. `SymbolTable::ExternStructsForModule` reads each `extern struct <Name> : <Interface>
  <<Args>>` header from the source text (one line, with or without a default, a line comment ignored).
  `SlangCompiler::PrepareRawModule` keeps those whose interface `FindShimInterface` knows as `RawModule::ShimExterns`
  (`RawShimExtern`: name, interface, type arguments, resource class, file, line), and confirms each against
  `SlangModuleContext::BuildModuleScopeStructNames` (reflection). A shim extern that reflection does not find fails with
  `ShimExternNotReflected` = 56. 37 of 37 tests.
  - The attribute-free design (O18) needed no change to the axis code: `stageInterfaceStruct` already skips a struct with
    neither `ls_axis_interface` nor `ls_axis_interface_impl`.
  - Limits, on purpose: the declaration is on one line, and the module scope walk does not enter a namespace, so a shim
    extern in a namespace fails with the new error. A shim interface that also carries `ls_axis_interface` is not checked
    yet.
  - Next: F2.5 (link the implementation).
- 2026-10-22. **F2.4 to F2.6 done**, not committed. 39 of 39 tests, 60 of 60 known-good dumps.
  - `compile/ShimLinkSource` (`MakeShimLinkSource`, `SplitShimTypeArguments`): the link module text, from the externs, the
    profile's access model (`AccessModelFromPlacementKind` is new), and the declaring module of each type argument. New errors
    `ShimFormUnavailable` = 57 (empty cell) and `ShimExternInvalid` = 58. `PrepareRawModule` resolves each type argument
    with `SlangModuleContext::FindTypeDeclaringModule` and keeps the source in `SlangCompiler`. `Compile` passes it to the
    thread pool, each worker context loads it once (`LoadShimLinkModule`), and `LinkVariant` adds it to the components.
    Nothing is mutated between batches.
  - **Measured (F2.6):** the shim material and its hand-written control give identical resources at identical places for
    both targets (the Bound form of §9e, now through the cooker). One cook of two targets takes about 800 ms for the shim
    and about 770 ms for the control, so the extra module and the extra code cost about 4 percent on a one-variant cook.
  - **Found, fixed:** `SlangCompiler::BuildAxisDeclarations` returned an empty list when `BuildDeclaredAxes` failed, so a
    cook reported the error and then exited 0 with every axis missing (an interface axis whose implementation holds a
    resource, a conformance to two interfaces, an invalid attribute). It returns the `CookResult` now and the driver stops.
    No test covered `SlangInterfaceHasResourceMember`; `AccessModelRejectTest` has a row for it now.
  - Not covered: a type argument that is not `public` in its module (Slang reports it as an undefined identifier in the link
    module), a shim extern in an array, and a shim interface that carries `ls_axis_interface`.
  - Next: C11 (`ls_format` per variant) on this machinery, planned first. Then F4.
- 2026-10-22. **C11.0 spike done** (DiamondDogs C11, `ls_format` per variant). No code change. Decisions O22 to O24 made by the
  author: accept the six float formats now (integer formats have no use case), the format is specified by the author as
  data, and a subset of formats is policy (`CookValues`), not a second mechanism in the shader.

  **Measured (slangc 2026.18.1, `spirv` and `wgsl`):**
  - **The format is a type argument in this Slang, not only an attribute.** `hlsl.meta.slang` defines a named storage format
    type for each entry of `slang-image-format-defs.h` (`RGBA8Unorm`, `RGBA8Snorm`, `RGBA16F`, `R32F`, `RG32F`, `RGBA32F`),
    each an `ITexelElement` whose `DataType` is `float`, `float2`, or `float4`. `WTexture2D<RGBA16F>` is a write-only storage
    texture of that format, and the `[format(...)]` attribute is deprecated for Slang 2026 (`DeprecatedFormatAttribute`).
  - So a generic shim impl takes the format as a **type** argument: `BoundWriteStorageTexture2D<F : ITexelElement>` holds a
    `WTexture2D<F>`, and a link module exports `Dst : IWriteStorageTexture2D = BoundWriteStorageTexture2D<RGBA16F>`. All six
    formats link on both targets, as a block field, and WGSL gives `texture_storage_2d<rgba16float, write>` and the other
    five spellings. The type argument is a plain identifier, so the F2.5 link source needs no new syntax.
  - The texel width differs (`R32F` stores a `float`, `RG32F` a `float2`). A small helper interface `IWiden` with
    `FromFloat4` on `float`, `float2`, and `float4`, and a constraint `where F.DataType : IWiden`, lets one impl serve all six
    behind `Store(uint2, float4)`.
  - **Access decides which formats WGSL allows.** `RWTexture2D<F>` is always `read_write` in WGSL, even for a store-only
    kernel, and Slang rejects `rgba16float` there (E56106). It also accepts `rgba8unorm`, `rg32float`, and `rgba32float` as
    `read_write`, but core WebGPU allows `read_write` for `r32*` only (the others need the
    `readonly_and_readwrite_storage_textures` feature, `slang-emit-wgsl.cpp` `wgslFormatSupportsReadWrite`). `WTexture2D<F>`
    gives `write` access for all six, and it has `Store` and no subscript. A read-only use of a format type
    (`Texture2D<RGBA16F>`) is a sampled `texture_2d`, so a read needs no storage format.
  - The portable pattern is therefore: read through `ITexture2DRef`, write through a write-only storage shim.
    Read and write of one storage texture (`read_write`) is portable for `R32Float` only.
  - The manifest needs no change: the reflected `StorageFormat` of the linked program is already recorded per binding and
    compared by both validators.

  **Plan (each slice ends green):**
  - **C11.1.** `public enum StorageFormat` in `builtins/LodestoneShims.slang` (the six cases, a comment that lists each format
    with its WGSL and Vulkan names, and the formats that are not accepted with the reason), a table in `target/` (case,
    Slang type name, `TextureFormat`, WGSL name), `TextureFormat::RGBA8Snorm` and its case in the three readers (Slang
    reflection, `SpvModuleReader`, `WgslValidator`). Proof: a test reads the enum cases through reflection and compares them
    with the table, and cooks one probe per format with a literal `WTexture2D<Type>` for both targets: all three readers
    must report the same `TextureFormat`.
  - **C11.2.** `IWriteStorageTexture2D`, `IWiden`, and `BoundWriteStorageTexture2D<F>` in the shim module, and a table row whose
    implementation takes the format. `ShimTableTest` covers each format.
  - **C11.3.** An attribute on the extern names the format axis (`[ls_storage_format("LEVEL_FORMAT")]`); the axis is an enum
    axis over `StorageFormat`. The link module of these externs is built for each variant, from the axis value, like the
    enum-axis modules; fixed externs stay in the module that each worker context loads once. Errors: an axis that does not
    exist or is not a `StorageFormat`.
  - **C11.4.** A mip-style kernel with a format axis, and a control with a literal type for each value. Proof, for both
    targets and every value: the binding's `StorageFormat` and placement equal the control's, and no variant needs
    `StorageImageWriteWithoutFormat`. A policy that lists a subset of values cooks that subset.
  - Not in this plan: integer formats, a `read_write` shim (R32Float only on core WebGPU), `Bgra8Unorm`, and the 8-bit,
    16-bit, and packed formats, which are not WebGPU core storage formats.
- 2026-10-23. **C11.1 done**, not committed. 40 of 40 tests, 60 of 60 known-good dumps.
  - `public enum StorageFormat` in `builtins/LodestoneShims.slang` (six cases, with a comment that lists WGSL and Vulkan names
    and the formats that are not accepted). `target/StorageFormatTable` (case, Slang type, `TextureFormat`, WGSL name).
    `TextureFormat::RGBA8Snorm` is appended at the end of the enum, so that a stored value keeps its meaning, and it has a
    case in the three readers (`FromSlangImageFormat`, `TextureFormatFromTint`, `TextureFormatFromSpirv`).
  - `StorageFormatTest` compares the enum (through the cases of an enum axis), the table, and what all three readers give
    for a probe with one texture of each format. It compares each reading with the table, because readers that both give
    `Invalid` agree.
  - **Found, fixed:** `SymbolTable::AddSource` read declarations inside comments. The new doc comment of `StorageFormat` shows
    an axis declaration as an example, and `ShimTableTest` failed with "could not read the default of extern constant
    'LEVEL_FORMAT'" because every module that imports the shim module saw a second axis. The scanner now drops `//` comments,
    block comments over one line or several, and keeps a string with two slashes (`StripComments`). Words in a comment no
    longer count as tokens, so an axis that only a comment names is pruned from the space. No known-good dump changed.
  - Not checked: the Vulkan names in the enum comment are the `VkFormat` names, and the claim that these six formats are
    mandatory storage image formats still comes from memory of the Vulkan required format table.
  - Next: C11.2.
- 2026-10-23. **C11.2 done**, not committed. 40 of 40 tests.
  - `builtins/LodestoneShims.slang`: `IWriteStorageTexture2D` (`Store(uint2, float4)`), `IStorageTexelFromFloat4` (implemented
    by `float`, `float2`, and `float4`), and `BoundWriteStorageTexture2D<F : ITexelElement>` over a `WTexture2D<F>`.
  - `ShimTable`: a fifth row, with `ShimResourceClass::StorageTexture` and `ImplementationTakesFormat` (the implementation
    takes the Slang type of a format as its only type argument and the interface takes none). `ShimTableTest` instantiates the
    implementation with each of the six formats against the interface.
  - `StorageFormatTest` cooks `ShimStorage.slang` (one shim texture of each format in a block): both targets cook with both
    cross-checks, and each binding is a write-only storage texture with the format of the table.
  - `MakeShimLinkSource` rejects a format row with `ShimExternInvalid` until step C11.3 supplies a format: no extern can
    link a storage shim yet, so a shader uses the implementation type directly for now.
  - Next: C11.3 (the attribute that names the format axis, and the link module for each variant).
