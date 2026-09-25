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
| F1.4 | Add a SPIR-V validator (decision O2). | It must find a planted mismatch, and pass KitchenSink. |
| F1.5 | Per-target policy: a module with no section for a requested target (decision O4). | `PolicyDocumentTest` rows. |
| F1.6 | Cook KitchenSink for both targets in one cook. Add SPIR-V known-good dumps. | The bundle holds two real profiles. The known-good check covers both. |

### F2 — The shim module and the two tables, proved on WGSL

The Bound form works on WGSL, so this slice needs no SPIR-V. It can run beside F1.

| Step | Work | Proof |
|---|---|---|
| F2.1 | Add the shim module to `builtins/`, beside `LodestoneAttributes.slang`. | It compiles for both targets. |
| F2.2 | Add the shim table and the profile form table in `target/`. | The agreement test of D6. |
| F2.3 | Recognize an extern whose interface is in the shim table. Keep it out of the axis space. | The variant count of a shim module does not change. |
| F2.4 | Allow a resource member in a shim impl. Keep `rejectResourceMembers` for a technique axis. | The technique-axis rejection test still fails the cook. |
| F2.5 | Build the impl name from the extern (`IReadBuffer<PointLight>` gives `BoundReadBuffer<PointLight>`). Import the module that declares the element type. | `FindDeclaringModule` gives the import. `todo.md` holds the related import item. |
| F2.6 | A cook test asset: a material that uses buffer, texture, and sampler shims in a `ParameterBlock` and in an entry point `uniform` parameter. | The cross-check agrees. Check the binding numbers: no global may move down a set (vocabulary §9e, measurement notes). |
| F2.7 | Fix the cross-check for an entry point `uniform` container. Today it expects an empty name for `entryPointParams`. | F2.6 passes with F0.1 in place. `AccessModelRejectTest` uses `EntryPointResourceStruct.slang` as its mismatch row, so replace that row with another mismatch in the same step. |

### F3 — The capability set

| Step | Work | Proof |
|---|---|---|
| F3.1 | A string table of capability names and a bitmask over it. Use the feature-bit names in vocabulary §9d, not only extension names. | `ShaderManifestRejectTest` reads the table back. |
| F3.2 | Fill `Profile::CapabilityFloor` from the profile. | The manifest dump shows it. |
| F3.3 | Fill `Variant::CapabilityRequirement` from what the variant uses. Source to be decided: Slang metadata, or the SPIR-V capabilities in the binary. | Compare the two sources where both exist. |

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
| O4 | F1.5 | A module has no policy section for a requested target. | **Decided 2026-09-25.** The cook fails. Inheritance from another section is in `todo.md`. |
| O5 | F1.2 | Which Vulkan environment does the `spirv` profile target? | **Decided 2026-09-25.** Vulkan 1.2 (SPIR-V 1.5) for now. At the end of phase F, measure the device coverage and decide the range again, from 1.0 up to 1.4. |
| O6 | F3.3 | Where does a variant's capability requirement come from? | Decide after F3.1. Measure what Slang metadata gives first. |

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
