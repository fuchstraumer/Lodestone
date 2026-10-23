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

The configuration argument defaults to `RelWithDebInfo`. Until 2026-10-16 it defaulted to `Debug`. The
Debug test binaries in `build/ninja-msvc` were last built on 2026-10-01 from Slang `6eb89786c`, which
writes IR module version 32. They share the module cache folder `%TEMP%\LodestoneShaderCooker` with the
RelWithDebInfo build, and `ac945e5` reads version 31 only. So one Debug run poisoned the cache, and every
later RelWithDebInfo cook then failed with E00131, crashed with 0xC0000409, or hung. Measured on
2026-10-16: the two files differ in one byte, the version field. Do not run the Debug tests until that
tree is rebuilt, and rename the cache folder after any run of an old binary.

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

There is no test framework. `tests/TestHarness.hpp` gives a counter, `Check(condition, description)`,
and a nonzero exit code.

Forty-one test targets exist. Thirty-four are unit tests, and each one proves a claim the repository
makes. Nineteen need no Slang and no asset, and together they run in about half a second.
`WgslValidatorTest` links Tint to read WGSL. `SpvValidatorTest` links SPIRV-Tools to assemble SPIR-V.
`AccessModelRejectTest`, `AxisDefaultTest`, `BindingArrayTest`, `CapabilityAxisTest`, `DerivedConstantTest`, `EntryPointConstantsTest`, and `ResourceRoleTest`, `ShimCookTest`, `ShimExternTest`, `ShimModuleTest`, `StorageFormatTest`, and `StorageFormatAxisTest` cook small modules
through Slang, and each takes about one second. `SlangMetadataCanaryTest` calls Slang directly.

