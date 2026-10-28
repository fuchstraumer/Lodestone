# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

The style rules live in `.github/copilot-instructions.md`. Read that file first, and obey it.
This file adds code context only. It does not repeat the style rules.

Write your text in ASD-STE100 (Simplified Technical English), as the style file says.

## Build and test

The project uses CMake presets with the Ninja Multi-Config generator. Each preset builds into
`build/<presetName>`. Slang, xxHash, toml++, and Dawn are git submodules in `third_party/`.

```bash
git submodule update --init third_party/xxHash third_party/tomlplusplus third_party/dawn
```

```bash
git submodule update --init --recursive third_party/slang
```

Do not run `git submodule update --init --recursive` on the whole tree. It recurses into Dawn's own
submodules, which are Chromium dependencies for Dawn's GN build. Measured on 2026-10-19: four of them
(`buildtools`, `testing`, `third_party/android_build_tools`, `third_party/android_sdk`) had an empty index and
an empty worktree, so `third_party/dawn` showed as modified in every `git status`. The CMake build does not use
them: `DAWN_FETCH_DEPENDENCIES` fetches the list in Dawn's `tools/fetch_dawn_dependencies.py`
(`required_submodules`) at configure time. To clear that state, run
`git submodule deinit -f <path>` for each of them inside `third_party/dawn`.

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

`LODESTONE_BUILD_COOKER` (default `NOT EMSCRIPTEN`) selects the cooker half. With it OFF, the build adds
the client library, the JSON writer, `tools/suggest`, and `manifest_dump`, and nothing more: no Vulkan SDK,
no Slang, no Tint. `cmake/LodestoneCooker.cmake` holds the cooker half, and the root file includes it only
when the option is ON. Every test links the cooker, so `LODESTONE_BUILD_TESTS` follows the option, and
tests ON with the cooker OFF fail the configure.

`lodestone_add_shader_cook(<name> OUTPUT_DIR <dir> TARGETS <profile>... MODULES <file>... [POLICY_FILE <file>]
[OPTIONS <arg>...])` (in `cmake/LodestoneShaderCook.cmake`) runs one cook as a build step, with `--depfile`, so
an edit to any file a module reads re-cooks. It runs the `lodestone_cooker_console` target, or the host cooker
that `LODESTONE_COOKER_EXECUTABLE` names. A build with no cooker (Emscripten) must set that variable. The
target property `LODESTONE_MANIFEST` gives the manifest path. The console builds whenever the cooker does. The preset `ninja-msvc-client` turns the cooker off:
`scripts\configure.bat ninja-msvc-client`, then `scripts\build.bat RelWithDebInfo ninja-msvc-client`.

`LODESTONE_ISOLATE_SLANG` (default ON) builds Slang and SPIRV-Tools in their own build trees under
`build/<preset>/_isolated/`, through `ExternalProject` (`cmake/IsolatedSlang.cmake`). No flag or target of this project
reaches those trees, so a change here does not rebuild Slang. Slang builds Release with LTO for every configuration:
its API is COM-style interfaces, so a Debug `lodestone` loads a Release `slang-compiler.dll`. SPIRV-Tools builds
static, in the configuration of this project (it shares the CRT), with no mimalloc and no tools. Slang's mimalloc stays
inside its DLL, so a parent project can link its own mimalloc. `ConfigureSlang.cmake` holds the Slang options for both
paths: `LODESTONE_SLANG_FORWARDED_OPTIONS` names each one that the isolated build passes on. With the option OFF, the
old `add_subdirectory` build runs. To rebuild Slang from nothing, delete `build/<preset>/_isolated/slang-build`.

`LODESTONE_ENABLE_WGSL` (default ON) pulls in Tint from the Dawn submodule at `third_party/dawn`,
configured by `cmake/ConfigureTint.cmake`. The WGSL validator parses the emitted text with Tint, so a
standalone configure needs Python and network access: `DAWN_FETCH_DEPENDENCIES` is ON, and Dawn fetches
its own dependencies at configure time. A parent project that already builds Dawn (defines `tint_api`)
shares that target rather than adding it twice. With the option OFF, Tint and `WgslValidator` leave the
build, and the `wgsl` profile has no validator. A wgsl cook in that build then fails with
`TargetValidatorUnavailable` unless the command line passes `--no-validate`. No WGSL leaves a cook
without a second opinion unless somebody asked for that.

No build turns on a sanitizer. AddressSanitizer on Windows does not support the debug CRT. ASan
intercepts `malloc` and `free`, `ucrtbased.dll` allocates through `_malloc_dbg`, and ASan then reports
the teardown free as a bad free. The test aborts before `main`, and no frame of the stack belongs to
this repository. To turn the sanitizers on, first move every target in the process to the release CRT,
slang included.

Check the exit code of the build itself. A pipe into `tail` gives you the exit code of `tail`, and a
failed build then looks like a success.

Run every test:

```bash
scripts\run-tests.bat RelWithDebInfo
```

The configuration argument defaults to `RelWithDebInfo`. On 2026-10-16 an old Debug tree (Slang `6eb89786c`, IR module
version 32) shared one module cache folder with a RelWithDebInfo tree that read version 31 only, and one Debug run
broke every later cook (E00131, 0xC0000409, or a hang). Two things now prevent that: the cache folder name holds the
configuration, and with `LODESTONE_ISOLATE_SLANG` both configurations load one Slang build. The Debug tree was rebuilt
on 2026-10-23, and 43 of 43 Debug tests pass. Run the Debug tests after a change to a fixture or to code that indexes a
table: the debug STL checks each subscript, and RelWithDebInfo checks none.
Rename the cache folder after any run of a binary built from another Slang.

It runs each executable directly and prints one line for each. A failing target is run a second time
with its output shown. The script compares each exit code with 0. A crash exits with a negative code
(0xC0000409 from `std::terminate`), and the old check `if errorlevel 1` reported such a crash as a pass.

`ctest --test-dir build/ninja-msvc -C Debug -R AttributeExpressionTest --output-on-failure` runs one test.

**Do not pipe a long run into a pager or into PowerShell `Select-Object`.** The whole stream is held
until the command ends. The run then prints nothing for minutes, and it looks like a deadlock. A test
that stops with an assertion also loses its buffered output.

Each test target is also a standalone executable. Run it directly to debug it:
`build/ninja-msvc/tests/Debug/ManifestIndexTest.exe`. A cook test needs a command line. With no argument
it exits 1 on `NoOutputSpecified`, which reads like a failure rather than a usage error.

There is no test framework. `tests/TestHarness.hpp` gives a counter, `Check(condition, claim)`, `CheckEqual`,
`CheckOk`, and `CheckError`, and a nonzero exit code. A failed `CheckEqual` prints both values, and a failed `CheckOk`
prints the error. `tests/TestFormat.hpp` formats any value for that message: an enum by its `ToString` or its
`magic_enum` name (a flag set by its bits), and ranges, optionals, variants, tuples, and plain aggregates by their
contents. `Check` returns its condition, so the check can guard the checks that depend on it. A section that runs no
check fails, so a guard that fails cannot hide the checks behind it. To read the diagnostics of a cook, pass a
`RecordingDiagnosticSink` to the three-argument `RunCook`.

