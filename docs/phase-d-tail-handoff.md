# Handoff: the end of phase D, and the start of phase E

Written on 2026-08-20, and corrected on 2026-08-21. This document records the state, the decisions,
and the next task. Read it before you read any other plan.

Text in this file follows ASD-STE100.

---

## 1. State on 2026-08-21

Phase D is complete. **The build is green, and every test passes.** Eleven test targets exist. Ten are
unit tests, and `CookTest` is the end-to-end cook.

`OceanFft` cooks with exit code 0. The numbers are 35 variants over an index space of 56, 105 entry
point variants, 77 unique sources, 4 resources, 1 resource list, 7 footprint lists, 2 visibility
lists, and 4 artifacts identical across two cooks.

### What changed on 2026-08-21

The source tree moved into subfolders. `include/` and `src/` each hold `permute/`, `compile/`,
`model/`, `target/`, `emit/`, and `driver/`. Each include is qualified, such as
`#include "permute/PermutationSpace.hpp"`. A folder is therefore a rule a reader can check, and not
only a shorter listing.

`PermutationSpace` became a class that owns its axes. The eight functions that took a space are now
member functions. `PermutationAxis::Parent` became `ParentIndex`, an index into the owning space,
because the space holds its axes by value and a copy would leave a pointer aimed at the axis of the
original. A copy of a space is deleted for the same reason, and a move is kept.

`PermutationBinding` became a struct with `Axis` and `Value` fields, in place of a `std::pair`.

`src/permute/PermutationSpace.cpp` went from 702 lines to 399. Two files came out of it:

- `permute/ExternConstantScanner.{hpp,cpp}` reads `extern static const` declarations out of Slang
  source text. It names no permutation type and no Slang type.
- `permute/PermutationRegistry.{hpp,cpp}` holds `k_ModuleSpaces` and the two lookups. **Step E6
  deletes this file.**

`ExternConstantScannerTest` is new, and it found a defect in code that moved unchanged.
`DeclaresExternConstantNamed` accepted a *use* of a name as a declaration, so an axis named after a
constant that only appeared in the default of a different constant passed
`VerifyAxisNamesAreDeclared`. That is the hole rule 6 of `CLAUDE.md` describes. The check now reads
the name to the left of the `=`.

## 2. Build and test

The build tree uses the compiler in `CMakeCache.txt`, which is Visual Studio 18 Community, version
14.51. Start each build from the environment of that compiler:

```
cmd /c '"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build D:\ShaderTools\build\ninja-msvc --config Debug'
```

**Do not build from the Visual Studio 2022 Professional environment.** CMake keeps the compiler that
the cache holds. The 14.51 compiler then reads the 14.38 headers, `ammintrin.h` raises C4392, and
every SPIRV-Tools target fails because Slang builds them with `/WX`. The `lodestone` targets still
build, so the fault stays hidden until a change invalidates the whole graph.

Run each test executable directly from `build/ninja-msvc/tests/Debug/`. Read the exit code.

**Do not pipe a long run into a pager or into `Select-Object`.** PowerShell holds the whole stream
until the command ends. The run then prints nothing for minutes, and it looks like a deadlock. A test
that stops with an assertion also loses its buffered output, so use `Start-Process` with
`-RedirectStandardOutput` when the text matters.

---

## 3. What phase D finished today

Steps D0 to D8b were already complete. Today closed the last three items.

- **D8c.** `CanonicalAssignment` is a distinct type. Only `CanonicalizeAssignment` builds one. A
  partial assignment can no longer reach `ComputeVariantIndex`. The type rejected three test call
  sites on the first build, and each one was stating something it did not mean.
- **Step 5 of the session plan.** `SlangCompiler::PrepareRawModule` replaces
  `ResolveExternConstantDefaults`. `GetExternConstantDefaults` is gone. Stage 4 now reads the extern
  defaults from `RawModule`, and it no longer holds a reference to the compiler.
- **D9.** `CLAUDE.md`, `docs/shader-cooker-handoff.md`, and `docs/shader-cooker-change-summary.md`
  record the finished state and the measured numbers.