| Target | Proves |
|---|---|
| `AttributeExpressionTest` | The attribute expression grammar — arithmetic, comparison, and logic — and every rejection it must make. |
| `ContentInternerTest` | A hash never decides equality. It supplies a hash that returns one constant, so only the byte comparison can separate the payloads. |
| `PermutationIndexTest` | A variant index is unique, dense, and stable, and a partial assignment resolves to one variant. |
| `ShaderManifestRejectTest` | The bundle reader rejects a short, misaligned, or damaged file, and opens a real one. It rejects an unknown code format and a SPIR-V source that is not whole words, and reads SPIR-V back as words. It also proves that the header region opens alone, that an extent read on its own opens against that bundle, that an environment the cook skipped reads as absent, and that a damaged directory entry or variant key table is rejected by name. It proves that the sampler and texture pairs read back through the layout, that a pair with the wrong binding kind, a position past the visibility list, or a bad list index is rejected, that a profile with no reader reads as not recorded, and that the round trip fails on a pair that differs. It also proves `VerifySamplerPairsAcrossProfiles`: two profiles that read different pairs fail, and a profile that did not record is not compared. It proves that the entry point constants read back and resolve through the layout to their uniform buffer, that the round trip fails on constants that differ, and that an unknown delivery, a uniform buffer of another size, a position past the visibility list, a position on a push constant block, a member run past the table, or a bad record index is rejected. It proves that a binding's roles and tags read back and take part in the round trip, and that an unknown role bit, a tag run past the tag table, a repeated tag, or a tag past the string table is rejected. It proves that the capability names and the workgroup memory of each source read back through an entry point instance, that the round trip fails on either one when it differs, and that a capability bit past the name table or a name past the string table is rejected. It also proves `VerifySourceFactsAcrossProfiles`: workgroup memory that differs fails, a 16-bit float or a subgroup operation that only one profile reads fails, a name that only one target spells is not compared, and a profile with no reader is skipped. It also proves that `EnvironmentView::HasCodeFacts` follows the profile flag. |
| `WgslValidatorTest` | The WGSL cross-check on Tint. It parses fixed WGSL, reads the used bindings from Tint's inspector, and compares them against hand-written reflection. It proves a match, and a mismatch of kind, shape, access, or name. It also proves that a comparison sampler and a depth texture read back correctly, that a storage buffer's structured or raw shape does not change the WGSL kind, and that invalid WGSL is a parse error, not a mismatch. It proves that a storage texture whose reflected format is absent or differs from the WGSL format is a mismatch. It proves that the sampler and texture pairs come from Tint, a comparison sampler too, and that the read path (`--no-validate`) gives the same pairs as the validate path. It proves that each `enable` directive reads back as a capability name, and that the workgroup memory of four workgroup variables (a `vec3` array, a struct, a float array, a `vec4<f16>` array) is 144 bytes, the same sum that `SpvValidatorTest` reads from the same variables in SPIR-V. |
| `SpvValidatorTest` | The SPIR-V cross-check on SPIRV-Tools. It assembles Slang's own output for one entry point from text, and compares it against hand-written reflection. It proves a match, a moved binding, a missing and an extra binding, and a mismatch of kind, access, name, depth shape, and storage image format. It proves that a builtin in the interface list is not read as a binding, that the capabilities read back, and that a module spirv-val rejects, or code that is not whole words, is an error and not a mismatch. It proves that the pairs come from each OpSampledImage, traced back through the loads to the variables, on both read paths, and that an image that does not trace back to a variable fails with `ReflectionSamplerPairUnresolved`. It proves that the push constant extent is the end of the last member, read from the member offsets: a module with no push constant block has none, Slang's block of two `float4x4` and a `float4` covers 144 bytes, a gap before the last member counts, and a non-square matrix strides its rows when `RowMajor` and its columns when `ColMajor`. It proves that each `OpCapability` and `OpExtension` reads back as a name, an alias (`StorageUniform16`) by its primary name, and that the std430 workgroup memory of four variables is 144 bytes. It proves that a binding array reads its length and compares it with reflection (a length of one or three against four is a mismatch), that an unbounded array is reported as unsupported, and that a dynamic index into a binding array adds the matching `...ArrayDynamicIndexing` capability while a constant index adds none. |
| `BindingArrayTest` | Fixed-size binding arrays on SPIR-V (DiamondDogs request C12), from the shader source to the manifest. `tests/assets/BindingArrays/BindingArrays.slang` declares one array of each kind (storage images, sampled images, structured buffers, uniform buffers) beside a sampler, all indexed by an entry point constant. The test reads each kind and `ArrayCount()` back, proves that the sampler pairs with the texture array through a dynamic index, and proves that each dynamic index states its device feature (`SampledImageArrayDynamicIndexing` and the three others). It proves that the WGSL cook of the same module fails (`CodeGenerationFailed`: Slang rejects an array of resources there), and that an unbounded array fails the SPIR-V cross-check (`ReflectionMismatch`) until phase F step F4. |
| `CapabilityAxisTest` | The check on axes of kind `capability` (phase F step F3.5, `VerifyCapabilityAxes`). Hand-built rows with no Slang: an axis that turns names on passes, an off path that needs a subset of the on path passes, two values that need the same names fail (`CapabilityAxisInert`), an off path that needs a name the on path lacks fails (`CapabilityAxisInverted`), and a gated axis, an axis of another kind, and a profile with no reader are not checked. Then four modules in `tests/assets/CapabilityAxes/` cooked for `wgsl` and `spirv`: `WaveGuarded` and `GatedWaveSize` must cook, `OffPathStillWaves` (both paths call a wave operation) and `OffPathNeedsMore` must fail. |
| `ShimModuleTest` | The shim module `builtins/LodestoneShims.slang` (phase F step F2.1). `tests/assets/Shims/ShimDirect.slang` puts the Bound implementations (`BoundReadBuffer<float4>`, `BoundTexture2D`, `BoundSampler`) in a `ParameterBlock` beside a count, and `BoundReadWriteBuffer<float4>` in an entry point `uniform`. It cooks for `wgsl` and `spirv` in one run, so both cross-checks agree. The test reads each binding back and compares its kind and placement: the scope is the path to the shim (`material_Points`) and the name is the member the shim holds (`data`, `texture`, `sampler`). |
| `ShimTableTest` | The shim table in `target/ShimTable.hpp` agrees with `builtins/LodestoneShims.slang` (decision D6, phase F step F2.2). It reads the table (five interfaces, every Bound cell filled, the Pointer and Indexed cells empty until F4.1, and the write-only storage texture as the one row whose implementation takes a storage format), then generates a Slang module with one generic struct for each non-empty cell (for the storage texture, one for each accepted format), constrained to the interface of its row, and an alias that instantiates it with the implementation of that cell. The module must cook. Three tables that disagree must not: a cell that names a type the module lacks, a cell whose type has the right arity and does not implement the interface, and a row with the wrong arity. Each fails with its own Slang error. |
| `ShimExternTest` | Shim externs (phase F step F2.3). `tests/assets/Shims/ShimExterns.slang` declares five externs of shim interfaces (with and without a default, one `public`, type arguments `PointLight` and `float4`), a commented-out one, an extern of an unrelated interface, and one ordinary axis. The test starts the bootstrap compile and reads `RawModule::ShimExterns`: five entries in source order, each with its name, interface, type argument, resource class, and line, and the only axis is the ordinary one, so a shim extern changes no variant count. Each extern also has the module that declares its type argument (`ShimExterns` for `PointLight`, none for `float4`). `ShimExternHidden.slang` declares a shim extern inside a namespace, where the symbol table sees it and reflection does not, and the cook fails with `ShimExternNotReflected`. `ShimStorageAxis.slang` puts `[ls_storage_format("LEVEL_FORMAT")]` on the line before a storage extern with no default, and reflection reads it into `RawShimExtern::FormatAxis` (phase F step C11.3); the buffer extern beside it has no axis. |
| `ShimLinkSourceTest` | `MakeShimLinkSource`, the text of the module that links the shim implementations (phase F step F2.5), with no Slang. Five externs give the exact source: `import LodestoneShims;`, one import for each distinct module of a type argument and none for a builtin type, and one `export struct X : IReadBuffer<T> = BoundReadBuffer<T>;` for each extern, in source order. The Pointer cell of the table is empty, so that profile fails with `ShimFormUnavailable`, and so does an invalid access model. A buffer with no type argument, a texture with one, a nested generic, two arguments for one parameter, a type argument with no module entry, and an interface that is not in the table fail with `ShimExternInvalid`. A storage extern with no format axis fails with `ShimStorageFormatMissing`, and a format axis on a buffer with `ShimExternInvalid`. `MakeShimFormatLinkSources` (phase F step C11.3) gives one module for each value of a format axis, with the exact text (`BoundWriteStorageTexture2D<RGBA16F>` for `Rgba16Float`), and the fixed module does not change when format externs join it. An axis the space does not declare fails with `ShimStorageFormatAxisUnknown`; an enum axis over another enum, over a `StorageFormat` of another module, a boolean axis, and a gated axis fail with `ShimStorageFormatAxisInvalid`. |
| `ShimCookTest` | Shim externs end to end (phase F steps F2.5 and F2.6). `tests/assets/Shims/ShimMaterial.slang` reads a buffer, a texture, and a sampler through shim externs in a `ParameterBlock` beside ordinary data, and takes a shim as an entry point `uniform`. `ShimControl.slang` is the same shader with the resource types written directly. Both cook for `wgsl` and `spirv` in one run, with `VerifyDeterministic`, and the test compares them by resource, sorted by group and binding, ignoring names (a shim resource is scope `material_Points`, name `data`): the same group, binding, kind, shape, access, element stride, array length, and byte size. A global that moved down a set, or a buffer that lost its element type, fails the comparison. `ShimLocal.slang` uses a shim extern as a function-local and must not cook (Slang asserts in its type flow pass). |
| `StorageFormatTest` | The storage texture formats that every target accepts (DiamondDogs request C11, phase F step C11.1). The `StorageFormat` enum in `builtins/LodestoneShims.slang` lists the six float formats (`Rgba8Unorm`, `Rgba8Snorm`, `Rgba16Float`, `R32Float`, `Rg32Float`, `Rgba32Float`) with a comment for the formats that are not accepted, and `target/StorageFormatTable` repeats them. The test proves that the table has six distinct formats, that the enum cases, which an enum axis reads through reflection, equal the table in order, and that `tests/assets/Shims/StorageFormatProbe.slang`, one `WTexture2D<Type>` for each format, cooks for `wgsl` and `spirv` and reads back as the `TextureFormat` of the table in both profiles. It also cooks `ShimStorage.slang` (phase F step C11.2), which puts `BoundWriteStorageTexture2D<Type>` of each format in a `ParameterBlock`: each is a write-only storage texture of its format on both targets. It cooks `ShimStorageAxis.slang` (phase F step C11.3), where a format axis over all six cases selects the format of a storage extern: six variants for each profile, so the axis survives pruning although only the attribute names it, and the variant of each case writes a storage texture of the format of the table. Agreement of the readers is not enough, because two readers that both give `Invalid` agree, so the test compares each reading with the table. |
| `StorageFormatAxisTest` | A storage format for each variant, end to end (DiamondDogs request C11, phase F step C11.4). `tests/assets/StorageFormatAxis/MipDownsample.slang` is one step of a mip chain: it reads the previous level through `ITexture2DRef` and writes the next through a storage shim extern whose format is the enum axis `LEVEL_FORMAT`, which no code reads. The test writes one control for each format, the same kernel with a literal `WTexture2D<Type>`, and cooks the subject and the controls for `wgsl` and `spirv` with `VerifyDeterministic`. For both targets and every format, the variant binds the same resources at the same places, with the same format, as its control (`tests/CookComparison.hpp`, shared with `ShimCookTest`). No variant names `StorageImageWriteWithoutFormat`, and a module with `[format("unknown")]` does, so the check can see it. `MipDownsample.toml` lists two case names in `CookValues`, and each profile then has those two variants only. |
| `ReflectionSchemaTest` | The pure data of the binding schema, with no Slang. The `ResourceShape` flag layout (a base shape in the low nibble plus array, multisample, shadow, and feedback flags, read with `GetBaseShape`), the `ToString` tables for `ResourceShape`, `BindingKind`, and `TextureSampleType`, and the `ReflectedUniformMember` equality that dedup rests on, which includes matrix layout and element stride. It also proves that every `CookError` band has names, above the 127 limit of magic_enum's default range. |
| `StageDumpTest` | A stage dump holds the model and no target text, it names itself the way `--dump-stage` names it, and two dumps of one input agree byte for byte. |
| `SymbolTableTest` | The tokenizer that powers axis-reachability pruning: it strips `extern static const` declarations and reserved keywords, and reports the axis names no reachable source uses. A comment declares nothing: a line comment, a block comment over one line or several, and an example in a doc comment are not read, and a string with two slashes is not a comment. |
| `PolicyDocumentTest` | The TOML policy reader, its query surface, and its validation of every axis name and value against the declared space. It also proves the four coverage answers (no module entry, no target sections, present, and other targets only) and the rejection of a target name the build does not know. `CookValues` of an enum axis are case names: a case the axis takes cooks alone, and a misspelled case, a case the axis does not take, and a name on a boolean axis each fail with `PolicyValueNotInAxis`. |
| `DedupeInfluenceTest` | Dedup changes what the tables cost and never what the cook measures. It also proves that an axis which changes only a buffer size, with the same code, is Active (`model/AxisInfluence` compares the whole entry point instance). It builds one module in both arms and checks that the axis influence agrees, and that the measurement reads every group of variants. |
| `DiagnosticParserTest` | The parser reads Slang's machine-readable diagnostic form into a record, and counts the records whose severity is a failure. Codegen reads that count. It names no Slang type, so it needs no compiler. |
| `ResolveStageTest` | Stage 4 resolves a hand-built `RawVariant` with no Slang present. This test is the proof that the stage 3 and stage 4 split worked, and before phase D step D5 it could not be written at all. |
| `AxisDefaultTest` | The declared default of each axis, from the shader initializer to `ManifestIndex::AxisDefault`. `tests/assets/AxisDefaults/AxisDefaults.slang` declares every default as a value other than the first, in all four domains, with a literal initializer (which reflection also reads, so the two readers are compared) and an expression initializer (which only the source text can read). It proves that a missing default and a default outside the declared values each fail the cook, and that a policy which drops the default cooks, keeps the default in the manifest, and leaves no variant for it. `AxisDefaultUnread.slang` declares an enum axis that no code reads: it is pruned, and the cook does not read its initializer as a value. |
| `DerivedConstantTest` | Derived constants in size expressions, from the shader source to the element count in the manifest. `tests/assets/DerivedConstants/DerivedConstants.slang` sizes a buffer with `LIGHT_SLOTS`, which uses another derived constant, two axes, and a constant from an `__include`d fragment. The test compares each variant's count with a hand-computed value, because nothing else checks it: reflection cannot fold the expression. It proves that a function-local constant with the name of an axis fails the cook, and that Slang rejects a cycle of constants before stage 4 runs (E39999). It also reads a `[ls_extent_1d]` width from the manifest (task L18), and proves that the footprint axes of both resources are `GRID_X` and `GRID_Y`, reached through the derived constants (task L16). |
| `AccessModelRejectTest` | The table of cooks that must fail. A pointer member under a bound access model, a reflection mismatch, a codegen error, an interface axis implementation that holds a resource, and four storage shim externs (no format axis, an axis typo that the error corrects, an axis over another enum, and a gated axis) each fail the cook with their own error. A control module still cooks, and so does `EntryPointUnusedResource.slang`, which needs the workaround for shader-slang/slang#13789. No row proves a reflection mismatch end to end now: the validator unit tests prove each mismatch kind. It also cooks into a folder on disk, and proves that a failed cook removes the manifest of an earlier cook and that each atomic write leaves no temporary file. |
| `SlangMetadataCanaryTest` | Shader-slang/slang#13789 still exists: Slang's metadata reports an unused resource beside entry point constants as used. When Slang fixes the issue, this test fails, and its header comment lists the workaround code to delete. |
| `EntryPointConstantsTest` | The entry point constants (task L14), from the shader source to the manifest, for both targets. `tests/assets/EntryPointConstants/EntryPointConstants.slang` declares a struct parameter with a resource and two scalars, loose parameters, data that is never read, and resources only. The test compares each delivery, byte size, and member with values computed by hand from std140 (the WGSL uniform buffer) and std430 (the SPIR-V push constant block), so the two targets of one variant differ in size. It proves that the WGSL constants resolve to the `entryPointParams` uniform buffer in the layout, that the resource in the struct is a binding on both targets, that unread data gives no constants, and that a 144-byte push constant block fails the SPIR-V cook with `EntryPointConstantsTooLarge` and cooks for WGSL. |
| `PermutationConstraintTest` | The axis constraint engine. `ActiveWhen` gates an axis the way the old parent link did, `Require` prunes a forbidden combination, and the load check rejects a forward reference, an unknown symbol, and a malformed expression. |
| `SuggestTest` | The nearest-name suggestion. It measures the edit distance between a mistyped name and the accepted names, and returns the closest one. The cooker and the client both use it for a name rejection. |
| `EnumTagDecodeTest` | The enum tag-blob decode (`compile/EnumTagDecode`). It reads each integer width, sign-extends a signed tag, zero-extends an unsigned tag, and accepts the `UInt64` wrap above 2^63. It names no Slang type, so the reflection read of an enum case value has a proof that needs no compiler. |
| `ManifestIndexTest` | The client query surface, end to end. It builds a bundle inline through the emitter, with one axis of each domain, then drives `ManifestQueryBuilder` through every valid construction and every error path, plus a sparse manifest and an `ActiveWhen`-gated one. On the gated manifest, a constraint on the gated axis selects only the variants where that axis is active, and `Decode` marks the inactive axis. It proves that two profiles of one module each key their own subset, and that two modules share a root axis only when their values agree in order. It also covers `AxisNames` and `AxisValues`, the axis relevance of each entry point, `SingleFor` (an irrelevant axis is not ambiguous, a pipeline joins the masks of its stages), and both failures of the relevance validator. It also proves the query presets: a `WhenActive` pin keeps the gated-off variants, a `Required` pin removes them, two modes on one axis merge, a pin on an undeclared axis gives a note and no error, a bad value is an error, `NotesFor` names a pin that cannot change the named entry points, and a repeated value gives a `DuplicateValue` note. It proves the device filter (`ForDevice`): a device without subgroups loses the wave instances of `GenerateCS` and still gets `BlurCS`, a device below the workgroup memory, the invocation limit, the size on one side, or the push constant bytes loses only the instances above it, each rejection names its reason, limits left at 0 reject everything, and a profile that read no facts fails with `DeviceFactsUnavailable`. It needs no Slang. |
| `ResourceRoleTest` | The resource roles and tags (task L17), from the shader source to the manifest, for both targets. `tests/assets/ResourceRoles/ResourceRoles.slang` puts each role on a buffer that fits it: the stride of the command, a 4-byte stride with an element count of whole commands, a raw buffer, and two roles on one buffer. The test reads each role and the creation usage (`ResolvedResource::RequiredBufferUsage`) back, and proves that tags in another order, with a repeat, read back sorted and unique. Six other assets each break one rule, and each cook fails with its own error: a role on a texture, a misspelled role, a stride that is not the command's, a count that is not whole commands, a 4-byte stride with no count, and a tag with a character outside the tag set. |
| `MergedLayoutTest` | The merged layout of one pipeline. It builds a bundle inline through the emitter, with a vertex, a fragment, and a compute entry point. It proves that a resource both stages read appears once with both stage bits, that the order is (group, binding), and that instances from two variants merge. It proves a conflict of kind and of byte size, a duplicate stage, and too many stages. It also proves that `Open` rejects a visibility list that is not in placement order, and that `SamplerTexturePairs` gives a pair that two stages sample once, with both stage bits. It proves that two push constant blocks merge into the larger one when one is a prefix of the other, with both stage bits, and that two blocks that differ at offset 0 fail. It proves that two resources at one placement that differ only in their tags conflict. It needs no Slang. |
| `ResourceReportTest` | The resource report (task L16), with no Slang. A hand-built module has an axis that changes one buffer's element count and an axis that changes nothing. It proves that the measurement finds the first axis and not the second, that a sparse cook with no one-axis pair is closed by the second pass, that `VerifyFootprintAxes` passes a declared axis and fails an undeclared one or a wrong one, and that a sparse cook does not fail it. It reads the masks back through the manifest, rejects a mask bit past the module's axes at `Open`, and proves the `DescribeCreation` answer for a counted storage buffer, a uniform buffer, a sampled texture, a storage texture, and a sampler. It reads the report from the written bytes and checks its entries and its text. |

