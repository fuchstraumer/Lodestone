# Phase D: separable stages and `--dump-stage`

This document plans the work that `docs/cooker-rendergraph-plan.md` calls tier D, and that
`docs/shader-cooker-handoff.md` §3 calls the stage separation pass. Read those two files first. This
file is the method.

---

## 1. What phase D actually contains

The cost table in the design document gives phase D one row. That row holds two different things.

| Part | What it is | What it gives |
|---|---|---|
| The stage 3 and stage 4 split | Architecture | A second target language becomes possible |
| `--dump-stage` | Observability | You can see between two stages |

The design document cut phase D because the split alone "produces no new capability". That was correct
at the time. It is not correct now, and the reason is the order.

**Build `--dump-stage` first, on the stages that already exist. Then use it to prove that the split
changed nothing.**

This repository already uses that method three times. `VerifyLibraryRoundTrip` compares the tables
against the text. `VerifyManifestRoundTrip` compares the manifest against the module.
`--verify-deterministic` compares one cook against a second cook. Each one turns a claim into a
comparison of bytes.

A stage dump does the same job for a refactor. Cook `OceanFft.slang`, keep the dump, do the split, cook
again, and compare. A difference is a defect, and the dump names the variant and the field. Without the
dump you have four output artifacts, and each one is far away from the code you changed.

So `--dump-stage` is not a convenience that comes after the split. It is the test harness for the
split.

---

## 2. Where the boundaries are today

Eight stages have four real types. The other four boundaries are inside a function.

| # | Stage | Boundary today | State |
|---|---|---|---|
| 1 | Declare | `PermutationSpace`, from the static `k_ModuleSpaces` table | Real |
| 2 | Enumerate | `VariantSet` from `EnumerateVariants` | Real |
| 3 | Compile | none | **Fused with 4** |
| 4 | Resolve | `CompiledVariant`, from `SlangCompiler::CompileVariant` | **Fused with 3** |
| — | Cross-check | `ValidateVariantReflection` in `CookerDriver.cpp` | A free function, target specific |
| 5 | Normalize | none, and this is correct | Empty on purpose |
| 6 | Intern | the three interners, held **inside** `CookedModule` | **Fused with 7** |
| 7 | Key | `CookedModule` after `FreezeModuleTables` | **Fused with 6** |
| 8 | Emit | three emitters over the frozen model | Real |

**This table describes the state before phase D, and it is kept for the reasoning that follows it.**
Every boundary in it is a real type now. Stage 3 returns `RawVariant`, stage 4 is `ResolveVariant` in
`ResolveStage.cpp`, the cross-check is `ValidateResolvedLibrary` behind a target profile, and stage 6
is `InternedModule`.

Two problems follow from the table.

**Problem 1: stage 3 and stage 4 are one call.** `SlangCompiler::CompileVariant` links the program,
generates the target text, extracts reflection, reads the `[vx_*]` attribute strings, and evaluates
each size expression against the axis values. `ExtractDerivedSize` reads `impl->CurrentSymbols`, and
`CompileVariant` fills that member immediately before the link. The Slang contact and the arithmetic
share one object and one call.

**Problem 2: the frozen model is not frozen.** `CookedModule` holds `SourceInterner`,
`LayoutInterner`, and `RasterInterner` as members. Stage 8 therefore receives the stage 6 machinery
along with the stage 7 result. Rule 6 of the handoff says an emitter must not reach past
`CookedLibrary`. Today the type permits it.

Problem 1 blocks a second target. Problem 2 does not block anything yet, and it is cheap to fix while
the surrounding code is open.

---

## 3. A numbering conflict to settle first

**Settled. D0 landed, and the three files now agree.** The rest of this section states the rule.

The documents disagreed, and the work would have made the disagreement worse.

- `docs/cooker-rendergraph-plan.md` lists **8** stages. Stage 5 is Normalize.
- `CLAUDE.md` lists **9** steps. Step 5 is the cross-check, and Normalize does not appear.
- `docs/shader-cooker-handoff.md` §3 item 3 puts the cross-check **inside** stage 4.

Settle it this way, and say so in each file:

**Eight numbered stages, each one a transformation. The cross-check is not a stage.** It is a
validator. It reads the output of stage 4 and returns a count. It changes nothing. Give it the name
`ValidateResolvedLibrary` and attach it to stage 4, exactly as the handoff asks, but do not give it a
number. A stage transforms. A validator compares.

This matters more than it looks. The cross-check is the one part of the pipeline that is target
specific by construction, and a number would imply that a target must supply it. A target must supply
a validator only when it can. A target with no text to scan supplies none, and the pipeline still runs.

---

## 4. The types to add

### `RawLibrary`, the output of stage 3

Stage 3 must hold everything Slang says, and must have no opinion about any of it.

The one field that decides the split is the size attribute. `DerivedSize` today holds both the raw
`Expression` string and the evaluated `ElementCount` and `Extent` values. Two designs are possible.

**Design A: leave the numbers at zero in the raw stage.** Fewer types, about 10 new lines. But a
reader cannot tell "not resolved yet" from "the shader declared no attribute". Both look the same.

**Design B: give stage 3 its own binding type with no derived size at all**, plus a parallel list of
attribute records. Each record holds a binding index, the attribute name, and the argument strings.
Stage 3 then carries strings it does not understand, which is the exact statement you want to make.
About 60 new lines, and one more type to keep in step.