The `--no-dedupe` question that D8 left open is settled. `DisableDedupe` turns off all six interners.
Every artifact takes its own index.

---

## 4. Four defects found and corrected

Each one was found by running the tests before any edit. Two tests were failing on `master`.

1. **`ComputeAxisInfluence` read past the end of a vector.** The linear rewrite read
   `orderedIndices[end]` before the range test that guards it.
2. **`ComputeAxisInfluence` dropped every second group.** The loop set `begin = end` inside a `for`
   header that then increments. This one does not stop the cook. It reports `Inert` for an axis that
   changes the output, and `Inert` is the answer that lets a combinatorial explosion through.
   `DedupeInfluenceTest` now holds a module that only the second group can measure.
3. **The hash name was written twice, and the two copies disagreed.** `CookedLibrary.hpp` passed the
   literal `"xxHash3"` while `k_HashName` held `"xxHash3_64"`. Every interner now reads `k_HashName`.
4. **`ResolvedBindingView::operator==` read through a null pointer.** `ResolveLayoutView` leaves the
   footprint pointer null when a resource declares no `[vx_*]` size.

A fifth defect stopped every cook of a module with no registered permutation space.
`EnumerateActiveCombinations` called `space.front()` on an empty vector. `PermutationIndexTest` now
covers the empty space. **Step E6 depends on that path**, because its acceptance test is a cook of
`OceanFft` with no registry entry.

---

## 5. Three gaps that became phase E work

`docs/phase-e-data-driven-permutations.md` holds the plans. Sections §1c, §1d, and §1e state each
one, and the step table names them E0a, E0b, and E0c.

- **E0a. The entry point scope is never read. Complete on 2026-08-21.**
  `CollectBindingRangeDrafts` now walks any scope, and `ExtractRawEntryPoint` calls it a second time
  with `entryPointLayout->getTypeLayout()`. `EntryPointParamsCookTest` is the acceptance test, and
  `RawVariant::GlobalBindings` is now `RawVariant::Bindings`. Section 11 records what it taught.
- **E0b. A `ParameterBlock` is invisible.** Slang gives a parent scope only descriptor ranges, so the
  parent sees one range with a `descriptorSetIndex` of -1. The contents live in a sub-object range.
  §1d records four traps that each cost a cook.
- **E0c. A pointer type reaches WGSL, and no check sees it.** This one is a defect, and not a missing
  capability. A cook can exit 0 and write invalid output.

Take E0a first. It separates the binding range walk from `ExtractRawBindings`, and E0b calls the same
separated function again.

`EntryPointReflection::getTypeLayout()` is `getVarLayout()->getTypeLayout()`. `ExtractRasterState`
already calls `getVarLayout()` to read the varying semantics, so E0a starts one step from code that
runs today.

**A probe module proved each of the three.** Write the probe modules into `tests/assets/`, because the
cross-check is the acceptance test for E0a and E0b.

---

## 6. The phase F conclusion

`docs/phase-f-vocabulary.md` §9a records the measurement. The short form follows.

Slang lowers a buffer to a device address only when the source declares a pointer. A
`ParameterBlock` that holds a `StructuredBuffer<T>` gives `OpMemoryModel Logical` and a descriptor
set.

Each module has one `OpMemoryModel` instruction, so the addressing model is a property of the module.
Link time specialization runs inside a module whose memory model is already set. **The mechanism
therefore cannot select the access model.**

`__target_switch` is also the wrong mechanism. It selects the pointer form for every SPIR-V device,
and some SPIR-V devices support no buffer device address. Section 6 of the phase F document states the
rule: a capability that varies between devices inside one target belongs to Lodestone.

---

## 7. Decisions the author made today

Record these as settled. Do not open them again without a request.

1. **No link time specialization of a resource declaration.** Section 6 above gives the mechanical
   reason. She also expects a large cost in the generated code, and nobody measured that part.
2. **Lodestone gets a preprocessor pass.** It finds a `ParameterBlock` that carries an annotation. It
   then generates one form of that block for each access model that the cook builds. The pass does no
   dead code elimination, and it changes nothing downstream.