An error check prints a diagnostic to `stderr` on purpose. Read the last line for the result.

The last seven are different. Each one is the cooker driver, and not an assertion suite.
`tests/CMakeLists.txt` gives each a command line through `TEST_ARGS`, and all seven build from
`CookTest.cpp`. Each one passes `--target=wgsl`, `--target=spirv`, and `--verify-deterministic`, so one
cook writes a bundle with two profiles. Exit code 0 there is a real statement: every variant compiled
for both targets, every reflection agreed with the emitted WGSL (Tint) and the emitted SPIR-V
(SPIRV-Tools), all three round trips read back the same bytes, and two cooks agreed byte for byte.

- `KitchenSinkCookTest` cooks the four KitchenSink modules together against
  `tests/assets/KitchenSink/KitchenSink.toml`. It is the end-to-end coverage of the permutation path:
  616 variants for each target, every axis domain, nested parameter blocks, and raster state. One cook
  of both targets takes 7 to 9 seconds in RelWithDebInfo, and the test takes about 18 seconds.
- `EntryPointParamsCookTest` cooks `tests/assets/EntryPointParams.slang`. Three of its four entry
  points declare a `uniform` parameter, and one of those takes a struct with an annotated field.
  Phase E step E0a needed it, and it is the acceptance test for the entry point scope walk. It cooks
  one variant in about half a second.