**Take design B.** The extra 50 lines buy a type that cannot represent a wrong state. That is the same
trade this repository already made with `Active` and `Canonical`, and with the frozen model. Design A
saves lines and loses the property.

Sketch of the shape, not the final code:

- `RawSizeAttribute` — binding index, attribute name, argument strings.
- `RawBinding` — name, placement, kind, shape, sample type, storage format, access, sampler type,
  stride, byte size, array count, uniform members. **No derived size.**

**Shape `RawBinding` for the three-way split that D8b describes.** Placement, visibility, and
footprint have three different keys and three different lifetimes. D4 is where this type gets written,
so it must not be written as one flat record and reshaped later. See section 4b.

**Do not make `Group` and `Binding` mandatory fields of `RawBinding`.** They are concepts of the bound
access model. A pointer-model target places a resource at a byte offset in a struct, and an indexed
target places it at a heap index. Model placement as a variant, or at minimum leave the two fields
optional. This is the field most likely to harden into a WebGPU assumption, and the handoff already
records the smell: *"`BindingKind` is WebGPU shaped."* See `docs/phase-f-vocabulary.md` §4.
- `RawEntryPoint` — name, suffix, stage, workgroup, target text, usage mask, raster state.
- `RawVariant` — suffix, description, dense index, the shared `GlobalBindings` list, the size
  attributes, and the entry points.
- `RawModule` — module name, entry point names, and **the extern constant defaults**.

The last field is easy to miss. `ExtractDerivedSize` needs the axis values *and* the defaults of every
`extern const static` constant that no axis drives. `ResolveExternConstantDefaults` gets those from
Slang. Stage 4 must not call Slang, so the defaults must travel in the stage 3 output.

## 4b. `ReflectedBinding` is three things in one struct

Found while D2 was in progress, and measured from the `cooked` stage dump that D2 produces.

`OceanFft` interns **21 layouts**. It has one set of four resources, at one group and one binding
number each, in every entry point of every variant. So the shader declares **one** layout. Every field
that says where a resource lives is invariant across all 21 entries. Two fields vary:

| Field | Distinct values | Keyed by |
|---|---|---|
| everything that states placement | 1 | the module |
| `EntryPointUsageMask` | 3 | the entry point |
| `Derived` (the evaluated `[vx_*]` size) | 7 | the variant |

**21 = 1 x 3 x 7.** The layout table is the Cartesian product of three tables, because one struct
carries three concerns that each want their own key.

### The three concerns

- **Placement.** Where the resource lives, so a caller can create a binding. Group and binding number
  today; a byte offset or a heap index under another access model. Per module.
- **Visibility.** Which entry points can see the resource. This is what becomes stage visibility in a
  real API. Per entry point. D3 moves it structurally.
- **Footprint.** How much. Per variant, per binding.

Do not call visibility "binding time". `docs/phase-f-vocabulary.md` §3 already defines binding time as
*when must the value be known*, which is a different question.

### Footprint is a sum type, not a struct with holes

A buffer footprint and a texture extent are two kinds, not one kind with unused fields.

- A buffer has an element count and an element stride, and their product is a byte count the cooker
  can compute.
- A texture has an extent. **That extent is not a byte count and cannot become one.** `VkImageCreateInfo`
  takes a `VkExtent3D` plus format, tiling, mip levels, and sample count, and only
  `vkGetImageMemoryRequirements` states the real size, because optimal tiling pads and swizzles.
  WebGPU takes a `GPUExtent3D` and offers no size query at all. The extent is a creation parameter.

One struct that holds both invites a caller to multiply the extents by a stride and hand the result to
an allocator. That is wrong on every desktop API. Model the footprint as a sum, so the wrong thing
cannot be written down. `PermutationValue` is already a `std::variant`, so this is house idiom.

**Do not fill the unused fields with `uint32_t` max.** A maximum is a plausible arithmetic operand: a
caller who multiplies it overflows and gets a wrong number rather than a failure. Zero gives a
zero-size allocation, which fails at the API call where a person can see it. The repository already
says zero is the absence of a valid answer, and `DerivedSize` already carries explicit presence flags.
A sum type removes the question.

**A sampled texture does not know its texel size.** `ShaderDataSchema.hpp` records why: the shader
states that it samples a `float4`, never that the texture is `Rgba16Float`. Only a storage texture
spells the format into the binding type. A texture footprint that carries a byte stride will one day
get that stride from the sample type, and the number will be confidently wrong.

### Why this matters more than a table size

**Nothing in the repository checks that a layout collapse was correct.**

- `VerifyLibraryRoundTrip` replays the **source text** only.
- `CheckManifestLayout` compares the manifest against `module.Layouts[index]`, which is the table it
  was written from. It proves the serialization is faithful. It cannot see that the table collapsed
  two entries that had to stay apart.

The source table has a real second opinion. The layout table has none. That is why the comment on
`ReflectedBinding::operator==` says every field a consumer reads must take part in the key: with no
comparison to catch a bad collapse, a total key is the only thing holding the line.

**So a field cannot leave the key while it stays in the type.** Drop `Derived` from the key alone and
the interner collapses seven layouts that differ in a field a consumer still reads, `IfftInput`
reports 65536 elements for a variant that asked for 4096, and the cook exits 0. The field has to leave
the type, and the step that collapses the table owes the repository the layout round trip that does
not exist yet.

### `ResolvedLibrary`, the output of stage 4

This is `CompiledVariant` as it stands today. Rename it, or keep the name and add the alias. The
contents do not change, so stages 5 to 8 do not change.