3. **The annotation goes above an ordinary `ParameterBlock`.** A separate `LodestoneParameterBlock`
   type would break the Slang language server. `LodestoneAttributes.slang` already declares custom
   attributes that reflection reads, so the mechanism exists.
4. **The access model is a Lodestone dimension, and not a Slang target question.** One cook produces a
   bound form and a pointer form of one SPIR-V module. The client selects one at run time.

---

## 8. Three constraints the preprocessor must obey

These came out of the same discussion. No step performs them yet.

1. **Substitution must preserve line numbers.** Step D5b gives every Slang message a file, a line, and
   a column. A rewrite that moves a line makes each of those point at the wrong line, in a file the
   author cannot open. Replace one declaration line with one generated line. Append every other
   generated line at the end of the file.
2. **Do not parse a structure body.** The bound form always compiles, and reflection then states each
   field name, each field kind, and each element type name. The text pass needs to find one
   declaration line. The shape comes from the bootstrap compile that step E6 already needs.
3. **The pass owes a comparison.** The cross-check compares the emitted text against reflection, and
   both come from the rewritten source. It therefore cannot find a wrong rewrite. The comparison that
   finds one runs across arms: the bound form and the pointer form must describe the same block, with
   the same field names, the same order, and the same element types.

---

## 9. Open questions for the author

1. **The stage 1 dump does not carry the axis fields that E2 adds.** Phase E §9 states the two
   answers. Add the fields now as nulls and keep the byte comparison, or let E2 move the dump and
   write a field by field comparison. Settle this before E2 starts.
2. **`ResolveLayout` and `ResolveLayoutView` disagree about an absent footprint.** The value form
   gives a default `ResourceFootprint`. The view form gives a null pointer. Section 4b of the phase D
   plan argues that a default is the wrong substitute. Settle this before a consumer reads either one.
3. **The JSON target is named `lodestone_json_writer`.** `JsonReader.cpp` would land in a target whose
   name says that it only writes. Rename the target, and keep the `lodestone::json` alias.

---

## 10. Where the documents are

`docs/phase-d-stage-separation-plan.md`, `docs/phase-e-data-driven-permutations.md`, and
`docs/phase-f-vocabulary.md` **are not tracked by git**. Only `shader-cooker-handoff.md` and
`shader-cooker-change-summary.md` are. Today's edits to the three untracked files exist on disk only.
Copy them somewhere safe, or add them to the repository.

---

## 11. What step E0a found

Written on 2026-08-21.

**The placement was right on the first cook, and the name was not.** Slang moves each entry point
`uniform` parameter to the global scope, and gives that scope the fixed name hint `entryPointParams`.
The emitted WGSL therefore declares `entryPointParams_albedoMap_0` where reflection says `albedoMap`.
`slang-ir-entry-point-uniforms.cpp` line 584 writes that string, and it is a constant.

`StripSlangNameMangling` already removed the numeric suffix Slang appends, so the prefix joins it
there. Only that one fixed string comes off, and a test holds that line. The name is a decoration the
backend adds, and the model keeps the name the author wrote.

**Ownership decides visibility, and the probe shows why the placement query is not enough.**
`EntryPointParams.slang` gives `ShadeCS` and `TintCS` one texture and one sampler each. Slang placed
them at four separate bindings in this module, so a placement query would have answered correctly
here. It cannot be trusted to: Slang generates each entry point as its own artifact, so two entry
points can hold different resources at one group and binding. `CollectUsedBindingIndices` now reads
the global scope alone, and each entry point appends the indices of the rows it owns.

**The measured result.** Five bindings, and three visibility lists: `ShadeCS` reads 0, 1, and 2,
`TintCS` reads 0, 3, and 4, and `ClearCS` reads 0 alone.

**Every `OceanFft` artifact stayed byte identical.** The two stage dumps changed by one key, because
`globalBindings` became `bindings` with the field.

**A `[vx_*]` annotation cannot be written on an entry point parameter, and step 5 of §1c assumed it
could.** The walk does call `CollectRawSizeAttributes` for an entry point range, so the code path is
there. Slang rejects the attribute before it: `[lodestone_extent_2d("256", "128")] uniform Texture2D<float4>
albedoMap` fails with `E31002: invalid attribute placement`.