- `CrossModuleCookTest` cooks `tests/assets/CrossModule/lib/CmShared.slang` and `tests/assets/CrossModule/app/CmKernel.slang`
  in one run, with `--include-dir` for the `lib` folder. The kernel imports the shared module from a folder that is neither
  its own nor its parent. The shared module cooks first, so the kernel session can find it through the module cache, which
  the worker sessions cannot load from. It cooks two modules in about one second.

- `WorkgroupMemoryCookTest` cooks `tests/assets/WorkgroupMemory.slang`. Four `groupshared` variables pad
  differently (a `float3` array, a struct, a `float` array, a `half4` array), and both targets must read 144 bytes and
  a 16-bit float requirement. `VerifySourceFactsAcrossProfiles` fails the cook otherwise. It cooks one variant in
  about one second.

- `ParameterBlocksCookTest` cooks `tests/assets/ParameterBlocks.slang`. It holds a block of
  resources, a block of ordinary data, a block inside a block, and a block on each of two entry
  points. Phase E step E0b needed it. It cooks one variant in about half a second.

- `InterfaceAxisCookTest` cooks `tests/assets/InterfaceAxis/InterfaceAxisTest.slang`. It declares an
  interface axis (`extern struct SHADE_MODE : IShadeMode`) whose three implementations reach the module
  through an `__include`d fragment, crossed with a boolean axis for six variants. Phase E step E7 needed
  it.

