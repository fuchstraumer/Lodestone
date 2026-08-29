# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The style rules live in `.github/copilot-instructions.md`. Read that file first, and obey it.
This file adds code context only. It does not repeat the style rules.

Write your text in ASD-STE100 (Simplified Technical English), as the style file says.

## Build and test

The project uses CMake presets with the Ninja Multi-Config generator. Each preset builds into
`build/<presetName>`. Slang and xxHash are git submodules in `third_party/`.

```bash
git submodule update --init --recursive
```

```bash
cmake --preset ninja-msvc
```

```bash
scripts\build.bat
```

`scripts/build.bat [Debug|RelWithDebInfo] [preset]` sets the compiler environment and then builds.
**Use it rather than a bare `cmake --build`.** The build tree keeps the compiler that `CMakeCache.txt`
holds, which is Visual Studio 18 Community, toolset 14.51.36231. Another environment gives that
compiler the headers of a different toolset, `ammintrin.h` raises C4392, and every SPIRV-Tools target
fails because Slang builds them with `/WX`. The `lodestone` targets still build, so the fault stays
hidden until a change invalidates the whole graph.

The install holds both 14.51.36231 and 14.52.36615. `vcvars64.bat` selects one, and the cache then
keeps it. Do not change which one the cache holds without a reconfigure.

`scripts/configure.bat [preset]` sets the same environment and configures. Run it after a change to a
`CMakeLists.txt` that adds or removes a target. A bare `cmake --preset` writes a cache that names a
different toolset, and it leaves a build tree that no longer builds.

Presets: `ninja-msvc` and `ninja-clang-cl`. Configurations: `Debug` and `RelWithDebInfo`.
Clang-CL with the MSVC frontend variant is a hard configure error.

No build turns on a sanitizer. AddressSanitizer on Windows does not support the debug CRT. ASan
intercepts `malloc` and `free`, `ucrtbased.dll` allocates through `_malloc_dbg`, and ASan then reports
the teardown free as a bad free. The test aborts before `main`, and no frame of the stack belongs to
this repository. To turn the sanitizers on, first move every target in the process to the release CRT,
slang included.

Check the exit code of the build itself. A pipe into `tail` gives you the exit code of `tail`, and a
failed build then looks like a success.

Run every test:

```bash
scripts\run-tests.bat
```

It runs each executable directly and prints one line for each. A failing target is run a second time
with its output shown.

`ctest --test-dir build/ninja-msvc -C Debug -R SizeExpressionTest --output-on-failure` runs one test.

**Do not pipe a long run into a pager or into PowerShell `Select-Object`.** The whole stream is held
until the command ends. The run then prints nothing for minutes, and it looks like a deadlock. A test
that stops with an assertion also loses its buffered output.

Each test target is also a standalone executable. Run it directly to debug it:
`build/ninja-msvc/tests/Debug/CookTest.exe`. `CookTest` is the one that needs a command line. With no
argument it exits 1 on `NoOutputSpecified`, which reads like a failure rather than a usage error.

There is no test framework. `tests/TestHarness.hpp` gives a counter, `Check(condition, description)`,
and a nonzero exit code.

Thirteen test targets exist. Ten are unit tests, and each one proves a claim the repository makes.
None of them needs Slang, a compiler, or an asset, and all ten together run in under one second.

| Target | Proves |
|---|---|
| `SizeExpressionTest` | The size expression grammar, and every rejection it must make. |
| `ContentInternerTest` | A hash never decides equality. It supplies a hash that returns one constant, so only the byte comparison can separate the payloads. |
| `PermutationIndexTest` | A variant index is unique, dense, and stable, and a partial assignment resolves to one variant. |
| `ShaderManifestRejectTest` | The manifest reader rejects a short, misaligned, or damaged file, and opens a real one. |
| `WgslBindingScannerTest` | The cross-check reads the emitted WGSL correctly, and fails on a real mismatch. It also proves that a scoped binding states the name the emitted text must declare. |
| `ExternConstantScannerTest` | The scanner reads an `extern static const` declaration out of shader text, and refuses a use of the name as a declaration. Nothing compares this answer against a second opinion, and it decides buffer sizes. |
| `StageDumpTest` | A stage dump holds the model and no target text, it names itself the way `--dump-stage` names it, and two dumps of one input agree byte for byte. |
| `DedupeInfluenceTest` | Dedup changes what the tables cost and never what the cook measures. It builds one module in both arms and checks that the axis influence agrees, and that the measurement reads every group of variants. |
| `DiagnosticParserTest` | The parser reads Slang's machine-readable diagnostic form into a record. It names no Slang type, so it needs no compiler. |
| `ResolveStageTest` | Stage 4 resolves a hand-built `RawVariant` with no Slang present. This test is the proof that the stage 3 and stage 4 split worked, and before phase D step D5 it could not be written at all. |