### `ResolveContext`, the input of stage 4

Holds the symbol table for one variant: the axis values from `descriptor.Canonical`, plus the extern
defaults from `RawModule`. `EvaluateExpression` already takes this shape, so this is a small type.

---

## 4c. Diagnostics from this repository, not only from Slang

Found after D5b landed. This section records the finding. **No step performs it**, and the reason is
in the last part.

D5b gives a diagnostic a record, a location, and a sink. The parser is the half that is specific to
Slang. The record and the sink are not, so a check that this repository performs can report through
the same path. That path already reaches a terminal, and it will reach a console and an editor.

### What already detects, and reports poorly

This is an upgrade of checks that exist. It is not a new class of check, and pricing it as new work
gives the wrong answer.

| Check | Detects today | Reports today |
|---|---|---|
| A size expression names an unknown symbol | `AttributeExpressionUnknownSymbol` | the binding name |
| An attribute has the wrong number of arguments | `AttributeExpressionParseFailed` | the binding name |
| An axis name matches no `extern const static` | `VerifyAxisNamesAreDeclared` | the axis name |

Each one prints a name, because a name is all the code holds. A reader must then find the line.

### The part that is missing is the location

Slang can supply it. Three facts, read from `third_party/slang/include/slang.h`:

- `ISession::getDeclSourceLocation(DeclReflection*, SourceLocation*)`, at line 4710.
- `SourceLocation` holds a file path, a line, and a column, at line 4459.
- `IModule::getModuleReflection()` gives the module's declaration tree. A child of kind
  `DeclReflection::Kind::Variable` is a resource declaration, and `asVariable()` gives the reflection
  this repository already reads.

The cooker does not walk that tree. It reads `ProgramLayout` and the parameter list, and neither one
carries a location. So the work is a second traversal, and a location field that leaves stage 3 on the
raw model. Stage 4 must not call Slang, so a check in stage 4 can only report a location that stage 3
carried out for it.

### Two rules this work must obey

**Plan it with D8b, never after it.** D8b reshapes `RawBinding` into placement, visibility, and
footprint. A location added later opens that type a second time, for a reason that is known now.

**A lint must compare, and not inspect.** The three checks above each compare two answers that were
derived apart: the symbols of an expression against the axis values and the extern defaults, and an
axis name against the declarations in the shader. That is why each one finds a real defect. A check
that only reads one artifact and calls it suspicious is a defensive check with better formatting, and
`CLAUDE.md` states why those go.

### Why no step performs this here

The best form of the work belongs to phase E, and doing it in phase D builds the weaker form first.

`VerifyAxisNamesAreDeclared` searches the raw source text for a string today. Phase E moves each axis
declaration onto the `extern const static` as an attribute. The check then becomes a walk of the
declaration tree over structured data, with a location on each declaration, and the crude string
search goes away rather than gaining a location. See `docs/phase-e-data-driven-permutations.md` §9.

Walk the declaration tree once, in phase E. Do not attach a location to the parameter list path now.

---

## 5. The dump format

Write JSON. Three reasons.

1. `tools/manifest_dump/include/JsonWriter.hpp` already exists, it emits and never parses, and it has
   no exception paths.
2. A text dump is diffable. `fc` or `git diff` names the line. A binary dump needs a second tool.
3. A dump is a diagnostic, not a shipped format. It needs no version field and no reader.

**Write each dump through the `OutputSink`, never to the filesystem directly.** This is the important
decision, and it gives three things at no cost:

- `MemoryOutputSink` captures the dumps, so a test can read them without touching a disk.
- `--verify-deterministic` compares every artifact the sink holds, so it checks the dumps too. An
  `unordered_map` that reaches a dump then fails the cook.
- The file naming follows the rule that the other artifacts already follow.

Name the artifacts `<Module>.stage-<name>.json`.

**Do not dump the full target text.** The WGSL already ships in three other artifacts. A dump that
holds it is large, slow to diff, and hides the tables you want to read. Dump the source index, the
byte length, and the content hash. Add `--dump-stage-sources` later if a real need appears.

Accepted stage names: `space`, `variants`, `raw`, `resolved`, `interned`, `cooked`. Accept `all`.
Allow the option more than once.

### Phase D needs no JSON parser

This is worth stating, because it looks like it needs one and it does not.

Every check in section 8 is a **byte comparison**. `--verify-deterministic` compares strings. A golden
comparison is `git diff`. The dump unit test compares text. **Nothing reads a dump back.**

So phase D needs a writer, and the writer already exists. The question of which JSON library to depend
on belongs to E5, where the policy file needs real parsing, and it must be answered with those
requirements in hand. Do not answer it here.

---

## 6. Steps, in order

Each step must end with the same evidence. Section 8 states it. Do not start the next step until the
current one gives that evidence.

### D0 — Settle the documents

Fix the stage numbering in `CLAUDE.md`, `docs/cooker-rendergraph-plan.md`, and
`docs/shader-cooker-handoff.md`, as section 3 states. No code changes.

*Small. No risk.*

### D1 — Move `JsonWriter` to a shared target

`JsonWriter` lives in `tools/manifest_dump` and the cooker cannot reach it. Make it its own small
target, `lodestone::json`, and link it from both `lodestone` and `manifest_dump`.

**Keep the property that the manifest_dump link line proves.** That tool links
`lodestone::client_internal` and never the cooker or Slang. `lodestone::json` must depend on nothing,
so the property holds.

*Small. No risk. No behavior change.*