- `EnumAxisCookTest` cooks `tests/assets/EnumAxis/EnumAxisTest.slang`. It declares an enum axis
  (`[ls_axis_enum]` on an `extern static const` of a `public` Slang `enum`) whose three cases hold
  explicit, non-ascending values, crossed with a boolean axis for six variants. The manifest stores the
  case names, and the digit stays the declaration order. Phase E needed it.

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
that `tests/CMakeLists.txt` supplies.

`ParseCommandLine` in `src/driver/CookerOptions.cpp` parses the flags, and `GetUsageText` prints them:
`--output/-o`, `--cache-dir`, `--include-dir/-I <path>` (repeatable),
`--O0` to `--O3`, `--target=<name>`, `--no-validate`, `--quiet`,
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
`WgslValidator` (Tint) and `SpvValidator` (SPIRV-Tools).

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
bindings it declares (a fixed-size binding array is one binding with its length) and the sampler and texture pairs it samples. `SpvValidator` compares what the reader
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

Eight validators run inside that loop. None of them is a stage.

- **The reflection cross-check**, after stage 4. `ReadEmittedCode` (in `BuildModuleStep.cpp`) asks the target
  profile for a validator. The WGSL profile parses the emitted text with Tint (`WgslValidator`) and reads the
  bindings from Tint's inspector, then compares that against the bindings the entry point uses. Tint
  also proves the emitted WGSL parses and resolves, which the old text scanner could not. A mismatch
  increments a counter, and a nonzero counter fails the cook with `CookError::ReflectionMismatch`. A
  target that supplies no validator fails the cook with `TargetValidatorUnavailable`. `--no-validate`
  skips the check, and only on purpose.

  The same parse also produces data (task L7): the sampler and texture pairs of each entry point. Reflection
  cannot state which sampler a texture is sampled with, so the emitted code is the only source. Tint's inspector
  gives the WGSL pairs (`GetSamplerTextureUses`), and the SPIR-V reader follows each `OpSampledImage` back through
  the loads to its variables. `ResolveSamplerTexturePairs` maps each placement to a position in the entry point's
  used bindings, and fails with `ReflectionSamplerPairUnresolved` when reflection has no texture or sampler there.
  The pairs are cook output, so `--no-validate` still reads them (`ReadEmittedCodeFacts`, no comparison). A
  profile with no reader cooks with no pairs, and the manifest records that they were not read.

  The same read settles the entry point constants (task L14): the `uniform` parameters of ordinary data. Slang
  puts them in a uniform buffer named `entryPointParams` on WGSL, which stage 3 drafts as a binding, and in a push
  constant block on SPIR-V, which has no placement. Slang's metadata does not track push constants, so stage 3
  states a block whenever the layout has data, and `ReconcileEntryPointConstants` (in `BuildModuleStep.cpp`) clears
  it when the SPIR-V declares no block, because Slang removes data that the entry point does not read. A block
  that reflection does not state, or that covers more bytes than reflection's size, is a `ReflectionMismatch`. A
  block above the profile's `MaxPushConstantBytes` (128 for `spirv`, the Vulkan minimum) fails with
  `EntryPointConstantsTooLarge`.

  A known limit, measured on 2026-10-19: when an entry point has ordinary data, Slang's metadata marks every slot of
  the parameters' constant buffer as used once any part is used (`collectMetadataFromInst` in
  `slang-ir-metadata.cpp` inserts the whole slot range of the parameter). A resource parameter beside that data
  that the entry point does not read is then visible to reflection and absent from the code. Reported as
  shader-slang/slang#13789. `ApplySlangWorkaround13789` (in `BuildModuleStep.cpp`) removes such a resource from the
  visibility when the emitted code does not declare it, and prints a warning for each one. It reads only the
  resources in the constants element (`ConstantsElementBindings`), so every other binding keeps the full
  cross-check. `SlangMetadataCanaryTest` fails when Slang fixes the issue, so the workaround cannot outlive it.