An error check prints a diagnostic to `stderr` on purpose. Read the last line for the result.

The last three are different. Each one is the cooker driver, and not an assertion suite.
`tests/CMakeLists.txt` gives each a command line through `TEST_ARGS`, and all three build from
`CookTest.cpp`. Exit code 0 there is a real statement: every variant compiled, every reflection agreed
with the emitted WGSL, both round trips read back the same bytes, and two cooks agreed byte for byte.

- `CookTest` cooks `OceanFft.slang` with `--verify-deterministic`. It takes about 18 seconds, and it
  is the end-to-end coverage of the permutation path.
- `EntryPointParamsCookTest` cooks `tests/assets/EntryPointParams.slang`. Three of its four entry
  points declare a `uniform` parameter, and one of those takes a struct with an annotated field.
  Phase E step E0a needed it, and it is the acceptance test for the entry point scope walk. It cooks
  one variant in about one second.

- `ParameterBlocksCookTest` cooks `tests/assets/ParameterBlocks.slang`. It holds a block of
  resources, a block of ordinary data, a block inside a block, and a block on each of two entry
  points. Phase E step E0b needed it. It cooks one variant in about one second.

Each cook writes one shared header and one shared dedupe report, so each module cooks on its own.
One cook of all three would leave no artifact of `OceanFft` byte identical.

Add a test with `add_lodestone_unit_test(<Name> <Name>.cpp)`. Add `TEST_ARGS <args>` after the sources
when the test needs a command line.

## Running the cooker

`tools/cooker_console` is the CLI. It builds `lodestone_cooker_console`, and phase D step D1b added
it. `lodestone` itself stays a static library: the tool parses a command line, builds a
`FileOutputSink`, and calls `RunCook`. `CookTest.cpp` drives the same three calls with a command line
that `tests/CMakeLists.txt` supplies.

`ParseCommandLine` in `src/driver/CookerOptions.cpp` parses the flags, and `GetUsageText` prints them:
`--output/-o`, `--cache-dir`, `--O0` to `--O3`, `--target=<name>`, `--no-validate`, `--quiet`,
`--single-threaded`, `--no-dedupe`, `--verify-deterministic`, and `--dump-stage=<name>`.

A value flag is a row in `k_ValueFlags`, beside `k_SwitchFlags`. Add a row, not a branch.

`--target` is rejected at the command line and never in the driver, so a name that reaches
`CookerOptions` is a name `FindTargetProfile` accepts. `wgsl` is the only name this build has.

`lodestone` must stay `STATIC`. No header marks a symbol `dllexport`, so a DLL build of this target
exports nothing and every consumer fails to link. A `SHARED` build is for instrumented performance
analysis only, and it needs `WINDOWS_EXPORT_ALL_SYMBOLS` to link at all.

`tools/manifest_dump` is a real executable. It reads one `.ldshaders` manifest and writes JSON. It
links only `lodestone::client_internal`, never the cooker or Slang. That link line is the proof that
the client half stays free of the compiler.

Test shaders live in `tests/assets/`. `tests/assets/compute/Ocean/OceanFft.slang` is the reference
module, because it is the only module with a registered permutation space.

## Where the code lives

`include/` and `src/` mirror each other, and each holds six folders. **Every include is qualified**,
such as `#include "permute/PermutationSpace.hpp"`. One include directory, not six. A wrong dependency
is therefore a visible word in a diff: the day a file in `emit/` writes
`#include "compile/SlangCompiler.hpp"`, review shows it.

