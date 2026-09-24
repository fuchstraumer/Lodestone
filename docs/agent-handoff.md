# Agent handoff

Read this first. It gives the state, the next work, the open faults, and the facts that are expensive
to measure again. `CLAUDE.md` gives the build rules, the code map, and the author's positions. This file
is newer than `CLAUDE.md` on anything that moves.

Text in this file follows ASD-STE100. Compressed on 2026-09-24. Section 10 keeps one line for each
earlier update. Git history holds the full text.

`docs/` is in `.gitignore`. This file survives only as a force-add: `git add -f docs/agent-handoff.md`.

---

## 1. State on 2026-09-24

| Configuration | Build | Tests | Known-good dumps |
|---|---|---|---|
| RelWithDebInfo, `ninja-msvc` | green | 22 of 22 | 30 of 30 |
| Debug, `ninja-msvc` | green | 21 of 22: all but `KitchenSinkCookTest` (section 3, item 3) | — |

- Phases D and E are complete. E8 closed Phase E. Its final measurements are in
  `docs/phase-e-data-driven-permutations.md` §11a.
- The manifest is one bundle for each cook, format version 5. `CLAUDE.md` ("One output form") holds the
  layout.
- The client reads the bundle through `BundleView`, `ModuleView`, `EnvironmentView`, and the view types
  (`VariantView`, `EntryPointInstanceView`, `LayoutRange`, `ResolvedResource`, `UniformMemberRange`).
  `ShaderSourceProvider` allocates nothing, and `BindingInfo` is gone.
- The query layer reads the axis-active mask. A constraint on an axis selects only the variants where
  that axis is active. `Decode` and `Enumerate` set `QueryAxisValue::Active`.
- `--target` is required. A command line with no target fails with `NoTargetSpecified`.
- The KitchenSink set is the reference. `KitchenSinkCookTest` cooks 616 variants. The known-good check
  compares its five modules, six stages each.

**Phase F is the next work.** Section 2 gives the order.

## 2. Next work, in order

1. **Answer the open Phase F questions** in `docs/phase-f-vocabulary.md` §9. Q1 has an answer (§9a).
   Write each answer into that file.
   - Q2: does `DescriptorHandle` cover textures on WGSL and on SPIR-V? If yes, the Indexed access model
     is portable. Use the Slang submodule and a `slangc` probe.
   - Q3: what can a WGSL `override` size? This decides whether pipeline binding time is real on WebGPU.
     Use a `slangc` probe and Tint.
   - Q4: the exact Vulkan extension names for descriptor heaps and buffer device addresses. This needs
     the current Vulkan registry. It also fills the capability name table (section 3, item 4).
2. **Write the Phase F plan** from those answers.
3. **Add a second target: SPIR-V, Bound access model only.** Today `wgsl` is the only profile, so the
   profile grid, the per-profile policy, and the capability field have never held two real profiles.
   The target needs its own validator, a second opinion on SPIR-V bindings. SPIRV-Tools is already in
   the tree through Slang.
4. Then the runtime features: bindless (Indexed and Pointer placements), binding-time lowering, and the
   capability requirement that a client matches against its adapter.

Two client decisions are open. Ask the author before you build either.

- **Query presets.** A preset is a module-agnostic constraint set that matches an axis by name. The match
  rule is open: what a preset does with an axis that a module does not declare.
- **The persisted form of a variant.** `DecodedVariant` holds string views into one manifest and no axis
  names, so it cannot outlive that manifest. A persisted form needs (axis name, value) pairs with owned
  strings, and a load that re-keys them through a query.

## 3. Open faults

Ordered by what can write wrong output first.

1. **A compiled-in absolute path.** `src/compile/impl/SlangModuleContext.cpp:208` calls
   `std::filesystem::canonical("C:/SoftwareDev/Lodestone/tests/assets/")`. `canonical` throws on a path
   that does not exist, so a cook on another machine stops. Give this search path the treatment the
   other search paths get, or add a field to `SlangCompilerCreateInfo`.
2. **The manifest drops the sample type.** `ReflectedBinding::SampleType` exists in the cooker, but
   `manifest::Binding` has no field for it. A renderer cannot read the sample type of a texture. Add a
   record field and an emitter write, and compare it in `CheckManifestLayout`.
3. **The Debug Slang build asserts on `KsMaterial`, and the test then hangs.** Section 5 has the
   detail. The assert opens a modal dialog, and `scripts\run-tests.bat` sends the output to `nul`, so
   `run-tests.bat Debug` stops with no message and waits for a click. On 2026-09-24 it waited for 15
   minutes. Do not run `run-tests.bat Debug` unattended. Run the Debug tests one at a time, with a
   timeout, and skip `KitchenSinkCookTest`. Two fixes are possible: find the duplicate symbol, or make
   a Debug assert in the cook tests print and exit, not open a dialog.