### D1b — The offline cooker target

`CookTest.cpp` holds the only `main` today. Add a real executable: `ParseCommandLine`, a
`FileOutputSink`, and `RunCook`. `CookTest.cpp` already shows the shape, so this is small.

Do it here, and not later, because two things are behind it:

- **`--dump-stage` must be reachable by hand.** A flag that only `TEST_ARGS` can set is a flag nobody
  reaches for when something looks wrong, and reaching for it is the entire value of D2.
- **The live cooker needs a process to be.** See `docs/tooling-track.md`.

*Small. No risk.*

### D2 — Add `--dump-stage`, for the stages that already have types

- Add a `DumpStages` bit set to `CookerOptions`, and parse `--dump-stage=<name>`.
- Add `StageDump.{hpp,cpp}`, with one function for each stage.
- Implement `space`, `variants`, and `cooked` now. The other three names parse and do nothing yet.
- Add a unit test that dumps a hand-built `CookedModule` and checks the text. It needs no Slang, so it
  runs in milliseconds.
- Cook `OceanFft.slang` with `--dump-stage=all`. **Keep the three dumps. They are the golden files for
  every step after this one.**

*Medium. Low risk. This step is the harness, so do not shorten it.*

### D3 — The `GlobalBindings` rename

The handoff asks for this under "A stage gets bindings that it does not touch", and asks for it as a
pass of its own. Do it here, before the split, for two reasons. The split moves this code anyway, and
doing both at once gives a diff nobody can read. Also `RawVariant` wants one shared binding list and a
mask for each entry point, which is exactly what this rename produces.

- `CompiledVariant::GlobalBindings` holds the set once.
- `CompiledEntryPoint` holds a parallel `UsageMask`.
- Rebuild the per-entry-point view at the intern call site.

**The layout count must stay 21.** The mask is part of the layout key today. Moving the mask out of
the key changes the count to about 7, and that is a separate decision the handoff records. Do not make
it here.

*Medium. Medium risk, and the golden dumps find any error at once.*

### D4 — Add `RawLibrary` and `CompileVariantRaw`

- Add the types from section 4.
- Add `SlangCompiler::CompileVariantRaw`, which returns `RawVariant`.
- **Keep `CompileVariant`**, as a thin wrapper that calls the raw function and then the resolve code
  that is still in place. Nothing downstream changes yet.
- Implement the `raw` dump.

*Large. Low risk, because the old path stays live and the golden dumps must not move.*

### D5 — Move stage 4 out of `SlangCompiler`

- Add `ResolveStage.{hpp,cpp}` with `ResolveVariant(const RawVariant&, const ResolveContext&)`.
- Move `ExtractDerivedSize`, `ExtractDerivedExtent`, `EvaluateExtentArgument`, and the
  `CurrentSymbols` member out of `SlangCompiler.cpp`.
- Delete the wrapper. `CookerDriver` now calls stage 3, then stage 4.
- Implement the `resolved` dump.

**After this step `SlangCompiler.cpp` names no size expression and no `[vx_*]` attribute, and
`ResolveStage.cpp` names no Slang type.** Check both with a search, and record the result.

*Large. Medium risk. This is the step the whole plan exists for.*

### D6 — Test stage 4 with no Slang

Add `tests/ResolveStageTests.cpp`. Build a `RawVariant` by hand. Resolve it. Check that the derived
element count is correct, that an unknown symbol fails with `AttributeExpressionUnknownSymbol`, and that a
binding with no attribute keeps `HasElementCount` false.

This test is the proof that the split worked. Before D5 it cannot be written at all.

*Small. No risk. Do not skip it.*

### D5b — The diagnostic sink

`docs/shader-cooker-handoff.md` §5 designs this and nobody scheduled it. Schedule it here.

- Parse the Slang blob into a record at the boundary: severity, file, line, column, code, message.
- Take the record shape from the Language Server Protocol `Diagnostic` type. Editors already read it.
- Report each record to an abstract sink. **Compilation must never format a string.**
- Write one sink for stderr. `docs/tooling-track.md` covers the others.

**Two reasons this belongs inside phase D rather than beside it.**

It cannot endanger the goldens. A diagnostic goes to stderr, never to an artifact, so changing how one
is emitted cannot move `cooked.hpp`, the manifest, or a stage dump. It is free of the one risk that
sets the order of every other step here.

**D7 needs it.** The capability profile in the handoff says an unmapped enum value must be reported
through the sink, because *"a silent `Invalid` return repeats the failure shape of an axis name
typo."* Building D7 first means writing that reporting twice.

After D5 rather than earlier, because the stages then have boundaries, and each one takes a
`DiagnosticSink&` at a seam that already exists.

*Medium. Low risk, and no artifact can move.*

**Done. Two findings changed the shape of the step.**

**Do not parse the block form.** Slang renders a diagnostic two ways. The default is a Rust-style
block, with a caret row and a quoted line of source. That is a layout for a person, and it may change
in any release. Slang also has a tab separated form, one record per line, behind the compiler option
`EnableMachineReadableDiagnostics`. Its field order is stated in
`third_party/slang/source/compiler-core/slang-rich-diagnostics-render.cpp`:

    E<code>	<severity>	<file>	<start line>	<start column>	<end line>	<end column>	<message>

`SlangCompiler.cpp` turns the option on, and `SlangDiagnosticParser.cpp` reads it. The parse takes a
string and names no Slang type, so `DiagnosticParserTest` needs no compiler.