| Folder | Holds | Why it is one thing |
|---|---|---|
| `permute/` | `PermutationValue`, `PermutationAxis`, `PermutationAssignment`, `PermutationSpace`, `PermutationPolicy`, `PermutationRegistry`, `SizeExpression`, `ExternConstantScanner` | The authoring parameter domain. Stages 1 and 2. Phase E fills this folder. |
| `compile/` | `SlangCompiler`, `SlangDiagnosticParser`, `RawLibrary`, `Diagnostics`, and `src/compile/impl/` | **The Slang wall. No file outside this folder names a Slang type.** |
| `model/` | `ResolveStage`, `ShaderDataSchema`, `ContentHash`, `ContentInterner`, `CookedLibrary` | The data that flows, interns, and freezes. Stages 4, 6, and 7. |
| `target/` | `TargetProfile`, `WgslBindingScanner` | A target, its access model, and its validator. Phase F fills this folder. |
| `emit/` | `ShaderLibraryEmitter`, `ShaderManifestEmitter`, `OutputSink`, `StageDump`, `DedupeReport` | Everything that writes through a sink. Stage 8, plus the two reports. |
| `driver/` | `CookerDriver`, `CookerOptions` | The loop and its command line. |

`CookerErrors.hpp` sits at the top of `include/`, because every folder uses it.

`src/compile/impl/` is the one place a header sits beside its source instead of in `include/`. Those
five headers name Slang types, so `include/` must not hold them. `include/compile/SlangCompiler.hpp`
is the wall, and it names no Slang type. **A file that needs `slang.h` belongs in `src/compile/impl/`
and nowhere else.**

| File | Owns | Lifetime |
|---|---|---|
| `SlangCompilerTypes` | The enum conversions, `BindingScope`, `RawBindingDraft`, the option table. | Free functions. |
| `SlangModuleContext` | One global session, one session, the root module, the entry points, the source texts. | One for each thread. |
| `SlangVariantCompiler` | Link, target codegen, entry point metadata. Returns `LinkedVariant`. | One for each job. |
| `SlangReflector` | Every reflection walk. Takes `LinkedVariant`, returns `RawVariant`. | One for each job. |
| `ThreadPool` | An atomic job index and a latch. Spreads variants across workers. | One for each compiler. |

`client/include/` is not part of this split. It is the contract a renderer links against, and it holds
four headers and nothing else.

There is no `validate/` folder, and that is worth knowing. Three of the four validators are functions
inside the file they check: `VerifyLibraryRoundTrip` and `VerifyLayoutRoundTrip` live in
`src/driver/CookerDriver.cpp`, and `VerifyManifestRoundTrip` lives in
`src/emit/ShaderManifestEmitter.cpp`. Only `WgslBindingScanner` is its own file. The tree cannot show
that validators rank beside stages, and no arrangement of the current files would.

## The two problem domains

The repository serves two domains, and the whole design follows from the split.

- The authoring domain wants flexible shaders, readable parameters, and fast edits.
- The runtime domain wants small data, fast loads, no main-thread stalls, and no undefined behavior.

The cooker is the compiler between them. `client/include/ShaderLibraryTypes.hpp` is the contract.
A renderer links against that header, and against nothing else in this repository. A new shader
therefore does not force the renderer to rebuild.

Do not put a Slang type, a WebGPU type, or a `std::filesystem` type into the client headers or into
`model/ShaderDataSchema.hpp`. A compiler type that leaks into the schema becomes a dependency the engine
must carry forever.

## How this repository thinks

Read this before you propose a design. These are the author's positions, and most review comments that
land badly are comments that argue against one of them without knowing it was a position.

**A pipeline of data transforms, not a program that does a job.** The cooker is a compiler between two
problem domains. Each stage has an input type, an output type, and one responsibility. This came from a
data-pipeline engineer's rebuild of a monolithic geometry importer, and it is the founding idea of the
repository. A change that fuses two stages is a regression even when it is shorter.

**Correctness is proved by comparison, never by inspection.** The repository does not assert that dedup
is correct. It replays every variant through the finished tables and compares against the text the
compiler produced. It does not assert that reflection is correct. It reads the emitted WGSL back and
compares. It does not assert that a cook is deterministic. It cooks twice and compares bytes. When you
add a capability, ask what it is compared against. If the answer is "nothing", that is the design
problem, not a testing gap.