Forty-nine test targets exist: forty-two unit tests and seven cook tests. `tests/README.md` lists what each one proves. Read it before you add or change a test, and add a row there for each new target.

Each cook test writes into its own output directory. The bundle and the dedup report hold every module
of one cook, so a module added to a cook changes those files.

Add a test with `add_lodestone_unit_test(<Name> <Name>.cpp)`. Add `TEST_ARGS <args>` after the sources
when the test needs a command line. A cook test also needs a line in `scripts\run-tests.bat`: the
script skips the cook tests in its glob and re-runs each with explicit arguments, so add the new name to
that skip list and give it its own explicit block.

A test that builds a `CookedModule` by hand uses `tests/ManifestFixtures.hpp`. `MakeFixtureVariant` sets every
per-entry-point vector of a `LibraryVariant` from one `FixtureInstance` for each entry point, so a new
per-entry-point field changes that file and no fixture. Before it existed, each such field broke every hand-built
fixture at once.

## Running the cooker

`tools/cooker_console` is the CLI. It builds `ls_cooker_console`, and phase D step D1b added
it. `lodestone` itself stays a static library: the tool parses a command line, builds a
`FileOutputSink`, and calls `RunCook`. `CookTest.cpp` drives the same three calls with a command line
that `tests/CMakeLists.txt` supplies. `RunCook` has a second form that takes a `DiagnosticSink&`, so an embedding host
or a test receives every record of the cook instead of the console text. The two-argument form reports to stderr.
Every message of a cook reaches the sink. Only `ParseCommandLine`, which runs before a sink exists, prints to stderr
itself. A function in the cooker library that can report takes a `DiagnosticSink&` and does not call `std::println`.