4. **The capability requirement is always zero.** `Profile::CapabilityFloor` and
   `Variant::CapabilityRequirement` wait for a capability set: a bitmask over a string table of
   capability names, such as Vulkan extension names. The author agreed to that shape.
5. **Push constants and specialization constants are reserved, not cooked.** `BindingKind::PushConstant`
   exists, and nothing produces it. The specialization-constant table is always empty. Section 7 holds
   the plan.
6. **A build with `LODESTONE_ENABLE_WGSL` OFF does not link.** `include/LodestoneConfig.hpp.in` defines
   the switch, but no source reads it. Guard `WgslValidator` and the wgsl profile.
7. **No guard fails a cook that emits no manifest.** The required `--target` closes the zero-target
   case. A guard in the emit path is still open.
8. **The thread count is not tuned.** `ThreadPool::Initialize` uses `hardware_concurrency` and ignores
   `SlangCompilerCreateInfo::ExpectedBatchSize`. Measure before you tune.
9. **The Slang global-session convoy.** `slang_createGlobalSession` serializes per-thread startup.
   `k_UseSlangWorkaround` in `SlangModuleContext.cpp` toggles a bypass. It helps a release cook and hurts
   a Debug cook. It is `true` now. Read it before you measure anything.
10. **`tests/DeclKindProbe.cpp`** is a throwaway Slang probe. Remove it when it stops earning its place.

Review points from the view-type work, not faults:

- `CheckManifestLayout` compares through `ResolvedResource::Record()` and the two record comparators.
  Moving it onto the view accessors is a separate change.
- A view points at its `EnvironmentView`. A view from a temporary environment, or from a moved
  `ShaderSourceProvider`, dangles. The headers document this.

`todo.md` holds the rest, grouped by area.

## 4. Build and test

`CLAUDE.md` holds the full rules. These points each cost a session once.

- Use `scripts\build.bat`, never a bare `cmake --build`. Read the exit code of the build itself. A pipe
  into `grep` or `tail` gives the exit code of that command.
- A change to the `third_party/slang` submodule rebuilds all of Slang, about ten minutes.
- After an interrupted regeneration, a Debug link can fail with `LNK1163` on a COMDAT in
  `ShaderManifest.cpp.obj`. A clean recompile of that file fixed it on 2026-09-24. If it comes back on a
  clean tree, it is a real fault.
- Run `scripts\run-tests.bat RelWithDebInfo` after each change. A green build proves less than it looks.
- Run `python scripts\check-known-good.py` after a change to reflection, resolve, intern, or freeze. It
  cooks each KitchenSink module and compares every stage dump. It once found a defect that no validator
  saw. Accept a changed dump only after you read the diff.
- For a change that must not change output, save the KitchenSink bundle hash and a
  `manifest_dump --with-sources` JSON before the change. Compare both after it.

## 5. Measured facts (do not measure again)

Slang documents none of these. Probe modules measured them.

**Documents that hold more facts.** `docs/phase-e-attribute-spike.md` holds the attribute and axis-read
facts, including the `__include` recursion. `docs/phase-e-interface-spike.md` holds the interface-axis
facts. `src/compile/impl/SlangReflector.cpp` holds the scope, block, and binding-range walk.

**Reflection.**

- An imported module is a requirement, so a synthetic axis module must be a component.
- A user attribute cannot take a type argument, and a generic user attribute does not parse. Reflection
  reads only int, float, and string arguments.
- `getScalarType()` on an enum returns `None` across a module boundary. The enum case read uses the
  width of the case value blob and assumes a signed `int` (`EnumTagKindFromByteWidth`).
- A `TypeReflection` is interned for each session, so pointer identity holds across modules.
  `FindDeclaringModule` uses this to find the module that declares an axis type.
- A `ParameterBlock` that holds only nested blocks reflects as a zero-size `ConstantBuffer` container,
  with descriptor set index 0 and type kind `ParameterBlock`. The walks discriminate on the type kind,
  so the container is descended, not drafted as a uniform buffer.
- `extern static const` on a struct compiles. A grouped configuration struct is possible.

**WGSL.**

- Slang does not infer a depth texture from usage. A `Texture2DArray<float>` sampled with `SampleCmp`
  emits `texture_2d_array<f32>`, and Tint rejects it. Use `DepthTexture2D`, `DepthTexture2DArray`, or
  `DepthTextureCube` with a `SamplerComparisonState`. Issue shader-slang/slang#6942.

**Debug Slang.**