**A worker thread must not touch a sink.** Entry point codegen runs on `std::async`, and it reported
diagnostics from the worker. A sink has no lock, and a message from a worker arrives in whatever
order the threads finished. Each worker now carries its diagnostic text back with its code, and the
joining thread reports in entry point order. So two runs of one cook report the same messages in the
same sequence.

**`--quiet` does not reach a diagnostic.** That flag turns off the cooker's per-variant reflection
report, which the cooker chose to print. A compiler message is not the cooker's to withhold, so
`StderrDiagnosticSink` has no suppression switch.

### D7 — Make the cross-check an interface

- Add a target profile type. It supplies a name, an **access model**, and an optional validator.
- The validator interface takes the emitted text and the used bindings, and returns a
  `BindingComparison`.
- `WgslBindingScanner` becomes the WGSL implementation. Its code does not change.
- Add `--target=<name>`, and accept `wgsl` only. The parameter must exist and must be used, or the
  next target will find the same fusion again in a new place.

**Add the access model field now, even though `Bound` is the only value.** Phase F adds `Indexed` and
`Pointer`, and the profile type is being written anyway. See `docs/phase-f-vocabulary.md` §4 and §8.

*Medium. Low risk.*

**Done. Four notes.**

`BindingComparison` moved from `WgslBindingScanner.hpp` to `TargetProfile.hpp`. The interface must not
depend on the one implementation it has. `ScanWgslBindings` and `CompareBindings` are unchanged, and
`WgslReflectionValidator` in `TargetProfile.cpp` only gives them the shape the interface asks for.

**`CookerOptions::ValidateReflectionAgainstWgsl` became `ValidateAgainstEmittedText`.** Leaving the
old name would have put the target back into the mechanism through a field, which is the fusion this
step removes.

**The cook says what it checked, once for each module:** the target name, the access model, and
whether the cross-check ran, was off by `--no-validate`, or was unavailable because the target
supplies no validator. A cook that checked nothing must not read like a cook that checked and agreed.

**`--target` is rejected at the command line, never in the driver.** A name that reaches
`CookerOptions` is a name `FindTargetProfile` accepts, so no later stage asks again. The rejection
names the targets this build has.

Adding `--target` put `ParseCommandLine` at cognitive complexity 28 against a threshold of 25.
`--dump-stage=`, `--target=`, and `--O` were three branches of one shape, so they became a
`k_ValueFlags` table beside the existing `k_SwitchFlags`. A fourth value flag now costs a row rather
than a branch.

### D8 — Separate stage 6 from stage 7

Move the three interners out of `CookedModule` into an `InternedLibrary` builder.
`FreezeModuleTables` becomes the function that turns the builder into the frozen module. Stage 8 then
receives a type that holds tables and nothing else.

This is problem 2 from section 2. It blocks nothing today, so it is the first item to cut if the work
runs long.

*Medium. Low risk. Cuttable.*

**Done. The type is `InternedModule`, not `InternedLibrary`.** Interning is per module, so a library
level type would be a vector that nobody reads. `FreezeModuleTables` takes the builder by value and
returns a `CookedModule`, so nothing after the freeze can reach an interner.

**The frozen module keeps the facts, not the machine.** The dedup report and the `cooked` dump both
read the hash name, the enabled flag, and the interning counters after the cook. Those now travel on
`CookedModule` as three `TableStatistics` members. A frozen model may record how it was built; it may
not keep the thing that built it.

**Stage 6 now has a boundary type, so `--dump-stage=interned` writes a real dump.** It carries one
thing that no later stage can: the provenance. The interner records which artifacts mapped onto which
unique entry, `FreezeModuleTables` copies out the unique entries and drops the rest, and after that
the answer is gone. A collapse that surprises you is findable in `interned` and nowhere else.

**This step adds an artifact rather than moving one.** All four output artifacts and all five earlier
goldens stay byte identical, in both the deduped and the `--no-dedupe` arm. `--verify-deterministic`
now compares 10 artifacts instead of 9, and `tests/goldens/` holds six files.

### A finding from D8, settled

**Settled in favour of the first reading. `DisableDedupe` in `src/CookedLibrary.cpp` now disables all
six interners, the raster interner included, and `DedupeInfluenceTests.cpp` agrees with it.**
`--no-dedupe` means "every artifact takes its own index", which is what `ContentInterner.hpp` states
and what `CLAUDE.md` rule 2 states. The rest of this entry is the original finding.

**`--no-dedupe` did not disable the raster interner.** `CookerDriver.cpp` disables the source and the
layout interners and leaves the raster interner on. `DedupeInfluenceTests.cpp` disables all three. So
the control arm the test exercises is not the control arm the cooker runs.

Nothing observed today depends on it, because a compute module has exactly one raster state and the
table is the same size either way. A raster module would differ.

D8 kept the behaviour exactly as it was, because a step that moves a boundary must not also change
what a flag does. Decide it separately:

- If `--no-dedupe` means "every artifact takes its own index", the driver is wrong and the raster
  interner must be disabled too.
- If it means "collapse nothing that a consumer could notice", the test is wrong and must match.

The first reading is what `ContentInterner.hpp` states and what `CLAUDE.md` rule 2 states.

### D8b — Split placement, visibility, and footprint

Section 4b states the finding and the reasoning. This step performs it.

- Take the footprint out of `ReflectedBinding`, as a sum type. It becomes a table keyed per variant
  and per binding, the way `SourceIndices` is keyed per variant and per entry point.