- **The sampler pair cross-check**, after the last module and before the emit. `VerifySamplerPairsAcrossProfiles`
  (in `model/CookedLibrary.cpp`) compares the pairs of each variant that two profiles cooked, by resource scope
  and name. The two readers are independent, so agreement is a second opinion. It skips an entry point with a
  combined texture sampler, which SPIR-V samples with no `OpSampledImage`. A difference fails the cook with
  `SamplerPairsTargetMismatch`.
- **The source facts cross-check**, next to it. `VerifySourceFactsAcrossProfiles` (in `model/CookedLibrary.cpp`)
  compares the workgroup memory bytes, and a portable subset of the capability names, of each entry point that two
  profiles cooked for one variant. The map is `f16` with `Float16`, and `subgroups` with the `GroupNonUniform` family.
  A name that only one target spells (`StorageInputOutput16`, `DrawParameters`) is not compared. A profile with no
  reader (`CookedProfile::CodeFactsRead` false) is skipped, because it stores empty facts. A difference fails the
  cook with `SourceFactsTargetMismatch`. Both cross-profile checks share `VisitSharedVariants`.
- **The capability axis check**, next to it. `VerifyCapabilityAxes` (in `model/AxisInfluence`) reads the capability
  names that each profile's reader measured, for each axis of kind `capability` that no other axis gates. A one-axis pair
  must differ in the names of some entry point, or the cook fails with `CapabilityAxisInert`: the "off" path still needs
  what the axis claims to turn on. For a boolean axis the off variant's names must be a subset of the on variant's
  (`CapabilityAxisInverted`). The author declares nothing but the kind. A gated axis (a wave size) refines another axis and
  is not checked, and a profile with no reader (`CodeFactsRead` false) is skipped. An axis that controls two capabilities
  and leaves one of them in the off path still passes, because the pair differs in the other one.
- **The footprint axis cross-check**, after stage 7. `VerifyFootprintAxes` (in `model/AxisInfluence`) compares the
  axes that change each resource's footprint, measured from the cooked tables, with the axes that its size
  attributes name. Stage 4 reads the named axes from the expression text, through the derived constants
  (`CollectDeclaredFootprintAxes`), so the two sides share no code. Only an axis that a one-axis pair proves is
  compared, because the closing pass can mark an axis that a sparse cook cannot separate. A proven axis that no
  attribute names fails the cook with `FootprintAxisUndeclared`.
- **The library round trip**, after stage 7. `VerifyLibraryRoundTrip` replays every variant through
  the finished tables and compares the result against the text the compiler produced. It covers the
  **source text** only.
- **The layout round trip**, after stage 7. `VerifyLayoutRoundTrip` replays every (variant, entry
  point) through the resource, footprint, and visibility tables and compares the result against
  `BuildEntryPointLayout`. Phase D step D8b built it, because the step that collapses the layout
  tables owes the repository a check that a collapse was correct. `EnforceModulePolicy` then checks
  the measured axis influence against the `ModulePolicyEntry` the driver read from the policy file.
- **The manifest round trip**, inside stage 8. `VerifyManifestRoundTrip` reads each manifest back and
  compares it against the module it came from. `CheckSourceFacts` reads the capability names of each source back by
  their text, and its workgroup memory (`ManifestSourceFactsMismatch`). `CheckManifestLayout` walks the manifest the way a
  consumer walks it, from variant to resource list to footprint list to visibility list, and compares the
  sampler and texture pairs (`ManifestSamplerPairMismatch`).
  `VerifyAxisRelevance` derives the axis relevance a second time from the written environment alone, through
  the client views, with both passes of `model/AxisInfluence`, and compares content, not record indices. It
  fails the cook when the derived mask differs from the stored one (`ManifestAxisRelevanceMismatch`).

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