- `loadRootModule` on `KsMaterial` raises `unexpected: duplicate global instruction`. The check is
  `checkIRDuplicate` in `third_party/slang/source/slang/slang-ir-link.cpp`, inside `#ifdef _DEBUG`, so a
  release Slang does not run it. It started with the Slang update in commit `557ea09`. The cook code does
  not run before this call.

**Cook cost.** `docs/phase-e-data-driven-permutations.md` §11a. A one-variant cook takes about 0.4 s,
so startup dominates a small cook. KitchenSink takes about 3.4 s for 616 variants.

## 6. Design decisions in force

- **Variant keys are per module.** A shared axis value set under one name in two modules was a footgun,
  and one global key forced a phase barrier. Shared meaning lives in shared types (a shared enum or
  interface) and in query presets, never in a global axis.
- **A type-backed axis gets its identity from its type.** An enum or interface axis needs no attribute
  argument. Only an integral or boolean axis would need an explicit shared-axis reference, and that
  reference must be a string.
- **A root axis is storage and dedup only.** Two modules share a root axis when name, kind, domain, and
  binding time agree and the values fit in order. No key crosses a module boundary.
- **A constraint on an axis implies that the axis is active.** `WhereNoneOf` is a complement
  `WhereAnyOf`, so it follows the same rule.
- **Plain integer indices in the public API.** Strong index types were tried and removed. `CLAUDE.md`
  ("A guardrail must pay for itself at the call site") holds the rule. Do not propose them again.
- **Bindings are resolved on demand.** `ShaderSourceProvider::Bindings` returns a `LayoutRange`. The
  provider caches nothing, because a client reads the bindings once to build a pipeline.
- **The iterators are input iterators.** A proxy iterator returns its value by value, so it cannot meet
  the legacy forward or random-access requirements.

## 7. Deferred capability areas (do not lose)

These cannot wait much longer once a second target exists.

- **Bindless (Indexed and Pointer).** The reflected schema (kind, shape, access, format, member layout)
  is the invariant, and placement is the variant. Under bindless the schema facts are the only guard
  against a mismatch a driver would otherwise catch, so they matter more. Add unbounded and runtime-array
  detection then. Do not design the Indexed and Pointer payloads until the target is concrete.
- **Specialization constants.** Slang reflects them (`SPECIALIZATION_CONSTANT`). WGSL spells them
  `override`. The table slot exists.
- **Push constants.** Slang reflects them (`PUSH_CONSTANT_BUFFER`). Model them as their own category. On
  Vulkan a push-constant member can be a buffer device address, which ties them to the Pointer model.
- **Portable geometry (vertex and index pulling).** `todo.md` ("Phase F: portable geometry") holds the
  design. One unknown comes first: entry-point synthesis for the input-assembler arm, because Slang ties
  varying inputs to entry-point parameters. Not scheduled.

## 8. How this author works

Read "Working with this author" and "How this repository thinks" in `CLAUDE.md`. Three points:

- She reserves the implementation work she enjoys. Plan it and explain it. Write it only when she asks.
- Ask before a design choice that is hers. State a recommendation, not a survey.
- Measure, do not estimate. A probe module answers a Slang question in one build.

## 9. Tooling notes

- `tools/manifest_dump` reads a bundle and writes JSON. `--with-sources` adds the source text.
- `tools/cooker_console` (`lodestone_cooker_console`) is the CLI. `--dump-stage=all` writes six dumps,
  named `<module>_<target>_<Stage>.json`. Before 2026-09-24, the Raw dump was empty and the Resolved file
  held the Interned dump. Do not trust a dump from before that date.

## 10. History

One line for each update. Git history holds the full text.

- **2026-09-17.** Phase E done through E7, plus enum axes. The client query surface was complete.
- **2026-09-19.** The cooker driver became a chain of step functors (`src/driver/steps/`). Cross-module
  enum resolution was fixed. The per-module key decision was made.
- **2026-09-22.** Tint replaced the text WGSL scanner. `BindingKind`, `ResourceShape` (a flag enum), and
  `ResourceAccess` were split. The member walk reached structured buffers. The block-of-blocks walk was
  fixed. Test targets reached 22.
- **2026-09-22, night.** The multi-module bundle (three scopes, root axes, per-environment extents) was
  built and wired into the driver. The round trip gained key-decode and axis-mask checks.
- **2026-09-23.** `TableRef` and format version 5. Strong index types were tried and removed. The view
  types were added.
- **2026-09-24.** On-demand bindings, and `BindingInfo` removed. `--target` became required. The query
  layer reads the axis-active mask. `AxisNames` and `AxisValues` were added. The stage dumps were fixed
  and the KitchenSink known-good baseline was accepted. The stale `CookTest` line was retired. E8 closed
  Phase E.