- Take visibility out of the layout key. D3 already moved it structurally, so this is the second half
  of that move.
- `ManifestBinding` loses the same fields, and the manifest gains the tables that hold them.
- **Build the layout round trip.** Replay each variant's layout through the finished tables and
  compare it against the bindings the compiler produced for that variant, exactly as
  `VerifyLibraryRoundTrip` does for source text. This step is what makes that check necessary, and the
  step must not land without it.

**Read section 4c before you write the new binding type.** A source location on a resource declaration
is the field that makes a lint from this repository as useful as one from Slang, and section 4c states
where that location comes from. It is not this step's work, but this step decides whether the field
costs one edit or two.

**This step is last among the code steps, and its evidence rule is different.** Every other step in
this plan proves itself by producing byte-identical artifacts. This one moves the artifacts on
purpose: the layout table collapses from 21 entries to 1, the generated source shrinks, and the
manifest record size changes. `ManifestBinding` is pinned by `static_assert` because the record size
is part of the file format.

**The schema change itself is cheap while nothing consumes this repository.** No product embeds the
library, no manifest exists that anyone must read, and the cost of a format change is regenerating the
goldens and checking the result. Treat the format as settled only when a consumer arrives. The reason
this step goes last is the evidence rule, not the risk: it is the one step whose output must move, so
it must not run while other steps still use "nothing moved" as their proof.

Evidence instead:

1. The layout count for `OceanFft` goes from 21 to 1, and the dedup report says so.
2. The new layout round trip passes, and fails when a layout is corrupted by hand.
3. Every variant still resolves to the same placement, the same visibility, and the same footprint it
   had before, checked against the D2 `cooked` golden by a field by field comparison rather than a
   byte comparison.
4. The source table does not move. 77 unique sources, 105 entry point variants.
5. `--no-dedupe` gives the same answers as the deduped arm, which `DedupeInfluenceTest` already holds.

**Why this is in phase D and not beside it.** Phase D exists to make each stage a transformation with
one input type, one output type, and one responsibility. A binding record that carries three concerns
with three different keys is the same defect as a fused stage, in a type instead of a function. D4
writes the stage 3 binding type, so the split has to be decided before D4 even though it is performed
here.

*Large. Medium risk, and the only step in this plan that changes the file format.*

**Done. The shape is four tables, and the measurement moved twice.**

| Table | `OceanFft` | Keyed by |
|---|---|---|
| resources | 140 -> **4** | interned by content |
| resource lists | 35 -> **1** | per variant |
| footprint lists | 35 -> **7** | per variant |
| visibility lists | 105 -> **2** | per variant and entry point |

Section 4b measured 21 layouts and read them as 1 placement x 3 usage masks x 7 derived sizes. Two
of those three numbers were wrong about what they counted.

**Visibility is 2, not 3.** Two of the three entry points read the same set of resources. The usage
mask separated them because the mask held a different *bit*, not because the entry points saw
anything different. The old key counted entry point identity and called it visibility.

**A resource is 4, not 1.** Section 4b counted one layout, which is a list of four bindings. The
resource table holds each binding once, so four is the same fact stated per resource.

### Two decisions that came out of review

**Visibility is a list of indices, never a mask.** `1u << entry_point_index` was undefined behaviour
at 32 entry points, with nothing bounding the index. A module with a shared frame constant layout can
reach that. The list also has the right meaning: an entry point sees a subset, and a subset is a list.

**A footprint is per variant, and never per entry point.** How much of a resource exists cannot depend
on which shader reads it. A layout can differ per entry point, and does: the permute pass of `OceanFft`
reads the root data and not the lookup tables.

### The layout round trip

`VerifyLayoutRoundTrip` was written before this step, against the single table model, so it was
known-good before the tables moved under it. It replays every (variant, entry point) through the
finished tables and compares against `BuildEntryPointLayout`.

It was proved to fail twice: once against the old model by moving a binding number, and once against
the new model by corrupting a footprint. The second proof matters, because the resolution path is
different code.

`CheckManifestLayout` now walks the manifest the way a consumer walks it, from variant to resource
list to footprint list to visibility list, rather than reading the tables the emitter just wrote.

### Manifest format

`ManifestBinding` lost the four derived size fields and gained a `PlacementKind` byte. Three run
tables and three payload tables replace `ManifestLayout`. `ManifestVariant` gained a resource list
index and a footprint list index. `ManifestSlot` names a visibility list rather than a layout.

The `sizeof` assertions are gone. They pinned a format that nothing reads yet, and every agent that
met them treated them as a constraint. `k_IsManifestRecord` replaces them: a record must be trivially
copyable and must not need more than 8 byte alignment, because the reader reinterprets file bytes as
records. Pin the sizes when a manifest that this build did not write must open.

### Evidence

Every artifact moved, as this step is allowed to. The manifest fell from 659.5 KB to 653.5 KB, the
generated C++ from 712.1 KB to 688.2 KB, and the `cooked` dump from 156.6 KB to 67.7 KB. Sources stay
at 77 and entry point variants at 105.

### Still open

**`BindingInfo` in `ShaderLibraryTypes.hpp` keeps the flat derived size fields.** The generated C++
header is a projection for a consumer, and `ShaderLibraryEmitter.cpp` joins the four tables back into
one array for each (variant, entry point). So the sum type stops at the manifest. `todo.md` plans to
delete the C++ emitter, which is why this was not carried further, but a consumer of the generated
header can still multiply a texture extent by a stride.

### D8c — Give an assignment a type that says which assignment it is