`LodestoneAttributes.slang` declares each attribute with `[__AttributeUsage(_AttributeTargets.Var)]`,
and `core.meta.slang` line 4738 declares `_AttributeTargets.Param` beside it. So the fix is one word
in the attribute declarations. It is not part of E0a, because `LodestoneAttributes.slang` is the
shader-side contract and every test shader imports it. **Decide whether a footprint may be declared on
an entry point parameter at all** before making that change: a footprint is per variant, and an entry
point parameter is per entry point.

---

## 12. The binding scope name

Written on 2026-08-21, after step E0a.

A binding now carries `ScopeName` beside `Name`, from `RawBinding` through `ReflectedBinding` and into
`ManifestBinding` and `BindingInfo`. It is empty at global scope, and it holds the scope chain the
emitted shader puts around the binding otherwise.

**Why the field exists, and not only for the cross-check.** E0a made a name ambiguous: `albedoMap` in
`ShadeCS` and `albedoMap` in `TintCS` are two resources, and before this field the model recorded two
rows with one name. The visibility lists told them apart by index and nothing told them apart by name.
The scope is the other half of the identity.

The cross-check is the second reason. `ExpectedDeclaredName` builds `ScopeName + "_" + Name` and
compares that against the de-mangled emitted identifier. It states what it expects, so the check keeps
its strength. The fixed `entryPointParams_` strip that E0a first put in the WGSL scanner is gone, and
the scanner now names no Slang convention.

**Where the name comes from.** Reflection does not report it, and `getVarLayout()->getName()` on an
entry point layout returns nothing. Two parts build it:

- `k_EntryPointScopeName`, the fixed hint `slang-ir-entry-point-uniforms.cpp` line 584 writes. It is a
  Slang convention, so it lives in `src/compile/SlangCompiler.cpp` and reaches no other folder.
- `CollectScopeNames`, which walks the fields of a scope with `getFieldBindingRangeOffset` and appends
  the name of each struct field it descends through. Slang flattens a scope into one binding range
  list, and that call is the only thing that says which field a range came from.

The walk is what E0b needs as well, so E0b extends it rather than adding a second one.

**Measured.** `EntryPointParams.slang` reports `entryPointParams` for a bare parameter and
`entryPointParams_material` for a member of a struct parameter. Every `OceanFft` artifact differs from
the E0a baseline by exactly the new field: one `"scope": ""` line in each dumped binding, one `""`
argument in each generated `BindingInfo` row, and 40 bytes in the manifest, which is four records at
eight bytes plus one string reference.

**The other finding stands, with a correction.** Slang still refuses `[vx_*]` on a bare entry point
parameter. It accepts one on a **struct field**, so an entry point resource can declare a footprint
after all: put the resources in a struct and take one `uniform` parameter of that type.
`EntryPointParams.slang` covers that form in `MaterialCS`, and the annotation reaches stage 4. The
struct form is also the readable one, so nothing needs `_AttributeTargets.Param`.

---

## 13. What step E0b found

Written on 2026-08-21. `CollectSubObjectDrafts` in `src/compile/SlangCompiler.cpp` is the walk.
`tests/assets/ParameterBlocks.slang` and `ParameterBlocksCookTest` are the acceptance test.

### The rule that makes the two walks safe

The range walk keeps every binding range the parent placed itself. The sub-object walk keeps exactly
the rest, which are the ranges the parent describes with descriptor ranges alone. Both test
`getBindingRangeDescriptorSetIndex(range) >= 0`, so the two walks partition the ranges: no range is
drafted twice, and none is dropped.

**A global `ConstantBuffer<T>` is what proves the test is needed.** The parent places it, so it is an
ordinary binding, and it *also* reports a sub-object range. A walk that filtered on the binding type
alone drafted it a second time at the wrong location, and every entry point of `OceanFft` then failed
the cross-check with `reflection has @group(0) @binding(0) IfftParams : wgsl has no binding at that
location`. §1d step 1 says to filter on the binding type. That filter is not enough on its own.

### The three numbers, as measured