A validator and a defensive check look alike, and only one of them may go. A **validator** compares two
answers that were derived independently, so it can find a defect that neither side could find alone.
Those are the four named above, they are internal, they look redundant, and they stay. A **defensive
check** re-tests an invariant that this code already established, such as a bounds test on an index
this code wrote into a table. Those hide defects as ordinary return values, and the repository is
removing them. Validate at the ingestion surface, then trust the data inside.

**Prefer the design where the wrong thing cannot be written down.** `Active` and `Canonical` are two
fields so that canonicalization cannot reach the linker. The frozen model exists so that an emitter
cannot reach a Slang type. Phase E moves axis declarations into the shader so that an axis name cannot
drift from the constant it drives. Given a choice between a check and a structure that needs no check,
take the structure.

**Two independent implementations that agree beat one implementation trusted twice.** The WGSL scanner
is not a better reflection API. It is a second opinion, and the asymmetry rule states which opinion
wins on which question. Look for that shape when adding a verification.

**Make the expensive thing a number somebody reads.** The axis influence matrix exists because a
combinatorial explosion used to show up as a slow build months later. Now it fails the cook, on the
commit that caused it, naming the axis and the entry point. Cost that nobody can see gets paid forever.

**The escape hatch stays working.** `--no-dedupe` is not a debug flag. It is the control arm, and both
arms must emit valid output. A mechanism with no way to turn it off is a mechanism nobody can debug.

**A third-party type never rises above the layer that needs it.** `compile/SlangCompiler.hpp` names no Slang
type. `manifest_dump` links only the client half, and that link line is the proof the client half stays
free of the compiler. When a dependency arrives, it goes behind a facade in one translation unit.

**Author for people, cook for machines.** Authoring wants flexible shaders and fast edits. Runtime
wants small data and no stalls. The reason to keep them apart may be elegant: but it also stops a
performance engineer and a content author from having to win the same argument in a planning meeting.
Policy that a tech artist owns belongs in a data file. Structure that an engineer owns belongs in code.
Each gets to accomplish their work and solve the problems they're tasked with as they see fit: this
pipeline transforms between those domains to maintain that capability. Software as a people management
tool.

**Use the invariant the data already has**. CanonicalizeAssignment walks the space in order, so slot k
of Canonical is always axis k. FindBinding searched it anyway. The search cost O(K) where O(1) was available,
and that was the small part. The larger part is that a search states there is no order, so every later
reader believed there was none. When you add a helper, name the invariant it relies on. If it relies on
none, find out whether the data really has none.

**A nested loop over one container is usually a missing key.** InfluenceOfAxis tested every pair of variants
to find the pairs that differ in one axis. That is an equivalence relation, and a key finds every class in
one pass. Before you write a loop inside a loop over the same data, ask which key makes the inner loop unnecessary.

### Working with this author

The author is a woman. Use she and her. Do not fall back to the neutral default.

Blunt and specific beats reassuring. If a plan is wrong, say which part and why. If a claim needs
checking, check it rather than hedging — the submodule under `third_party/` is the ground truth for any
question about Slang, not general knowledge. Name techniques by their formal names when they have them;
the author reaches many of them by instinct and finds the vocabulary useful. Do not credit the author
with a design decision without evidence that it was hers.

The author reserves implementation work she finds enjoyable. When she says she wants to write
something, plan it and explain it, and do not write the code for her.

When she proposes an optimization, establish that the cost exists before helping her implement it.
Estimate the magnitude, say so plainly if it is negligible, and redirect to where the real cost is.
She would rather be told an idea is aimed at nothing than be helped to build it.

## Data flow, one cook

`RunCook` in `src/driver/CookerDriver.cpp` is the whole loop. Read that file first. It calls each stage in
order, for each module path.

**A stage transforms. A validator compares.** The pipeline has eight numbered stages, and each one
takes an input type and gives an output type. A validator reads the output of a stage, compares it
against a second opinion, and changes nothing. A validator gets a name and no number, because a
number would state that every target must supply one. A target supplies a validator only when it can.