Found during D6. `PermutationAssignment` names four different things. An active assignment, a partial
one, a canonical one, and the argument of every function that takes any of them all share one type.

`ComputeVariantIndex(const PermutationSpace&, const PermutationAssignment& canonical)` states its
precondition in a parameter name. Nothing else holds it. Give it a partial assignment and it reads
past the end of `axis->Values` through `std::ranges::find`, and it gives back a plausible wrong index.
Give it an assignment that omits an axis and it dereferences a null pointer.

**Neither failure is reachable today, and this step adds no check.** `EnumerateVariants` is the only
caller, and it calls `CanonicalizeAssignment` on the line above, so the value always comes from the
axis and every axis is always present. A runtime check here would be a defensive check on data this
code wrote, and the repository removes those. Commit `25aacfa` removed one, correctly.

The repository's own rule says what to do instead: prefer the design where the wrong thing cannot be
written down. Make the canonical form a distinct type.

- A `CanonicalAssignment` that only `CanonicalizeAssignment` can return.
- `ComputeVariantIndex` takes that type, so a partial assignment fails to compile.
- No runtime cost. The type holds the same vector.

**Do this at the end of phase D, not earlier.** Phase E replaces mixed radix with a ranking index and
rewrites this code, so the work is only worth doing if it survives that. It survives as a shape rather
than as an implementation: whatever computes the index in phase E still needs an input it can trust.
Carry the shape forward, and drop the implementation if phase E does not want it.

*Small. No risk. No behavior change, and no artifact can move.*

**Done. `CanonicalAssignment` holds a `PermutationAssignment` and exposes it read only.** Only
`CanonicalizeAssignment` builds one, through a private constructor and one friend declaration. It
converts to `const PermutationAssignment&` so every reader of a canonical assignment keeps working
unchanged, and the conversion runs one way only. That direction is the safe one: the widening
direction loses a guarantee that nothing downstream reads.

The type earned its place on the first build. `PermutationIndexTests.cpp` and `StageDumpTests.cpp`
both handed a plain `PermutationAssignment` to a function that needed the canonical form, and both
now fail to compile until the value comes from `CanonicalizeAssignment`. `StageDumpTests.cpp` built
its canonical assignment as `PermutationAssignment{}`, and `DedupeInfluenceTests.cpp` built one by
hand for a two axis space while naming one axis. Neither was reachable as a defect, and neither was
stating what it meant.

A default constructed value is empty rather than partial, because `VariantDescriptor` and
`LibraryVariant` hold the type as a member. Nothing reads it before `CanonicalizeAssignment` fills it.

### D9 — Update the documents

Mark handoff §3 complete. Rewrite the data flow section of `CLAUDE.md` against the real call order.
Strike defect 5 from `docs/shader-cooker-change-summary.md`. Record the measured numbers again.

*Small.*

**Done. The measured numbers, from a cook of `OceanFft.slang` with `--verify-deterministic`:**

| Quantity | Deduped | `--no-dedupe` |
|---|---|---|
| variants | 35, over an index space of 56 | 35 |
| entry point variants | 105 | 105 |
| sources | 77 | 105 |
| resources | 4 | 140 |
| resource lists | 1 | 35 |
| footprint lists | 7 | 35 |
| visibility lists | 2 | 105 |
| hash collisions | 0 | 0 |
| generated C++ | 688 KiB | 733 KiB |
| manifest | 653 KiB | 686 KiB |

Both arms pass every round trip and `--verify-deterministic`.

### Three defects the document pass found

The step ran the test suite before it edited anything, which the earlier steps had stopped doing. Two
tests were failing on `master`, and the third defect was behind one of them.

**The hash name was written down twice.** `ContentHash.hpp` declares `k_HashName`, and
`CookedLibrary.hpp` passed the literal `"xxHash3"` to all six interners. The constant said
`xxHash3_64`. So the dedup report and every stage dump named a hash that no constant held, and
`StageDumpTest` asserted a third spelling, `fnv1a-64`, which was the name before the swap. Every
interner now takes the name from `k_HashName`, and the test reads the constant rather than a literal.
The comment in `ContentHash.hpp` says why. **The dedup report text moved**: `hash function: xxHash3`
became `hash function: xxHash3_64`.

**`ComputeAxisInfluence` read one element past the end, and dropped every second group.** The linear
rewrite in commit `4360a4e` reads `orderedIndices[end]` before the range test that guards it, which
aborts a debug build. It also sets `begin = end` inside a `for` header that then increments, so the
next group starts one element past its own first element and the group is never measured.

The second half is the one that matters. Nothing crashes, and the axis influence quietly reports
Inert for an axis that changes the output. That is the answer that lets a combinatorial explosion
through the check that exists to catch it. `--no-dedupe` cannot find it, because both arms call the
same function.

`DedupeInfluenceTests.cpp` gained a module that only the second group can measure: one entry point
whose text depends on the first axis only when the second axis is true. The check was proved to fail
against the defect and to pass without it.

**`ResolvedBindingView::operator==` dereferenced a null pointer.** `ResolveLayoutView` leaves the
footprint pointer null when a resource declares no `[vx_*]` size, and the comparison dereferenced both
pointers with no test. This crashed `DedupeInfluenceTest` as soon as the abort above stopped hiding
it. The comparison now treats absent as equal to absent, and never equal to a value.