Each module axis records the default that the shader declares, as `ModuleAxis::DefaultDigit`, a digit of the
module's own key (format version 8). It is per module and not on the root axis, because two modules can
share a root axis and declare different defaults. The cook reads the default from the initializer text
(the symbol table), and compares it with Slang reflection where reflection can read it: reflection returns
literal initializers only, and never the `= Impl` of an `extern struct`. An axis with no readable default
fails the cook. The declared default is not the canonical value: canonicalization still fills an unused
axis with the first value (`PermutationAxis::GetCanonicalValue`), so the default does not change any key.

Each environment records the axis relevance of each entry point (format version 9, task L3): one bitmask per
entry point, in the word layout of the axis-active mask, at `EnvironmentHeader::AxisRelevanceTableOffset`.
Bit k is set when axis k can change that entry point's instance: its source, its layout (each visible resource
and its footprint), its raster state, or its workgroup size. `model/AxisInfluence` measures it from the frozen
tables by content, so `--no-dedupe` gives the same mask. The source alone is not enough: an axis that appears
only in a size attribute changes a footprint and leaves the code the same. An axis with one cooked value is
Undetermined and counts as relevant. The emitter computes the mask from the module it writes, so a hand-built
`CookedModule` gets a correct mask. The validator `VerifyAxisRelevance` (see the data flow section) derives it
again from the written bytes. The client reads it with `EnvironmentView::AxisRelevance` and `IsAxisRelevant`,
`ManifestQueryBuilder::SingleFor(entry_points)` uses it, and `manifest_dump` writes a `"relevantAxes"` object
for each environment.

The measurement has two passes. The first compares variants that differ in one axis. That proves that a class
of variants which agree on the relevant axes holds one instance only along chains of cooked variants, and a
sparse policy can remove a link: cook only "all off" and "all on" on one quality level, and an interaction such as
`if (!LOW_QUALITY && FOG && SHADOWS)` has no one-axis pair that shows it. So the second pass closes the mask: it
groups the variants by their relevant axes, and in each group that holds more than one instance it marks every
axis that varies within the group, until no group does. The rule marks a set and never picks a pair, so the
cooker and the validator reach the same mask in any variant order. A smaller mask can exist, but it is not
unique, and a mask that is too large only costs pins. `SingleFor` rests on the closed property.
`ManifestIndexTest` builds such a cook, and the KitchenSink masks did not change.

Each entry point instance records its sampler and texture pairs (format version 10, task L7). A pair holds two
positions in the instance's visibility list, a texture and then a sampler, so `Layout()[pair.Texture]` resolves it.
The lists are interned like the visibility lists (`EnvironmentHeader::SamplerPairLists` runs over
`SamplerTexturePairs`, and `EntryPointInstance::SamplerPairs` indexes them). `Open` checks each position, the
binding kind at each, and that each list is sorted and unique. `Profile::Flags` holds
`ProfileFlags::SamplerTexturePairsRecorded`: a clear bit means nothing read the pairs, which is a different fact
from an entry point with no pairs (`EnvironmentView::HasSamplerTexturePairs`). The pairs are part of an entry point
instance, so the axis relevance and its validator compare them. `MergedLayoutView::SamplerTexturePairs` gives the
union for a pipeline, each placement pair once, with the stages that sample it. That is the input of the WebGPU
layout rule (a sampler paired with a texture that cannot filter must be non-filtering), and a Vulkan renderer can
check a format's filter support with it before it binds. Pairs come from static use: a dynamic index pairs the
array bindings, and a future bindless access model can hide a pair from the code.

Each entry point instance records its entry point constants (format version 11, task L14): the `uniform`
parameters of ordinary data, in the layout of the profile's target. `EntryPointInstance::Constants` indexes the
interned `EntryPointConstants` records (`EnvironmentHeader::EntryPointConstants`). A record holds the delivery
(`EntryPointConstantsDelivery`: `None`, `UniformBuffer`, or `PushConstant`), the byte size, a run of uniform
members, and for a uniform buffer the position of that buffer in the visibility list. The client reads it with
`EntryPointInstanceView::Constants()`. The size and the offsets follow the target's rules (std140 for the WGSL
uniform buffer, std430 for the SPIR-V push constant block), so the two profiles of one variant can differ. A
renderer writes a buffer for `UniformBuffer`, and declares a push constant range from offset 0 for `PushConstant`.
`Open` checks each record and each uniform buffer position. The constants are part of an entry point instance, so
the axis relevance and its validator compare them. Slang puts the push constant block of each SPIR-V stage at
offset 0, so the blocks of one pipeline overlap. `MergedLayoutView::PushConstants` gives one block for the
pipeline: the largest block, and the stages that read any block. `Merge` fails with
`MergedLayoutPushConstantConflict` when the members of the shorter block are not the first members of the longer
block. A uniform buffer delivery is a resource, so the resource merge checks it.