1. **Declare.** `FindPermutationSpaceForModule(name)` finds the module's axes.
   `space.VerifyAxisNamesAreDeclared` checks each axis name against the `extern static const`
   declarations in the Slang source texts, through `ExternConstantScanner`. A module with no
   registered space gets an empty space and one variant.
2. **Enumerate.** `space.EnumerateVariants()` expands the space into a `VariantSet` of
   `VariantDescriptor` values. Each descriptor holds `Active` and `Canonical` (see below) and a dense
   index.
3. **Compile.** Stage 3 walks three scopes, and each one needs a different call: the global scope,
   the parameter scope of each entry point, and the element of each `ParameterBlock`. Slang describes
   a block with descriptor ranges alone, so the binding range of a block reports a descriptor set
   index of -1 and the range walk drops it. `CollectSubObjectDrafts` keeps exactly the ranges the
   range walk dropped, so the two walks partition the ranges and no range is drafted twice.

   Stage 3 has two entry points. `SlangCompiler::PrepareRawModule` runs once for each
   module and returns the module facts only Slang can supply: the name, the entry point names, and
   the declared default of every `extern static const` constant no axis drives. A size expression may
   name one of those defaults, so it must run before the first variant.
   `SlangCompiler::Compile` then runs once for the whole variant set, and returns one
   `CookResult<RawVariant>` for each variant. A `ThreadPool` spreads the variants across workers.
   Each worker holds one `SlangModuleContext`, and each job runs a `SlangVariantCompiler` and then a
   `SlangReflector`. Output is `RawVariant`. Every `[vx_*]` argument comes back as the string the
   author wrote, because evaluating one is stage 4's job.
4. **Resolve.** `ResolveVariant` in `model/ResolveStage.cpp` reads the `[vx_*]` attributes and evaluates each
   size expression against a `ResolveContext`. Output is `CompiledVariant`.
   **No file under `src/compile/` names a size expression or evaluates a `[vx_*]` attribute, and
   `model/ResolveStage.cpp` names no Slang type.** Phase D step D5 made that true, and it is what lets a second target language
   exist. Do not undo it.
5. **Normalize.** Empty on purpose. The dedup report says `normalization passes active: (none)`. A
   whitespace pass with no stage boundary hides the difference between a true collapse and an effect
   of the stripping.
6. **Intern.** `AppendVariantToModule` gives each source, each resource, each resource list, each
   footprint list, each visibility list, and each raster state to a `ContentInterner`. The six
   interners live on `InternedModule`, which is the stage 6 boundary type. `FreezeModuleTables` takes
   that builder by value and returns a `CookedModule`, so nothing after the freeze can reach an
   interner.
7. **Key.** Each `LibraryVariant` holds only indices into those tables, plus its dense index.
8. **Emit.** `EmitLibraryArtifacts` writes through the `OutputSink`:
   - the C++ header, `sink.PrimaryName()`, from `EmitShaderLibraryHeader`
   - one C++ source for each module, `<headerStem>_<Module>.cpp`
   - one binary manifest for each module, `<Module>.ldshaders`, plus `VerifyManifestRoundTrip`
   - `ShaderLibrary.dedupe.txt`, from `GenerateDedupeReport`

Four validators run inside that loop. None of them is a stage.

- **The reflection cross-check**, after stage 4. `ValidateResolvedLibrary` asks the target profile for
  a validator. The WGSL profile scans `@group`/`@binding` back out of the emitted text with
  `WgslBindingScanner`, then compares that against the bindings the entry point uses. A mismatch
  increments a counter, and a nonzero counter fails the cook with `CookError::ReflectionMismatch`. A
  target that supplies no validator skips this, and the cook says which of the three happened.
- **The library round trip**, after stage 7. `VerifyLibraryRoundTrip` replays every variant through
  the finished tables and compares the result against the text the compiler produced. It covers the
  **source text** only.
- **The layout round trip**, after stage 7. `VerifyLayoutRoundTrip` replays every (variant, entry
  point) through the resource, footprint, and visibility tables and compares the result against
  `BuildEntryPointLayout`. Phase D step D8b built it, because the step that collapses the layout
  tables owes the repository a check that a collapse was correct. `EnforceModulePolicy` then checks
  the measured axis influence against `ModulePolicy`.