**One asymmetry is left open, and it is the author's to settle.** `ResolveLayout` substitutes a
default `ResourceFootprint` for an absent footprint, and `ResolveLayoutView` gives a null pointer. So
the value form and the view form do not describe the same thing. §4b of this document argues that a
default is the wrong substitute, because zero is the absence of a valid answer and a caller can
multiply it. Decide which form is right before a consumer reads either one.

---

## 7. Cost

| Step | Work | Lines | Risk | Cuttable |
|---|---|---|---|---|
| D0 | Document numbering | ~30 | none | no |
| D1 | `JsonWriter` to a shared target | ~40 | none | no |
| D1b | The offline cooker target | ~80 new | none | no |
| D2 | `--dump-stage` and three dumpers | ~350 new | low | **no — it is the harness** |
| D3 | `GlobalBindings` rename | ~120 / −60 | medium | yes, but D4 gets harder |
| D4 | `RawLibrary` and `CompileVariantRaw` | ~260 new | low | no |
| D5 | Stage 4 leaves `SlangCompiler` | ~200 / −220 | medium | no |
| D5b | Diagnostic sink and the stderr sink | ~220 / −80 | low | yes, but D7 gets worse |
| D6 | Stage 4 unit test | ~90 new | none | **no — it is the payoff** |
| D7 | Target profile and validator interface | ~150 | low | yes |
| D8 | Stage 6 apart from stage 7 | ~120 / −40 | low | **yes, cut this first** |
| D8b | Placement, visibility, and footprint apart | ~400 / −150 | high | yes, but decide it in D4 |
| D9 | Documents | ~60 | none | no |

The smallest set that gives the capability is D0, D1, D1b, D2, D4, D5, D6, D9. That set makes the
target language a parameter and gives stage 4 a test. D3 makes D4 and D5 much easier to read. D5b
makes D7 honest and unblocks the tooling track. D7 makes the parameter real. D8 is hygiene.

D8b is different from every other row. It is cuttable as an implementation, and it is **not cuttable
as a decision**: D4 writes the stage 3 binding type, and a type written as one flat record has to be
rewritten later along with every consumer. Decide the shape in D4 even if D8b never runs.

---

## 8. Evidence, at the end of every step

Every step must produce all of this. A step that does not is not complete.

1. `cmake --build build/ninja-msvc --config Debug` returns 0. **Read the exit code of the build
   itself.** A pipe into `tail` gives you the exit code of `tail`.
2. Every test passes. There are ten targets now, not six. Run each executable directly rather than
   piping a whole `ctest` run into a pager: the harness prints nothing until the end, and a test that
   aborts loses its buffered output.
3. A cook of `tests/assets/compute/Ocean/OceanFft.slang` returns 0.
4. The four output artifacts are byte-identical to the artifacts from before the step:
   `cooked.hpp`, `cooked_OceanFft.cpp`, `OceanFft.ldshaders`, `ShaderLibrary.dedupe.txt`.
5. The `cooked` stage dump is byte-identical to the golden file from D2.
6. The numbers do not move: 35 variants over an index space of 56, 105 entry point variants, 77 unique
   sources, 0 hash collisions. The layout count was 21 until D8b, which replaced the layout table with
   four tables. See the D9 note for the numbers after that step.
7. `--verify-deterministic` passes.
8. `--no-dedupe` cooks and gives correct output.

Item 5 is the new one, and it is the reason the order in section 6 puts D2 before D3.

**D8b is the one exception, and it is deliberate.** That step collapses the layout table on purpose,
so items 4 and 5 cannot hold. It carries its own evidence rule, stated with the step. No other step
may move an artifact.

---

## 9. Risks, and what to do about each

**The cross-check moves twice.** D3 changes what it reads, and D7 changes how it is called. Two changes
to one function in one work stream. Keep the two commits apart, and check the mismatch count after
each. It must stay 0 for OceanFft.

**`ExtractEntryPointReflection` is a long function with many callees.** D4 and D5 both cut into it. Do
not try to split it and move it in one commit. Move first with the shape unchanged, then split.

**The extern defaults are easy to forget.** Section 4 states why stage 4 needs them. A cook that
forgets them fails with `AttributeExpressionUnknownSymbol` and names `IFFT_NUM_WAVE_CASCADES`. That is a
loud failure, so the risk is small, but it will cost an hour if it is a surprise.

**The dumps must stay ordered.** A dump that reads an `unordered_map` gives a different order on a
second cook. `--verify-deterministic` finds this, because the dumps go through the sink. Run that flag
in step D2, not later.

**~~There is still no cooker executable.~~ Closed by D1b.** `tools/cooker_console` builds
`lodestone_cooker_console`, so every flag is reachable by hand. `CookTest.cpp` still holds a `main` of
its own, and `TEST_ARGS` in `tests/CMakeLists.txt` gives it a command line.

---

## 10. One more consumer: phase F

`docs/phase-f-vocabulary.md` describes a desktop Vulkan target that reaches buffers through device
address pointers. Two items in this plan serve it directly, and both are noted above: the access model
field on the target profile in D7, and the optional placement on `RawBinding` in D4.

Neither adds work. Both are hard to retrofit, because every later field that assumes a group and a
binding number has to be found again.

---

## 11. What this unlocks

After D5, a second target needs a target profile, a Slang target format value, and a validator. It
needs no change to stages 5 to 8, because those stages already read a model that names no Slang type
and no WGSL text.

After D2, a defect between two stages is a diff, not a search.

After D6, the arithmetic that decides every buffer size in the engine has a test that runs in
milliseconds and needs no compiler.