Each binding records its roles and its tags (format version 12, task L17). A role (`[ls_resource_role]`) is a usage
of a buffer outside of its shader bindings: `IndirectDispatch`, `IndirectDraw`, `IndirectDrawIndexed`, `IndexData`,
or `VertexData` (`ResourceRoleFlags` and `k_ResourceRoleNames` in `ResourceFlags.hpp`). Stage 4 checks each role:
the binding must be a structured or raw storage buffer, an indirect role needs the stride of its command (12, 16, or
20 bytes) or a stride of 4 with an `[ls_element_count]` of whole commands, `IndexData` needs a stride of 4, and
`VertexData` a stride that is a multiple of 4. A raw buffer counts as a stride of 4. A misspelled role gets the
nearest name. `IndirectCount` waits for the capability work of phase F, because WebGPU has no count buffer. A tag
(`[ls_resource_tags]`) is free text that the cooker does not interpret: letters, digits, and `_.:-`, sorted and
unique. Both are fields of `ReflectedBinding`, so they take part in the interning key, the axis relevance, the round
trips, and the merged layout (a difference is a `MergedLayoutConflict`). `Binding::Roles` holds the role bits, and
`Binding::FirstTag` and `TagCount` are a run of string indices in the `ResourceTags` table. The client reads them with
`ResolvedResource::Roles`, `TagCount`, `Tag`, and `HasTag`, and `RequiredBufferUsage` joins the usage of the binding
kind with the usage of the roles. Copies, readback, and render attachments stay with the graph.

Each binding record has a footprint axis mask (format version 13, task L16): `EnvironmentHeader::FootprintAxisTableOffset`,
one mask for each binding record in the word layout of the axis-active mask. Bit k is set when axis k changes the
resource's element count or extent. `model/AxisInfluence` measures it by scope and name with the two passes of the
axis relevance, and a variant that does not declare the resource does not take part. Before the closing pass an
Undetermined axis counts as Inert: one cooked value changes nothing, and a sparse cook with no one-axis pair is then
closed by the second pass. The client reads it with `ResolvedResource::FootprintAxes` and `IsFootprintAxis`.
`DescribeCreation` gives a `CreationFields` value for a resource: for each creation field (size, usage, dimension,
format, extent, layers, mips, samples, sampler state), whether the shader fixes it, fixes it for each variant, or
the renderer must supply it. A resource with no `Cpu` field is complete. `ShaderLibrary.resources.txt` prints it
for every resource that an entry point reads, and counts the complete buffers only: no shader states a texture's
mip count, so a texture is never complete. The report keeps its data (`ResourceReport`) apart from its text, so a
JSON form is a second writer over the same data.

Each source records what a device needs to run it (format version 14, phase F slice F3.1, task L5). The target's
reader reads it from the emitted code, so equal code gives equal facts, and the cook stores them once for each unique
source (`CookedModule::SourceFacts`, parallel to `Sources`):

- The capability names, in the names of the target: each SPIR-V `OpCapability` and `OpExtension`, or each WGSL
  `enable` directive. Each environment holds one sorted name table (`EnvironmentHeader::CapabilityNames`, string
  indices), and each source a mask over it (`SourceCapabilityTableOffset`). A name that the profile's Vulkan version
  enables alone (`Shader`, for the Vulkan 1.2 floor of `spirv`) is not stored. A SPIR-V alias is stored by its primary
  name, the one `vk.xml` uses.
- The workgroup memory bytes (`SourceRef::WorkgroupMemoryBytes`): for each workgroup variable `RoundUp(16,
  RoundUp(align, size))` with the std430 layout, summed. The WGSL reader takes it from Tint's
  `core::ir::GetWorkgroupInfo`, the value Dawn checks against `maxComputeWorkgroupStorageSize`. The SPIR-V reader lays
  out the `Workgroup` variables itself. The rule is an upper bound of the Vulkan rule (Vulkan-Docs `shaders.adoc`,
  `workgroup-padding`).

Format version 15 adds `ProfileFlags::CodeFactsRead`: a clear bit means no reader ran for the profile, which is a
different fact from a source that needs nothing (`EnvironmentView::HasCodeFacts`). `manifest_dump` writes the facts
only when the bit is set.

The client reads them with `EntryPointInstanceView::RequiredCapabilities`, `RequiresCapability`, and
`WorkgroupMemoryBytes`. `UniformMemberRange::Find(path)` finds a uniform member by its whole dotted path, with no suffix matching. `EntryPointInstanceView::Fit(supported, limits)` (header `ShaderManifestDevice.hpp`) tells whether a
device can run one instance, and returns the first reason when it cannot (`DeviceFit`, `DeviceMismatch`): a missing
capability, the workgroup memory, the invocations, the size on one side, or the push constant bytes. The device is a
capability predicate and a `DeviceLimits` value. The predicate is a plain function pointer with a user pointer
(`CapabilitySupportFn`), so no lambda or template crosses the library boundary, and `EnvironmentView::MakeDeviceCapabilities`
turns it into a mask over that environment's name table. It allocates nothing and is called twice, as in Vulkan: an empty
span returns the word count, and a span of that size is filled. Reading code in the client never allocates, so the
builder (which does) owns the vector. `ManifestQueryBuilder::ForDevice` stores the mask and the limits in
the builder. `client/include/VulkanCapabilityTable.hpp` maps each SPIR-V name to the Vulkan support that
permits it: any one of a version, an extension, a feature (struct and member), or a property bit. It names no Vulkan
type. `scripts/generate-vulkan-capability-table.py` writes it from `vk.xml` and `spirv.core.grammar.json`, and the file
is checked in. `Variant::CapabilityRequirement` and `Profile::CapabilityFloor` are now reserved fields.

Each profile records its `ShaderCodeFormat` (since format version 7). A SPIR-V source is read as words, so
`Open` rejects a SPIR-V source that is not on a 4-byte boundary or not whole words. The sources are packed
with no padding: every SPIR-V module is whole words, so each offset stays aligned. Codegen fails a SPIR-V
payload that is not whole words or has no SPIR-V magic number.

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
  do not link to it.
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