- **The manifest round trip**, inside stage 8. `VerifyManifestRoundTrip` reads each manifest back and
  compares it against the module it came from. `CheckManifestLayout` walks the manifest the way a
  consumer walks it, from variant to resource list to footprint list to visibility list.

`--verify-deterministic` runs stages 1 to 8 twice into two `MemoryOutputSink` objects, compares every
artifact byte for byte, then writes the first result to the real sink. A difference means an
unordered container reached the output.

## Major types

| Type | Header | Role |
|---|---|---|
| `CookerOptions` | `driver/CookerOptions.hpp` | Every knob one cook has. `ParseCommandLine` fills it. |
| `OutputSink` | `emit/OutputSink.hpp` | Where artifacts go. `FileOutputSink` and `MemoryOutputSink`. The seam a live cooker will use. |
| `PermutationValue` | `permute/PermutationValue.hpp` | One axis value: bool, uint32, or int32. A tagged union, and not a `std::variant`, so no header downstream carries the template. |
| `PermutationAxis` | `permute/PermutationAxis.hpp` | One axis of variation. Values are stored in place, capped at `k_MaxValues`. `ParentIndex` names the parent axis inside the owning space. |
| `PermutationSpace` | `permute/PermutationSpace.hpp` | A named, ordered set of axes, and every question you ask about them. It **owns** its axes, so it cannot be copied. |
| `PermutationBinding`, `PermutationAssignment` | `permute/PermutationAssignment.hpp` | One axis bound to one value, and a list of them. |
| `VariantDescriptor` | `permute/PermutationAssignment.hpp` | One variant identity. `Active` and `Canonical`. |
| `CanonicalAssignment` | `permute/PermutationAssignment.hpp` | An assignment that holds every axis of one space. Only `PermutationSpace::CanonicalizeAssignment` builds one, so `ComputeVariantIndex` cannot be given a partial assignment. |
| `ModulePolicy` | `permute/PermutationPolicy.hpp` | A variant budget, plus the axis influence the author expects. |
| `ExternConstantDeclaration` | `permute/ExternConstantScanner.hpp` | One `extern static const` line read out of shader text. |
| `CompiledVariant`, `CompiledEntryPoint` | `model/ShaderDataSchema.hpp` | Compiler output: WGSL text plus reflection. Owns its strings. |
| `ReflectedBinding` | `model/ShaderDataSchema.hpp` | What the shader states about one resource. The CPU side never writes any of it. `Name` and `ScopeName` together are the identity: two entry points can each declare `albedoMap`. |
| `RawVariant`, `RawModule` | `compile/RawLibrary.hpp` | Stage 3 output. Everything Slang says, with no opinion about any of it. A `[vx_*]` argument is still the string the author wrote. |
| `ResolveContext` | `model/ResolveStage.hpp` | Stage 4 input. The axis values of one variant, plus the extern constant defaults that stage 3 carried out. |
| `TargetProfile` | `target/TargetProfile.hpp` | A target name, an access model, and an optional validator. `--target` names one. |
| `DiagnosticRecord`, `DiagnosticSink` | `compile/Diagnostics.hpp` | One compiler message as a record, and where it goes. Compilation never formats a string. |
| `ContentInterner<T>` | `model/ContentInterner.hpp` | Collapses equal payloads, keeps provenance, counts collisions. |
| `InternedModule` | `model/CookedLibrary.hpp` | The stage 6 builder. It holds the six interners, and it is the only place the provenance of a collapse survives. |
| `CookedModule`, `CookedLibrary` | `model/CookedLibrary.hpp` | The frozen model. Every emitter reads this and nothing earlier. |
| `ShaderManifestView` | `client/include/ShaderManifest.hpp` | Read-only spans over the manifest bytes. Allocates nothing to open. |
| `ShaderSourceProvider` | `client/include/ShaderLibraryTypes.hpp` | Where a renderer gets source, bindings, and workgroup size. `Generation()` is the hot-reload hook. |

`ContentHashValue` is xxHash3, 64 bit. `model/ContentHash.hpp` holds the streaming form as well, which the
composite keys use. The hash name reaches the output, so a new hash needs a new name, and
`k_HashName` is the only place that name is written. Every interner takes it from there. The name was
once spelled out a second time in `model/CookedLibrary.hpp`, the two copies drifted, and the dump then
reported a name that no constant held.