Four probe modules measured these. §1d called two of them right and one wrong.

| Number | Where it comes from | Measured |
|---|---|---|
| the space of the block | the sub-element space offset of the sub-object range, **plus the space base of the enclosing scope** | 1 for a block at global scope |
| the binding the contents start at | nothing: the contents already count from the start of the space | 0 and 1 for a block of two resources, 1 and 2 when a container takes 0 |
| the binding of the container | `getContainerVarLayout()->getOffset(DESCRIPTOR_TABLE_SLOT)` | 0 |

Two corrections to §1d:

1. **A scope has two bases, and they are not the same number.** `BindingScope` now carries `Base`,
   where a binding declared directly in the scope sits, and `SpaceBase`, where a block declared in the
   scope starts counting spaces. An entry point scope reported a slot offset of 0 and a sub-element
   space offset of 1, and its block took space 1. Reading one for the other puts every content of the
   block in group 0.
2. **A container exists only when the block holds ordinary data.** §1d says to give the container a
   binding of its own, without qualification. A block of resources alone emits no such slot, and
   drafting one reports a binding the shader has not got. `getSize(SLANG_PARAMETER_CATEGORY_UNIFORM)`
   on the element says which case this is: 16 for the probe with a `float4`, 0 for the probe without.

Step 3 of §1d is right, and reading the element var layout offset as a base is the way to get it
wrong: the contents of a block with a container reported 1 and 2, and the element var layout also
reported 1, so adding it moved every content by one.

### Steps 4 and 5 of §1d

**Step 4 holds.** `isParameterLocationUsed` answers for the contents of a block at global scope.
`GlobalBlocksCS` reads bindings 0, 1, 2, and 3, and the three that come from blocks reached the list
through the placement query.

**Step 5 has a stale premise, so nothing changed.** §1d says `FromSlangBindingType` maps
`BindingType::ParameterBlock` to `BindingKind::UniformBuffer`, and that the answer is wrong for a block
of textures. It maps to `BindingKind::ParameterBlock`, which states nothing false. The line is still
unreachable, because a block always carries a descriptor set index of -1 and the range walk drops it.
`BindingKind::ParameterBlock` is named in one other place, a `ToString` in `src/model/ShaderDataSchema.cpp`.

### Measured, and the regression

`ParameterBlocks.slang` reports eleven bindings across four entry points:

    0  Output       scope=""                          group 0 binding 0   StorageBuffer
    1  AlbedoMap    scope="MaterialBlock"             group 1 binding 0   Texture
    2  Sampler      scope="MaterialBlock"             group 1 binding 1   Sampler
    3  TuningBlock  scope=""                          group 2 binding 0   UniformBuffer, 32 bytes
    4  DetailMap    scope="OuterBlock"                group 3 binding 0   Texture
    5  AlbedoMap    scope="OuterBlock_Surface"        group 4 binding 0   Texture
    6  Sampler      scope="OuterBlock_Surface"        group 4 binding 1   Sampler
    7  AlbedoMap    scope="entryPointParams_surface"  group 5 binding 0   Texture
    8  Sampler      scope="entryPointParams_surface"  group 5 binding 1   Sampler
    9  AlbedoMap    scope="entryPointParams_detail"   group 6 binding 0   Texture
    10 Sampler      scope="entryPointParams_detail"   group 6 binding 1   Sampler

Four rows are named `AlbedoMap`, and only the scope tells them apart. The `[lodestone_extent_2d]` annotation
on the field of the block reaches stage 4 three times, once for each block of that type.

Every `OceanFft` artifact and both stage dumps are byte identical to the state before E0b.

### The two probes §1d asked for, and one it did not

- **A block of ordinary data alone.** It becomes one container in a space of its own. `TuningBlock`
  took group 2 and no content binding.
- **Two entry points, and a block on each.** Slang gave them separate spaces, groups 5 and 6, so
  ownership and a placement query agree in this module. Ownership is still what decides, because
  nothing states that Slang must separate them.
- **A block inside a block.** `OuterBlock` holds `Surface`. The spaces add, the contents take group 4,
  and the scope chain reads `OuterBlock_Surface`.