`ParseCommandLine` in `src/driver/CookerOptions.cpp` parses the flags, and `GetUsageText` prints them:
`--output/-o`, `--cache-dir`, `--include-dir/-I <path>` (repeatable),
`--O0` to `--O3`, `--g0` to `--g3` (Slang's debug information level), `--target=<name>`, `--no-validate`, `--quiet`,
`--single-threaded`, `--no-dedupe`, `--verify-deterministic`, `--dump-stage=<name>`, `--dump-sources`,
and `--depfile`. `--depfile` writes `ShaderLibrary.d`, a Makefile-form depfile that names every file Slang
read, `import` and `__include` included. `--dump-sources` writes every unique compiled source to a subfolder
`<module>_<target>_sources`, beside a `SourceTable.json` that names each variant by its axis description
and its source hashes. Each file holds the target code only (`.wgsl` or `.spv`), so Tint, `spirv-dis`,
and `spirv-val` read it directly. The output sink creates the subfolder.

A value flag is a row in `k_ValueFlags`, beside `k_SwitchFlags`. Add a row, not a branch.

A module finds an imported module in its own folder, in that folder's parent, in each `--include-dir` in the order given,
and in the module cache, in that order. The cache comes last on purpose. A module that only the cache can find loads in
the first session and fails in the worker sessions, with no reason from Slang (the 2026-10-22 DiamondDogs report C13), so
the priming error names `--include-dir`. `lodestone_add_shader_cook` takes `INCLUDE_DIRS <dir>...`.

`--target` is required, like `-o` and a module path. A cook with no target cooks nothing, so
`ParseCommandLine` fails with `NoTargetSpecified`. Repeat the flag for more than one target. An unknown
name is rejected at the command line and never in the driver, so a name that reaches `CookerOptions` is
a name `FindTargetProfile` accepts. This build has `wgsl` and `spirv`. Each has a validator:
`WgslValidator` (Tint) and `SpvValidator` (SPIRV-Tools). A third profile, `spirv-modern` (buffers through buffer device
addresses, textures and samplers through descriptor handles, phase F step F4), exists and cooks. A profile has one access
model for each class of shim resource (`TargetProfile::Forms`, a
`ShimForms`), and `ShimLinkSource` links each extern in the form of its class. `spirv-modern` gives buffers the Pointer
form and textures, samplers, and storage textures the Handle form, with the descriptor heap in set 3
(`TargetProfile::DescriptorHeapSet`, given to Slang as its bindless space index). The manifest records the form of each
class (`Profile::Forms`, format 17), and the heap (`Profile::HeapLowering` and `Profile::DescriptorHeapSet`, format 18). A
name in the policy file can name it.

A Pointer or Handle member stores a value in a block: `[ls_shim_value]` marks it in `builtins/LodestoneShims.slang`, because
reflection gives a descriptor handle as a plain `uint2` and a pointer with no resource type. The reflector finds the marked
member in the ordinary data of a parameter block, a `ConstantBuffer`, or the entry point constants, and writes one binding
with a `StoredValuePlacement` (the form, the container, the container's group and binding, and the byte offset). The shim
table row states the resource (`ShimInterface::Resource`); a storage handle takes its format from the implementation's type
argument, and a pointer its element stride from the pointee. The access in a `Ptr` type's full name must equal the row's
(`ShimPointerAccessMismatch`). An array of Pointer or Handle shims, and one in a storage buffer element, fail with
`ShimStoredValueUnsupported`. A stored value is visible to an entry point when its container is: a used uniform buffer, or the
entry point's own push constants. `ResolvedLibraryValidator::ValidateEntryPoint` gives a validator the bound bindings,
sorted, and the stored values apart. `SpvValidator` compares the descriptor heap set with the mutable descriptor lowering
(`BindingMismatchKind::DescriptorHeap`): unbounded arrays, samplers at binding 0, combined image samplers at 1, and every
other resource at 2, and no reflected resource in that set. It finds the member of each stored value's container that starts
at its offset, and that member must have the value's name and type: a `uint2` for Handle, a `PhysicalStorageBuffer` pointer
for Pointer (`BindingMismatchKind::StoredValue`, F4.5). A sampler pair through the heap is traced from `OpSampledImage` back to
the two handles that index the heap (`SpvModuleReader`, `TraceHeapHandle`), so a pair names two stored values as reflection
does. A trace that leaves the known shape fails the cook (`ReflectionSamplerPairUnresolved`).

`lodestone` must stay `STATIC`. No header marks a symbol `dllexport`, so a DLL build of this target
exports nothing and every consumer fails to link. A `SHARED` build is for instrumented performance
analysis only, and it needs `WINDOWS_EXPORT_ALL_SYMBOLS` to link at all.

`tools/manifest_dump` is a real executable. It reads one `ShaderLibrary.ldmanifest` bundle and writes JSON. It
links only `lodestone::client_internal`, never the cooker or Slang. That link line is the proof that
the client half stays free of the compiler.

Test shaders live in `tests/assets/`. The KitchenSink set in `tests/assets/KitchenSink/` is the
reference. `scripts/check-known-good.py` cooks each KitchenSink module on its own, once for each target,
and compares its six stage dumps against `tests/known_good/`. A dump is named
`<module>_<target>_<Stage>.json`. The `wgsl` and `spirv` dumps of one module differ only in the code
length and the code hash. The reflection is the same for both targets, except for entry point constants: their
layout and delivery follow the target, and no KitchenSink module declares any.

## Where the code lives

`include/` and `src/` mirror each other, and each holds six folders. **Every include is qualified**,
such as `#include "permute/PermutationSpace.hpp"`. One include directory, not six. A wrong dependency
is therefore a visible word in a diff: the day a file in `emit/` writes
`#include "compile/SlangCompiler.hpp"`, review shows it.

| Folder | Holds | Why it is one thing |
|---|---|---|
| `permute/` | `PermutationValue`, `PermutationAxis`, `PermutationAssignment`, `PermutationSpace`, `PolicyDocument`, `AttributeExpression` | The authoring parameter domain. Stages 1 and 2. Phase E filled this folder. |
| `compile/` | `SlangCompiler`, `SlangDiagnosticParser`, `RawLibrary`, `ShimLinkSource`, `Diagnostics`, and `src/compile/impl/` | **The Slang wall. No file outside this folder names a Slang type.** |
| `model/` | `ResolveStage`, `ShaderDataSchema`, `ContentHash`, `ContentInterner`, `CookedLibrary`, `AxisInfluence` | The data that flows, interns, and freezes. Stages 4, 6, and 7. |
| `target/` | `TargetProfile`, `ShimTable`, `StorageFormatTable`, `TargetUtils`, `WgslValidator`, `SpvValidator` | A target, its access model, and its validator. Phase F fills this folder. |
| `emit/` | `ShaderLibraryEmitter`, `ShaderManifestEmitter`, `OutputSink`, `StageDump`, `DedupeReport` | Everything that writes through a sink. Stage 8, plus the two reports. |
| `driver/` | `CookerDriver`, `CookerOptions` | The loop and its command line. |

`CookerErrors.hpp` sits at the top of `include/`, because every folder uses it.

`src/compile/impl/` and `src/target/impl/` are the places where a header sits beside its source instead of
in `include/`. Those headers name third-party types (Slang, and SPIRV-Tools), so `include/` must not contain
them. The six in `src/compile/impl/` name Slang types. `include/compile/SlangCompiler.hpp`
is the wall, and it names no Slang type. **A file that needs `slang.h` belongs in `src/compile/impl/`
and nowhere else.**

| File | Owns | Lifetime |
|---|---|---|
| `SlangCompilerTypes` | The enum conversions, `BindingScope`, `RawBindingDraft`, the option table. | Free functions. |
| `SlangModuleContext` | One global session, one session, the root module, the entry points, the source texts. | One for each thread. |
| `SlangVariantCompiler` | Link, target codegen, entry point metadata. Returns `LinkedVariant`. | One for each job. |
| `SlangReflector` | Every reflection walk. Takes `LinkedVariant`, returns `RawVariant`. | One for each job. |
| `ThreadPool` | An atomic job index and a latch. Spreads variants across workers. | One for each compiler. |
| `EmbeddedFileSystem` | The Slang file system. It serves the builtin modules from memory, and every other path from disk. | One for the process. |

`SlangModuleContext` is defined in two files: `SlangModuleContext.cpp` contains the session, the bootstrap, and
the module cache, and `SlangModuleContextAxes.cpp` contains the axis declaration reading (`BuildDeclaredAxes` and
the enum and interface axes).

`src/target/impl/SpvModuleReader` parses one SPIR-V module with SPIRV-Tools into id records, and lists the
bindings it declares (a binding array is one binding with its length, 0 when unbounded) and the sampler and texture pairs it samples. `SpvValidator` compares what the reader
returns against reflection.

`client/include/` is not part of this split. It is the contract a renderer links against. A renderer includes
`ShaderManifest.hpp` (the bundle views) or `ShaderManifestIndex.hpp` (the query surface). The manifest header is
split in three layers, and each one includes the one before it: `ShaderManifestFormat.hpp` (the version, the
error codes, and every on-disk record), `ShaderManifestLayout.hpp` (the views of one variant, its resources, and
merged pipeline layouts), and `ShaderManifest.hpp` (the bundle, module, and environment views). The other headers
are `ShaderLibraryTypes.hpp`, `VariantKey.hpp`, `ResourceFlags.hpp`, and `EnumClassUtils.hpp`. The target
`lodestone_client_header_check` compiles each public header alone in its own translation unit, so a header that
needs another header first fails the build, and the include order of a client cannot matter.

`client/src/` contains two private headers that clients never see: `ShaderManifestRead.hpp` (the helpers that map
records out of the bytes) and `ShaderManifestValidator.hpp` (the checks that `BundleView::Open` and
`EnvironmentView::Open` run). The source files are `ShaderManifest.cpp` (the bundle, module, and environment views),
`ShaderManifestLayout.cpp`, `ShaderManifestValidator.cpp`, `ShaderManifestIndex.cpp` (the index and its scan), and
`ShaderManifestQuery.cpp` (the query builder and the presets).

There is no `validate/` folder, and that is worth knowing. Several validators are functions inside
the file they check: `VerifyLibraryRoundTrip` and `VerifyLayoutRoundTrip` live in
`src/driver/CookerDriver.cpp`. `VerifyManifestRoundTrip` lives in `src/emit/ShaderManifestVerify.cpp`,
beside the writer it checks (`ShaderManifestEmitter.cpp`). The verifier shares no code with the writer: it
compares each record against the model field by field. Only the reflection cross-check has its own files, one for
each target: `WgslValidator` and `SpvValidator`. `VerifySamplerPairsAcrossProfiles` and `VerifySourceFactsAcrossProfiles` live in
`src/model/CookedLibrary.cpp`. The tree cannot show
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

**A guardrail must pay for itself at the call site.** The rule above applies to a mistake that fails in
silence. `Active` and `Canonical` hold the same kind of data, and only the bound values differ. Code that
uses one cannot see that difference, so a mix-up gives wrong output and no error. The wrapper is also
cheap: it is more than an integer, so a convenient constructor loses nothing. A strong type for a plain
table index is different. A wrong index into a validated table gives data that is visibly wrong, and each
call site must convert the value. Strong index types were tried in the manifest reader and removed
(2026-09) for this reason. Before you propose a type-level guardrail, answer two questions:

1. Does the mistake fail in silence?
2. Does each call site pay a cost for the guard?

Add the guard only when the answer to 1 is yes and the answer to 2 is no. The manifest reader validates
every index once, at `Open`. That validation is the guard for the client API. A caller who passes the
wrong valid index owns that mistake.

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

When she proposes an optimization, judge it on two questions only: is it actually faster, and is its
implementation cost reasonable against the saving? If the answer to either is no, say so plainly — she
would rather be told an idea is aimed at nothing than be helped to build it. But that is the whole test.

**Do not reject or discount an optimization on the grounds that the Slang compile dominates the cost.**
That reflex is wrong here for three reasons: the "order of magnitude" numbers were measured before the
thread-pool change (they threaded `getEntryPointCode` alone, not variants across a pool) and were never
profiled in a real release build, so the true ratio is unknown and likely smaller; a cleaner approach
that is also faster with a low implementation cost is worth taking whether or not Slang dwarfs it; and
the goal of this layer is to be a near-zero-overhead abstraction over Slang. A near-free advanced
feature set over Slang is the product, so overhead added "because Slang is slower anyway" is death by a
thousand cuts, not a saved effort. State a cost when you have measured one. Do not invoke Slang's cost
to avoid making the cooker's own code fast and clean.

She dislikes default arguments and adds one only when it is unavoidable. A required parameter makes
every call site state the value it means, so `EnumerateVariants` takes its variant budget as a plain
`size_t` and the tests pass `0u`. Do not propose a default parameter as a convenience; propose an
explicit argument, or an overload, or a named constant the caller passes.

## Data flow, one cook

`RunCook` in `src/driver/CookerDriver.cpp` drives the cook. As of 2026-09-19 it is a chain of discrete
step functors, not one long loop: `PrepareCookStep` once, then `PrepareModuleStep`,
`PreparePermutationSpaceStep`, `BuildModuleStep`, and `FinalizeModuleStep` for each (module, target)
pair. `include/driver/CookerSteps.hpp` defines the state each step passes on, the step sources sit in
`src/driver/steps/`, and `SharedCookState` holds the whole-cook data. A cook now takes several modules
and several targets. The eight numbered stages below still describe the transformation the steps run, in
the same order. Read `CookerDriver.cpp` first.

**A stage transforms. A validator compares.** The pipeline has eight numbered stages, and each one
takes an input type and gives an output type. A validator reads the output of a stage, compares it
against a second opinion, and changes nothing. A validator gets a name and no number, because a
number would state that every target must supply one. A target supplies a validator only when it can.

1. **Declare.** The axes live in the shader, on `extern static const` (and `extern struct` for an
   interface axis) declarations, as `ls_axis_*` attributes. An interface axis names a concrete type per
   variant. An enum axis names a `public` Slang enum, and its cases become the axis values by name.
   `SlangCompiler::PrepareRawModule` reads them at the bootstrap compile through
   `SlangModuleContext::ReadDeclaredAxes`, and `BuildPermutationSpace` (in `src/driver/CookerDriver.cpp`)
   turns them into a `PermutationSpace`. The
   `SymbolTable` prunes an axis no reachable source uses, so an imported-but-unused axis leaves the
   space. An axis name cannot drift from its declaration, because only one name exists. A module with
   no declared axis gets an empty space and one variant.
2. **Enumerate.** `space.EnumerateVariants()` expands the space into a `VariantSet` of
   `VariantDescriptor` values. Each descriptor holds `Active` and `Canonical` (see below) and a dense
   index.
3. **Compile.** Stage 3 walks three scopes, and each one needs a different call: the global scope,
   the parameter scope of each entry point, and the element of each `ParameterBlock`. Slang describes
   a block with descriptor ranges alone, so the binding range of a block reports a descriptor set
   index of -1 and the range walk drops it. `CollectSubObjectDrafts` keeps exactly the ranges the
   range walk dropped, so the two walks partition the ranges and no range is drafted twice.

   **Shim externs** (phase F step F2) are read at stage 3 too. `extern struct X : IReadBuffer<T>` (any interface in
   `target/ShimTable`) is a resource that the profile lowers: `PrepareRawModule` reads it from the source text through
   `SymbolTable::ExternStructsForModule`, confirms it with reflection (`ShimExternNotReflected`), resolves the declaring
   module of each type argument, and builds the source of a link module (`MakeShimLinkSource`) that exports the
   implementation of the profile for each extern. Each worker context loads that module once and `LinkVariant` adds it to
   every variant. A shim extern is a parameter type (a block field, a global, an entry point `uniform`) and is not an axis.
   The type argument must be `public` in its module, and a plain name. Used as a function-local, a resource cannot work.
   A storage extern (`IWriteStorageTexture2D`) takes its format from an enum axis over `StorageFormat`, which
   `[ls_storage_format("AXIS")]` on the extern names. Reflection is the only reader of that attribute.
   `MakeShimFormatLinkSources` gives one link module for each value of the axis, each worker context loads all of them
   once, and `LinkVariant` adds the module of the variant's value. The axis must be ungated, because a variant with the
   gate off would have no format.
   Stage 3 has two entry points. `SlangCompiler::PrepareRawModule` runs once for each
   module and returns the module facts only Slang can supply: the name, the entry point names, and
   the declared default of every `extern static const` constant no axis drives. A size expression may
   name one of those defaults, so it must run before the first variant.
   `SlangCompiler::Compile` then runs once for the whole variant set, and returns one
   `CookResult<RawVariant>` for each variant. A `ThreadPool` spreads the variants across workers.
   Each worker holds one `SlangModuleContext`, and each job runs a `SlangVariantCompiler` and then a
   `SlangReflector`. Output is `RawVariant`. Every `[ls_*]` argument comes back as the string the
   author wrote, because evaluating one is stage 4's job.
4. **Resolve.** `ResolveVariant` in `model/ResolveStage.cpp` reads the `[ls_*]` attributes and evaluates each
   size expression against a `ResolveContext`. Output is `CompiledVariant`.
   **No file under `src/compile/` names a size expression or evaluates a `[ls_*]` attribute, and
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
8. **Emit.** `EmitLibraryArtifacts` (in `src/driver/CookerDriver.cpp`) runs once, after every (module,
   target) pair has cooked. It reads one `CookedLibrary`, a grid of `CookedModule` values ordered by
   profile and then by module, and writes through the `OutputSink`:
   - one binary manifest bundle for the whole cook, `ShaderLibrary.ldmanifest`, plus `VerifyManifestRoundTrip`
   - `ShaderLibrary.dedupe.txt`, from `GenerateDedupeReport`
   - `ShaderLibrary.resources.txt`, from `BuildResourceReport` and `FormatResourceReport`, read from the written
     manifest bytes through the client views

   A cooked module points into its `PermutationSpace`, so the driver moves each space to the heap and
   keeps it until the emit is done.

Nine validators run inside that loop. None of them is a stage.

Each validator is one bullet here. `docs/code-notes.md` section 14 holds the detail of each one: what it reads, what it
skips, and the Slang workarounds it carries.

- **The reflection cross-check**, after stage 4 (`ReadEmittedCode` in `BuildModuleStep.cpp`). The profile's validator reads
  the emitted code (Tint for WGSL, `SpvValidator` for SPIR-V) and compares its bindings with reflection. A mismatch fails
  the cook with `ReflectionMismatch`. A profile with no validator fails with `TargetValidatorUnavailable`, and
  `--no-validate` skips the check on purpose. The same read gives cook output: the sampler and texture pairs (task L7),
  the push constant block of the entry point constants (task L14), and the source facts. Each vertex input that the code reads must be a reflected
  vertex input with the same type (`CompareVertexInputs`, `BindingMismatchKind::VertexInput`). `--no-validate` still reads them
  (`ReadEmittedCodeFacts`). **The code decides which bindings an entry point uses** (`KeepDeclaredBindings`): above `--O0`
  Slang gives SPIR-V to spirv-opt, which removes bindings that Slang's metadata marks used, so a reflected binding that the
  code does not declare leaves the visibility and is not a mismatch. Every other difference still fails. One Slang fault
  has a narrow workaround here, with a canary test: shader-slang/slang#13895 (`IsSlangNonReadableArrayFault`).
  `SlangMetadataCanaryTest` still watches shader-slang/slang#13789, which needs no workaround now.
- **The delivered form check**, after the reflection cross-check (`VerifyDeliveredForm`, `model/DeliveredForm`). When the
  cook's flags are not the base flags, each entry point instance's delivered form must need no more than its base form, in
  one target: used bindings, sampler pairs (by binding), capability names (plus `SPV_KHR_non_semantic_info` from debug
  information), workgroup memory, and entry point constants (`DeliveredFormExceedsBase`).
- **The sampler pair cross-check** (`VerifySamplerPairsAcrossProfiles`, `model/CookedLibrary.cpp`). The pairs that two
  profiles read for one variant must agree (`SamplerPairsTargetMismatch`), in the base form. It skips
  an entry point whose pairs the code cannot show: a combined texture sampler.
- **The source facts cross-check** (`VerifySourceFactsAcrossProfiles`, same file). Workgroup memory and a portable subset of
  the capability names must agree between profiles (`SourceFactsTargetMismatch`), in the base form (`--O0`, no debug
  information), which the cook compiles beside the delivered form when the flags differ (`docs/base-cook-plan.md`).
- **The capability axis check** (`VerifyCapabilityAxes`, `model/AxisInfluence`). An axis of kind `capability` must change
  the measured capability names (`CapabilityAxisInert`), and a boolean off path must need a subset of the on path
  (`CapabilityAxisInverted`).
- **The footprint axis cross-check**, after stage 7 (`VerifyFootprintAxes`). An axis that a one-axis pair proves changes a
  footprint must be named by that resource's size attributes (`FootprintAxisUndeclared`).
- **The library round trip**, after stage 7 (`VerifyLibraryRoundTrip`). It replays every variant through the tables and
  compares the **source text** only.
- **The layout round trip**, after stage 7 (`VerifyLayoutRoundTrip`). It replays every entry point instance through the
  resource, footprint, and visibility tables and compares with `BuildEntryPointLayout`. `EnforceModulePolicy` then checks
  the measured axis influence against the policy file.
- **The manifest round trip**, inside stage 8 (`VerifyManifestRoundTrip`). It reads the written manifest back the way a
  consumer does and compares it with the model, including the source facts and the pairs. `VerifyAxisRelevance` derives the
  axis relevance again from the written bytes (`ManifestAxisRelevanceMismatch`).

`--verify-deterministic` runs stages 1 to 8 twice into two `MemoryOutputSink` objects, compares every
artifact byte for byte, then writes the first result to the real sink. A difference means an
unordered container reached the output.

## Major types

| Type | Header | Role |
|---|---|---|
| `CookerOptions` | `driver/CookerOptions.hpp` | Every knob one cook has. `ParseCommandLine` fills it. |
| `OutputSink` | `emit/OutputSink.hpp` | Where artifacts go. `FileOutputSink` and `MemoryOutputSink`. The seam a live cooker will use. |
| `PermutationValue` | `permute/PermutationValue.hpp` | One axis value: `Bool`, `UInt`, or `Type` (an interface-axis ordinal into the axis's implementation list). A tagged union, and not a `std::variant`, so no header downstream carries the template. |
| `PermutationAxis` | `permute/PermutationAxis.hpp` | One axis of variation. It owns its values in a `std::vector`. `ActiveWhen` gates it on an earlier axis (it replaced the old parent link). An interface axis also carries the interface name and its implementation list. |
| `PermutationSpace` | `permute/PermutationSpace.hpp` | A named, ordered set of axes, and every question you ask about them. It **owns** its axes, so it cannot be copied. |
| `PermutationBinding`, `PermutationAssignment` | `permute/PermutationAssignment.hpp` | One axis bound to one value, and a list of them. |
| `VariantDescriptor` | `permute/PermutationAssignment.hpp` | One variant identity. `Active` and `Canonical`. |
| `CanonicalAssignment` | `permute/PermutationAssignment.hpp` | An assignment that holds every axis of one space. Only `PermutationSpace::CanonicalizeAssignment` builds one, so `ComputeVariantKey` cannot be given a partial assignment. |
| `VariantKey` | `client/include/VariantKey.hpp` | A variant's identity: a strong `enum class : uint64_t`, a mixed-radix packing of the canonical assignment. `PackVariantKey`/`UnpackVariantKey` are the shared codec the cooker and the client both use. |
| `PolicyDocument`, `TargetCookPolicy` | `permute/PolicyDocument.hpp` | The cook policy read from a TOML file: per target a variant budget, a `CookValues` allow-list, and a `CookIf` predicate. `ModulePolicyEntry` carries the per-module expectations to `EnforceModulePolicy`. |
| `CompiledVariant`, `CompiledEntryPoint` | `model/ShaderDataSchema.hpp` | Compiler output: WGSL text plus reflection. Owns its strings. |
| `ReflectedBinding` | `model/ShaderDataSchema.hpp` | What the shader states about one resource. The CPU side never writes any of it. `Name` and `ScopeName` together are the identity: two entry points can each declare `albedoMap`. |
| `RawVariant`, `RawModule` | `compile/RawLibrary.hpp` | Stage 3 output. Everything Slang says, with no opinion about any of it. A `[ls_*]` argument is still the string the author wrote. |
| `ResolveContext` | `model/ResolveStage.hpp` | Stage 4 input. The axis values of one variant, the extern constant defaults that stage 3 carried out, and the derived constants that the variant's size attributes reach. `MakeDerivedConstantTable` indexes the derived constants once for each module. |
| `TargetProfile` | `target/TargetProfile.hpp` | A target name, an access model, and an optional validator. `--target` names one. |
| `DiagnosticRecord`, `DiagnosticSink` | `compile/Diagnostics.hpp` | One compiler message as a record, and where it goes. Compilation never formats a string. |
| `ContentInterner<T>` | `model/ContentInterner.hpp` | Collapses equal payloads, keeps provenance, counts collisions. |
| `InternedModule` | `model/CookedLibrary.hpp` | The stage 6 builder. It holds the six interners, and it is the only place the provenance of a collapse survives. |
| `CookedModule`, `CookedLibrary` | `model/CookedLibrary.hpp` | The frozen model. Every emitter reads this and nothing earlier. |
| `BundleView`, `ModuleView`, `EnvironmentView` | `client/include/ShaderManifest.hpp` | Read-only spans over the bundle bytes, one type for each scope: the whole cook, one module, and one module cooked for one profile. Each `Open` validates its scope once and allocates nothing. |
| `ManifestIndex`, `ManifestQueryBuilder` | `client/include/ShaderManifestIndex.hpp` | The client query surface, frozen. `ManifestIndex` decodes and enumerates variants and hands out a value-semantic builder. The builder resolves axis names and values by name, and its terminals are `Keys`, `Variants`, `Single`, and `SingleFor`. `ForDevice(supports, user, limits)` removes the variants that the device cannot run: `Keys`, `Variants`, and `Single` test every entry point of the module, and `SingleFor` tests the named ones. A query that the constraints match and the device rejects entirely gives `NoVariantFitsDevice`, with the `DeviceMismatch` in `Detail` and the capability name in `AxisName`. Each constraint has a `PinMode`: `Where` is `Required` (the axis must be active), and `WhereWhenActive` tests the value only where the axis is active, so it keeps the variants where a gate is off. `Apply(QueryPreset)` applies a set of pins that serves any module: a pin on an axis the module does not declare is skipped and gives a `QueryNote`, never an error. `Notes()` holds those and the `DuplicateValue` notes, and `NotesFor(entry_points)` adds a `PinAxisIrrelevant` note for each pin that cannot change those entry points. `SingleFor(entry_points)` is `Single` for a pipeline: an axis that cannot change any of the named entry points does not count, matches that differ only in such axes are one answer, and the smallest key is returned. `Keys` and `Variants` return a set or an error code: a malformed query is an error, and a valid query with no variant is an empty set. `Single` returns one key or a whole `QueryError`, and never chooses between two matches: more than one match is `AmbiguousVariant`, which names the axes that still vary. `NearestName` is the "did you mean" that the query errors use, public so a renderer can use it for its own names. | |
| `ShaderSourceProvider` | `client/include/ShaderManifest.hpp` | Where a renderer gets code, bindings, and workgroup size. `Source()` gives WGSL text, and `SpirvWords()` gives SPIR-V words. Each asserts the profile's `ShaderCodeFormat`. `Bindings()` returns a `LayoutRange` of `ResolvedResource` views, built on demand, so the provider allocates nothing. `Generation()` is the hot-reload hook. |
| `MergedLayoutView`, `MergedResource` | `client/include/ShaderManifestLayout.hpp` | The layout of one pipeline. `Merge` takes one `EntryPointInstanceView` for each stage, checks every shared (group, binding) once, and gives each resource once with a mask of the stages that read it, in placement order. No template and no allocation: a fixed array of up to `k_MaxPipelineStages` ranges. It rests on the placement order of each visibility list, which the cooker sorts and `Open` checks. `SamplerTexturePairs()` gives the union of the stages' sampler and texture pairs, as `MergedSamplerTexturePair` values. |
| `LayoutRange`, `ResolvedResource`, `VariantView`, `EntryPointInstanceView` | `client/include/ShaderManifestLayout.hpp` | Views over one environment. `LayoutRange::operator[]` is the one place that resolves `resources[visible[i]]`. A view points at its `EnvironmentView`, so keep that environment alive and do not move it. |

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
   cannot leave a key while it stays in the type. The tables are separate because one binding record
   carried three concerns with three keys: placement (per module), visibility (per entry point), and
   footprint (per variant). OceanFft once interned 21 layouts, and 21 = 1 x 3 x 7.
4. **The emitted artifact decides group and binding numbers. Reflection decides sizes and types.**
   That asymmetry is the only reason the cross-check finds real errors. A **name** is neither: the
   cross-check builds the name it expects from `ScopeName` and `Name`, and compares that against the
   de-mangled emitted identifier. It states what it expects rather than removing a prefix it does not
   recognise.
5. **Every emitter reads one frozen model.** An emitter must not reach past `CookedLibrary` into
   `CompiledVariant` or into Slang.
6. **An axis name cannot drift from the constant it drives, because only one name exists.** Phase E
   step E6 moved the axis declaration onto the `extern static const` (or `extern struct`) itself, as an
   `ls_axis_*` attribute the cooker reads through reflection. The old failure — an axis name that no
   symbol references, which leaves the shader on its default and fails nowhere — cannot be written down
   any more, so the check that guarded against it is gone.

### `Active` and `Canonical`

`Active` holds only the axes this variant really uses. It drives the Slang linker, so it decides the
emitted text. `Canonical` holds every axis in declaration order, with the first value filled in for a
disabled axis. It drives the dense mixed-radix index only. Canonicalization therefore can never
change shader output, and a caller can find a variant with a partial set of values.

`Canonical` has its own type, `CanonicalAssignment`, and only `PermutationSpace::CanonicalizeAssignment`
builds one.
`PermutationAssignment` named four different things: an active assignment, a partial one, a canonical
one, and the parameter of every function that takes any of them. `ComputeVariantKey` needs the
canonical form, and it stated that in a parameter name alone. A partial assignment now fails to
compile. The type holds the same vector and costs nothing at run time.

`SpaceSize` counts the dense index range with the holes included. A disabled dependent axis leaves
gaps, and the design accepts them.

### Attribute expressions

A size travels as a string, and this is not a style choice. Slang folds an attribute integer argument
at compile time, but the permutation constants are `extern static const` and fold at link time.
`[ls_element_count(IFFT_SIZE * 4)]` therefore fails to compile. A string argument reaches reflection
untouched, and `EvaluateExpression` does the arithmetic once for each variant. The same evaluator now
reads an axis constraint expression as well, which is why it lives in `permute/AttributeExpression.hpp`
and not the old `SizeExpression.hpp`. Phase E step E1 gave it comparison and logical operators for the
constraint language.

A size expression can also name a derived constant: a module-scope `static const` whose initializer is
computed from axes, such as `static const uint CLUSTER_COUNT = GRID_X * GRID_Y;`. The symbol table reads
the initializer text, and reflection filters it to module scope (`RawModule::DerivedConstants`). Stage 4
evaluates the text per variant, so the value follows the axes. A value folded by reflection would stay at
the declared defaults. `MakeResolveContext` walks depth first from the size attribute arguments, with
three-color marking: a gray constant reached again is a cycle, and a black one is already evaluated. Each
constant is evaluated in post-order, after the constants it uses. Only reached constants are evaluated, so
an unused `static const float` is harmless. A derived constant with the name of an axis or of an extern
constant fails the module (`DerivedConstantShadowsSymbol`), and so do two declarations of one name with
different expressions (`DerivedConstantAmbiguous`), even when no attribute reaches them. The errors are
150 to 153, and each error prints the `file:line` and expression of every constant involved. Slang
rejects a real cycle itself (E39999), so the cycle check fires only when the text reader and Slang
disagree. An axis default cannot use a derived constant, because defaults are read before any variant
exists.

The attribute declarations are in `builtins/LodestoneAttributes.slang`: `ls_element_count`,
`ls_extent_1d`, `ls_extent_2d`, `ls_extent_3d`, `ls_resource_role`, `ls_resource_tags`, and `ls_storage_format`. Slang has no optional
attribute parameters, so each arity needs its own name, and a list (of roles or of tags) is one string, separated
by spaces. The README shows `ls_element_count`; the code says `ls_element_count`.

## Where axes and policy live

No module data is compiled in. Phase E step E6 deleted the old registry
(`src/permute/PermutationRegistry.cpp`, `k_ModuleSpaces`, `FindPermutationSpaceForModule`) whole.

An axis is declared in the shader, on its `extern static const` (or `extern struct`, for an interface
axis) declaration, as an `ls_axis_*` attribute. `builtins/LodestoneAttributes.slang` defines the
attributes: `ls_axis_values`, `ls_axis_boolean`, `ls_axis_active_when`, `ls_axis_kind`, `ls_axis_enum`,
`ls_axis_interface`, and `ls_axis_interface_impl`. `SlangCompiler::PrepareRawModule` reads them at the
bootstrap compile through `SlangModuleContext::ReadDeclaredAxes`, and `BuildPermutationSpace` (in
`src/driver/CookerDriver.cpp`) turns them into a `PermutationSpace`.

An enum axis needs a `public` enum, because the per-variant synthetic module imports the enum's module
and names the case. The reflection reads each case name and its value. The manifest stores the names, so
the value stays a cook-time fact for a size expression to use later.

The cook policy is data, not code. A `PolicyDocument` reads a TOML file through toml++, behind a facade
in `src/permute/PolicyDocument.cpp` (no toml++ type leaves that file). A module can name an
`InertAxesForEntryPoints` table and a section for each target profile, and a target section carries
`MaxVariants`, a `CookValues` allow-list, and a `CookIf` predicate. A `CookValues` list holds booleans, integers, or, for an
enum axis, case names (`["Rgba16Float", "R32Float"]`), which `PermutationAxis::FindEnumCase` resolves. The `--policy-file` option names the
file. `tests/assets/KitchenSink/KitchenSink.toml` is the reference.

The policy is opt-in (phase F decision O4). A module with no entry, or with no `targets` table, cooks every
variant with no limit, and the cook says nothing. `FindTargetCoverage` names the one suspicious case: a
module that has a section for another target and none for the cooked one. That cook continues with no
limit, and prints a warning that names the module and the target. A section for a target the build does
not know (`targets.spriv`) fails the load with `PolicyUnknownTargetName`, as an unknown `--target` does.

A `PermutationSpace` owns its axes by value, so a copy would leave every `PermutationBinding` of the
original aimed at a different object, and a gated axis would then read as absent and quietly reduce the
variant count. Copying a space is deleted for that reason. Moving one is safe, because moving the vector
keeps the axis addresses.

## One output form: the binary manifest

The cook writes one output form, the binary manifest bundle, from the `CookedLibrary`. The C++
header/source emitter is gone. `-o` names an output **directory**, and every artifact goes inside it:
one `ShaderLibrary.ldmanifest` for the whole cook, one `ShaderLibrary.dedupe.txt`, one
`ShaderLibrary.resources.txt`, and, when `--dump-stage` asks, the stage-dump JSON files. A cook removes the
manifest, the two reports, and the depfile of an earlier cook before it starts, so a failed cook leaves none of
them, and the file sink writes each artifact to a temporary file and renames it into place.

The bundle has three scopes, and the reader has one view type for each.

- **Whole cook** (`BundleView`). The header region holds the `Header`, one `ModuleRootHeader` for each
  module, the profile table, the environment directory, the root axes and their values, each module's
  entry points and `ModuleAxis` run, and the string table. Offsets here are absolute and 64-bit.
- **One module** (`ModuleView`). The name, the entry points, and the axes. A `ModuleAxis` names a root
  axis and a 32-bit mask of the values the module uses. The radix of the axis in the module's key is the
  count of set bits, and digit D selects the D-th set bit.
- **One module for one profile** (`EnvironmentView`). One extent: the variant keys, the variant records,
  the per-variant axis-active masks, the slot grid, the sources, and every layout table. Offsets are
  32-bit and relative to the extent start, so an extent loads into its own buffer with no fixup.

The directory is a dense grid of `ProfileCount * ModuleCount` entries, ordered by profile and then by
module, and the extents sit in the same order. A renderer reads the header region, then one contiguous
run of extents for its profile. An entry with a size of zero means the cook skipped that module for that
profile. Each profile applies its own policy, so two profiles of one module can hold different variants.

Two modules share a root axis when the name, kind, domain, and binding time agree and the module's
values fit the root values in order. Otherwise the axis gets a root record of its own. A root axis is a
storage and dedup optimization only: keys are per module, so no key crosses a module boundary.

What each version added (details in `client/FORMAT.md`):

- 7: `Profile` records its `ShaderCodeFormat`, and a SPIR-V source must be whole words.
- 8: `ModuleAxis::DefaultDigit`, the declared default of each axis.
- 9: axis relevance for each entry point (two passes, closed mask), at `EnvironmentHeader::AxisRelevanceTableOffset`.
- 10: sampler and texture pairs for each entry point instance (`SamplerTexturePairs`, `ProfileFlags::SamplerTexturePairsRecorded`).
- 11: entry point constants for each entry point instance (`EntryPointConstants`, `EntryPointConstantsDelivery`).
- 12: resource roles and tags (`Binding::Roles`, `Binding::FirstTag`, `ResourceTags`).
- 13: footprint axis mask for each binding record (`EnvironmentHeader::FootprintAxisTableOffset`), and `DescribeCreation`.
- 14: source facts, the capability names and `SourceRef::WorkgroupMemoryBytes`, and the device fit (`Fit`).
- 15: `ProfileFlags::CodeFactsRead`.
- 16: `Binding::ArrayCount` of 0 for an unbounded binding array.
- 17: stored values for the Handle and Pointer forms (`Binding::ValueContainer`, `Binding::ValueOffset`, `Profile::Forms`).
- 18: the cook's `--O` and `--g` levels (`Profile::OptimizationLevel`, `Profile::DebugLevel`), the base form's hash for each entry point instance (`EntryPointInstance::BaseHash`), and the descriptor heap (`Profile::HeapLowering`, `Profile::DescriptorHeapSet`).

Read `client/FORMAT.md` before you change the manifest format or the client views.

Every cross-reference is an index or an offset, never a pointer, so the reader is a set of spans and it
relocates nothing. Sections start on 8-byte boundaries. A record must be trivially copyable, and the
`k_IsManifestRecord` asserts in `client/src/ShaderManifestRead.hpp` enforce that. Record **sizes** are not
pinned: the format has no version migration yet, so a record can still grow.

Each table has a `TableRef` (32-bit, inside an extent) or a `TableRef64` (header region): an offset and a
count together. The reader maps a table with `Map<T>(region, ref)`. Four extent tables keep an offset only,
because the variant count sets their size: keys, variants, axis masks, and slots.

A table index is a plain `uint32_t`. The field name tells which table it indexes, for example
`NameString` or `ResourceList`. Do not give a member function or a field the same name as a type that the
class already uses, such as `Variant` in `EnvironmentView`. That changes the meaning of the name in the
class, and clang and GCC reject it.

A variant is found by its **key**, not by a dense index. `EnvironmentView::FindVariant` does a
`lower_bound` on the sorted key table, and the position it finds is the variant index. The slot for
(variant V, entry point E) is at `V * EntryPointCount + E`, so a variant stores no slot range.

A `VariantKey` is an in-session handle, not a save token. A key packs against the axis order and value
count of one manifest, so a recook can change what it means. To persist a variant, save the decoded axis
names and values, and rebuild the key on load. The client query surface resolves by name for the same
reason: a rename or a reorder of axes degrades to an empty result, never to a wrong variant.

## The velox rename

This repository was extracted from an engine named `velox`. The C++ rename to `lodestone` is
complete. No `velox` name remains in `src/`, `include/`, `client/`, `tests/`, or `tools/`.

The shader side is renamed too. `builtins/LodestoneAttributes.slang` declares
`module LodestoneAttributes`, and every attribute uses the `ls_` prefix. The attribute names are part of
the shader-side contract, and `src/compile/impl/SlangReflector.cpp` reads them by string.

## Builtin modules

The files in `builtins/` are compiled into the library. A cook needs no path to find them.

- `cmake/EmbedBuiltins.cmake` writes each file into a byte array in the generated header
  `compile/EmbeddedBuiltinModules.hpp`. The module name is the file stem. A new file joins at the next
  build, because the glob uses `CONFIGURE_DEPENDS`. Run `scripts\configure.bat` after you add a file: before 2026-10-22 the
  list in `cmake/LodestoneCooker.cmake` named `LodestoneAttributes.slang` only, and this sentence was wrong.
- `EmbeddedFileSystem` in `src/compile/impl/` is the Slang file system of every session. It serves a
  builtin under the search path `lodestone-builtin`, and every other path from disk. No directory of that
  name exists.
- It implements `ISlangFileSystemExt`, not the plain interface. Slang wraps a plain `ISlangFileSystem` in a
  cache that reports a content hash in place of a path, so `getDependencyFilePath` then names no file.
- **Do not load a builtin with `loadModuleFromSourceString`.** Measured on 2026-09-24: the worker sessions
  then fail to load the serialized root module, and Slang reports nothing. An import through the file
  system works.

What the file system covers, read from `Linkage::setFileSystem` in the Slang submodule:

- **It replaces Slang's default completely.** With no file system, Slang uses
  `CacheFileSystem(OSFileSystem)`. An `ISlangFileSystemExt` is used directly, with no cache around it. So
  every source read, every `import` and `#include` search, and every probe for a `.slang-module` in the
  cache directory goes through `EmbeddedFileSystem`.
- **Some loads do not use it.** `loadModuleFromIRBlob` (the worker priming) takes a blob. The core module
  is built into the Slang library. `writeToFile` (the module cache write) opens a `FileStream` directly.
- **Two methods are partial on purpose.** `enumeratePathContents` returns `SLANG_E_NOT_IMPLEMENTED`, which
  the interface permits: Slang needs it for no normal compile. `clearCache` does nothing, because the
  object caches nothing.
- **The default cache layer is gone.** `CacheFileSystem` kept file contents and identities for each
  session. Slang's source manager still keeps each loaded file by its identity, so a file is not read
  twice for one module. No cost is measured. The KitchenSink cook time did not change visibly.
- **Paths are UTF-8, and a combined path uses the Windows separator.** The identity of a disk file is its
  canonical path. `FindEmbeddedBuiltin` accepts both separators.
- **Not implemented: `ISlangMutableFileSystem`.** Slang writes nothing through the session file system in
  this cooker, so no write method is needed.

## Documents

**`docs/` is in `.gitignore`.** A document survives only when somebody force-adds it with
`git add -f`. Run `git ls-files docs/` to see which ones did. Every other document exists on disk and
in no commit.

### Background — what the repository is

- `docs/agent-handoff.md` — **read this first.** State, the build faults that are open, the measured
  Slang facts, and the next task. It is newer than this file on anything that moves.
- `docs/code-notes.md` — the techniques behind single functions (the reflection walks, the two-pass relevance, the
  std430 workgroup rule, slang#13789), moved out of code comments on 2026-10-21. It links to the code. Code comments
  do not link to it. Section 14 holds the detail of each validator.
- `tests/README.md` — what each test target proves. Tracked in git, unlike `docs/`.
- `client/FORMAT.md` — what each manifest format version added. Tracked in git, unlike `docs/`.
- `README.md` — the author's intent. It mixes shipped features with planned ones. Check the code
  before you trust a feature claim there.

**Check that a document exists before you cite it.** This list has named files that were deleted or
renamed. `ls docs/` is the only reliable answer.

### Planned work — where the repository is going

Phases D, E, and F are a **sequence**. Each depends on the one before it. The tooling track runs
beside them.

- Phases D (stage separation, `--dump-stage`) and E (axes in the shader, policy in a data file,
  constraint expressions, the sorted key table) are **complete**. Their plans and the two phase E spikes
  were removed on 2026-10-14. Read one from history: `git show e113d8c:docs/<name>.md`, with the names
  `phase-d-stage-separation-plan`, `phase-d-tail-handoff`, `phase-e-data-driven-permutations`,
  `phase-e-attribute-spike`, and `phase-e-interface-spike`.
- `docs/phase-f-vocabulary.md` — **read this before proposing anything about targets or bindings.**
  It defines axis kind, binding time, and access model, and it divides the work between Slang's
  capability system and this repository. It is a vocabulary, not a plan. Its §9 holds the measured
  answers that the plan rests on. §9e corrects §9a: link-time specialization selects the access model.
- `docs/rendergraph-integration-tasks.md` — the Lodestone tasks that the VeloxRhi rendergraph needs, numbered L1 to L18
  as in the Velox note. Nearly all are done; L5 and L6 became phase F slice F3. Read its sections 4 and 5 first.
- `docs/phase-f-plan.md` — **the current work (resumed 2026-10-21).** The plan for phase F: decisions, slices, open
  decisions, and a progress log. F0, F1, F2, F3 (except F3.4d), and C11 are done. Read its progress section first, and update it at
  the end of each step.
- `docs/diamonddogs-requests.md` — what DiamondDogs, the embedding project, needs from Lodestone. Each row has a state,
  and the notes at the end explain the finished rows for that project's agent.

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