## Rules the code depends on

Break one of these and the cook can exit 0 with wrong content.

1. **A hash never decides equality.** It picks a bucket. A byte comparison decides. The interner
   counts each collision and reports it. It never resolves one in silence.
2. **`--no-dedupe` must stay correct.** It is the A/B control arm. Both modes must emit valid output,
   and both must reach the same conclusions about the shader. So a measurement of the content reads
   the content, never an interner index. An index is what the interner assigned, and `--no-dedupe`
   gives every artifact its own. `DedupeInfluenceTest` holds this line.
3. **The round trip checks are never optional.** `VerifyLibraryRoundTrip`, `VerifyLayoutRoundTrip`,
   and `VerifyManifestRoundTrip` run on every cook. Know what each one covers, because it is less than
   the names suggest. `VerifyLibraryRoundTrip` replays the **source text** only.
   `VerifyLayoutRoundTrip` replays the layout, and phase D step D8b built it for exactly this reason:
   the four binding tables collapse hard, and a bad collapse has to be found by comparison rather than
   trusted. `CheckManifestLayout` walks the manifest the way a consumer walks it, so it proves the
   serialization is faithful. Every field a consumer reads must still take part in a key, and a field
   cannot leave a key while it stays in the type. See `docs/phase-d-stage-separation-plan.md` §4b.
4. **The emitted artifact decides group and binding numbers. Reflection decides sizes and types.**
   That asymmetry is the only reason the cross-check finds real errors. A **name** is neither: the
   cross-check builds the name it expects from `ScopeName` and `Name`, and compares that against the
   de-mangled emitted identifier. It states what it expects rather than removing a prefix it does not
   recognise.
5. **Every emitter reads one frozen model.** An emitter must not reach past `CookedLibrary` into
   `CompiledVariant` or into Slang.
6. **An axis name must match the Slang `extern static const` name exactly.** A mismatch links a
   symbol that nobody references, leaves the shader on its default, and fails nowhere.
   `VerifyAxisNamesAreDeclared` exists for this reason.

### `Active` and `Canonical`

`Active` holds only the axes this variant really uses. It drives the Slang linker, so it decides the
emitted text. `Canonical` holds every axis in declaration order, with the first value filled in for a
disabled axis. It drives the dense mixed-radix index only. Canonicalization therefore can never
change shader output, and a caller can find a variant with a partial set of values.

`Canonical` has its own type, `CanonicalAssignment`, and only `PermutationSpace::CanonicalizeAssignment`
builds one.
`PermutationAssignment` named four different things: an active assignment, a partial one, a canonical
one, and the parameter of every function that takes any of them. `ComputeVariantIndex` needs the
canonical form, and it stated that in a parameter name alone. A partial assignment now fails to
compile. The type holds the same vector and costs nothing at run time.

`SpaceSize` counts the dense index range with the holes included. A disabled dependent axis leaves
gaps, and the design accepts them.

### Size expressions

A size travels as a string, and this is not a style choice. Slang folds an attribute integer argument
at compile time, but the permutation constants are `extern static const` and fold at link time.
`[vx_element_count(IFFT_SIZE * 4)]` therefore fails to compile. A string argument reaches reflection
untouched, and `EvaluateSizeExpression` does the arithmetic once for each variant.

The attribute declarations are in `tests/assets/LodestoneAttributes.slang`: `vx_element_count`,
`vx_extent_2d`, `vx_extent_3d`. Slang has no optional attribute parameters, so each arity needs its
own name. The README shows `lodestone_element_count`; the code says `vx_element_count`.

## Where to register a module

`FindPermutationSpaceForModule` and `FindPolicyForModule` read a static table, `k_ModuleSpaces`, in
`src/permute/PermutationRegistry.cpp`. Add an axis, a space, a policy, and a table row there. Only
`OceanFft` has an entry today. That file holds nothing else, so phase E step E6 deletes it whole.

An axis names its parent by index into the space that owns it, and not by pointer. A
`PermutationSpace` owns its axes by value, so a copy would leave every `PermutationBinding` of the
original aimed at a different object, and a child axis would then read as absent and quietly reduce
the variant count. Copying a space is deleted for that reason. Moving one is safe, because moving the
vector keeps the axis addresses.

This table is compiled in. Replacing it is phase E, and
`docs/phase-e-data-driven-permutations.md` plans it: the axis moves onto the `extern static const`
declaration as an attribute, and the policy moves into a data file a tech artist owns. Do not start
that work as a side effect of another task.

## Two output forms, one model

The generated C++ and the binary manifest carry the same tables from the same `CookedModule`.

- The C++ form compiles into the program. The header holds identity and lookup only, never shader
  text, so naming a shader does not rebuild when the text changes.
- The manifest form arrives as bytes. Every cross-reference is a `uint32` index, never a pointer, so
  the reader is a set of spans and it relocates nothing. Sections start on 8-byte boundaries. A record must be
  trivially copyable, and `k_IsManifestRecord` holds that line. Record **sizes** are not pinned, and
  `ShaderManifest.hpp` says so: the format has no version migration yet, so a record can still grow.

`todo.md` states the author's plan to delete the C++ emitter once the manifest path is complete. Do
not start that removal without a request.

## The velox rename

This repository was extracted from an engine named `velox`. The C++ rename to `lodestone` is
complete. No `velox` name remains in `src/`, `include/`, `client/`, `tests/`, or `tools/`.

The shader side keeps the old prefix on purpose. `tests/assets/LodestoneAttributes.slang` declares
`module VeloxAttributes` and the `vx_element_count`, `vx_extent_2d`, and `vx_extent_3d` attributes.
Those names are part of the shader-side contract, and `src/compile/impl/SlangReflector.cpp` reads
them by string. A rename there touches every test shader, so treat it as its own task.

## Documents

**`docs/` is in `.gitignore`.** A document survives only when somebody force-adds it with
`git add -f`. Run `git ls-files docs/` to see which ones did. Every other document exists on disk and
in no commit.

### Background — what the repository is

- `docs/agent-handoff.md` — **read this first.** State, the build faults that are open, the measured
  Slang facts, and the next task. It is newer than this file on anything that moves.
- `README.md` — the author's intent. It mixes shipped features with planned ones. Check the code
  before you trust a feature claim there.

**Check that a document exists before you cite it.** This list has named files that were deleted or
renamed. `ls docs/` is the only reliable answer.

### Planned work — where the repository is going

Phases D, E, and F are a **sequence**. Each depends on the one before it. The tooling track runs
beside them.

- `docs/phase-d-stage-separation-plan.md` — separate stage 3 from stage 4, and add `--dump-stage`.
  **Complete.** Read it for the reasoning behind the shape of the pipeline, and for §4b, which states
  why a binding record is four tables and not one.
- `docs/phase-e-data-driven-permutations.md` — axis declarations in the shader, policy in a data file,
  constraint expressions, and a ranking index in place of mixed radix. **This is the current work.**
  The compiler split it waited on is complete. Steps E0a and E0b are done. §1e holds step E0c, which
  is a defect rather than a missing capability, and it comes first. `docs/agent-handoff.md` §8 gives
  the order.
- `docs/phase-f-vocabulary.md` — **read this before proposing anything about targets or bindings.**
  It defines axis kind, binding time, and access model, and it divides the work between Slang's
  capability system and this repository. It is a vocabulary, not a plan.

Two terms from phase F are worth carrying into any discussion of variants:

- **Axis kind** — resource presence, capability, tuning, or technique. Each kind gets a different
  mechanism, and only resource presence collapses under a bindless or pointer access model.
- **Binding time** — cook, pipeline, draw, or thread. The author declares the *earliest sound* binding
  time for an axis. A target profile decides how late the value really binds.

The dividing rule between Slang's job and this repository's job: **a capability that varies between
targets belongs to Slang. A capability that varies between devices inside one target belongs to
Lodestone.** Slang's capability atoms say what a target language can express, never what a device
supports.

### The stage numbering

Phase D step D0 settled this: **eight numbered stages, each one a transformation, and each validator
gets a name and no number.** See the data flow section above.

An older document calls the phases "tiers". They are the same thing.
