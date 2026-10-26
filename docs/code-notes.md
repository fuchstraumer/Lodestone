# Code notes

The techniques behind the code, for the questions that the code cannot answer by itself. The comment compression of
2026-10-21 moved these explanations out of the source. Each section links to the functions it explains. Code comments
do not link back to this file, because `docs/` is gitignored. Force-add it: `git add -f docs/code-notes.md`.

`CLAUDE.md` has the architecture: the stages, the validators, and the manifest format by version. This file goes one
level down, to the reasons behind single functions.

---

## 1. Reflection (stage 3)

Files: [SlangReflector.cpp](../src/compile/impl/SlangReflector.cpp), [SlangCompilerTypes.hpp](../src/compile/impl/SlangCompilerTypes.hpp).

**Three scopes, three walks.** The global scope, the parameter scope of each entry point, and the element of each
`ParameterBlock` each need a different call. `collectBindingRangeDrafts` walks the binding ranges of one scope.
`collectSubObjectDrafts` walks the parameter blocks. Slang describes a block to its parent with descriptor ranges only,
so the block's binding range has a descriptor set index of -1, and the range walk skips it. The sub-object walk reads
exactly the ranges that the range walk skips, so the two walks partition the ranges and no range is drafted twice.

**Parameter block numbering** (`ReadParameterBlock`). The bindable contents of every block start at binding 0, because
their range offsets count from the start of the block's space. A block that also has ordinary data gets a constant
buffer at binding 0, and its resources move up by one: a block of two resources gives bindings 0 and 1, and the same
block with a `float4` gives 1 and 2. `readBlockContainer` drafts that constant buffer with the block's name, and only
when `UniformSize` is not zero. A block of resources alone has no such buffer.

**`BindingScope`.** `Base` is the placement of the bindings declared directly in a scope. `SpaceBase` is the first space
of the parameter blocks declared in it. They are different numbers: one probe reported an entry point scope with a slot
offset of 0 and a sub-element space offset of 1, and its block used space 1. A nested block counts its space from the
space of its parent (`block.Scope.SpaceBase = block.Scope.Base.Group`).

**`entryPointParams`.** Slang moves the loose `uniform` parameters of an entry point into a synthetic struct.
`slang-ir-entry-point-uniforms.cpp` writes the name `entryPointParams` as a name hint, and reflection never reports it.
When the entry point has one struct parameter, the scope name is that parameter's name (`entryPointScope`).

**Used bindings per entry point** (`CollectUsedBindingIndices`). Slang generates each entry point as its own artifact,
so two entry points can place different resources at one (group, binding). The metadata query runs for each entry point
against that entry point's own row, or one entry point could claim another's parameter.

**`SLANG_UNKNOWN_SIZE`** (`ResolvedNumber`). Slang returns it when a value depends on an unresolved generic or a
link-time constant. Every placement read goes through `ResolvedNumber`, because a cooked unknown value would place a
binding where no shader uses it.

**Sample types.** `getType()` on a texture returns the texture itself, and its scalar type is invalid. The sample type is
`getResourceResultType()`. A depth texture reports `float` until the shape says shadow.

**Attribute strings.** A size travels as a string, because Slang folds an integer attribute argument at compile time and
the permutation constants are `extern static const`, which fold at link time. `[ls_element_count(IFFT_SIZE * 4)]` does
not compile. A string reaches reflection untouched, and stage 4 evaluates it per variant. User attributes have no
optional parameters, so each arity has its own name (`ls_extent_1d`, `_2d`, `_3d`).

## 2. Entry point constants and shader-slang/slang#13789

Files: [SlangReflector.cpp](../src/compile/impl/SlangReflector.cpp) (`readEntryPointConstants`),
[BuildModuleStep.cpp](../src/driver/steps/BuildModuleStep.cpp) (`ReconcileEntryPointConstants`,
`KeepDeclaredBindings`).

**Delivery.** The range type of the parameters' constant buffer gives the delivery. A `ConstantBuffer` range is a
uniform buffer with a placement (WGSL). Stage 3 drafts it as a binding named as the target code names it, so the
cross-check and the layout read it like any other binding. A `PushConstant` range has no placement (SPIR-V).

**Settling the block.** Both targets remove the data when the entry point does not read it. The metadata reports that
for the uniform buffer only: `isParameterLocationUsed` reports no push constant (measured 2026-10-19). So stage 3 states
a push constant block whenever the layout has data, and `ReconcileEntryPointConstants` clears it when the SPIR-V declares
no block. A block that reflection does not state, or that covers more bytes than reflection's size, is a
`ReflectionMismatch`. A block above `MaxPushConstantBytes` fails with `EntryPointConstantsTooLarge`.

**slang#13789.** `collectMetadataFromInst` in `slang-ir-metadata.cpp` marks the whole slot range of the parameters'
constant buffer as used once any part is used. A resource beside that data that the entry point does not read then shows
in reflection and is absent from the code. Until 2026-10-25 a narrow workaround removed such a resource. Now
`KeepDeclaredBindings` covers it with every other binding the code does not declare (section 14). The canary
`SlangMetadataCanaryTest` stays, so that the fault cannot be forgotten. When it fails, delete the test.

**Used bindings come from the code (2026-10-25).** Slang computes `IMetadata` from its IR before `slang-emit.cpp` gives
the SPIR-V to spirv-opt (`slang-glslang.cpp`). Above `--O0` the optimizer inlines, folds constants, and removes dead
branches and variables that Slang kept, for example a branch on an interface method that returns a constant `false`
(DiamondDogs `NvproPyramid`, KitchenSink `Albedo` at `--O1`). `KeepDeclaredBindings` keeps only the bound bindings that
the code declares (`EmittedCodeFacts::DeclaredPlacements`), and the values stored in a removed uniform buffer. Entry point
constants in a uniform buffer name it by position, so the position is computed again. Slang runs no optimizer on WGSL.

## 3. Axis declarations

Files: [SlangModuleContextAxes.cpp](../src/compile/impl/SlangModuleContextAxes.cpp),
[EnumTagDecode.cpp](../src/compile/EnumTagDecode.cpp).

**Fragments.** An `__include`d file (with `implementing`) reflects as an Unsupported node, kind 0, and its children are
the real declarations. `collectAxesFromDecl` descends into it. `getType` on that node crashes, and
`getDeclSourceLocation` fails inside it, so a source location is best effort: an axis with no location is still valid.

**Enums across modules.** `getScalarType()` returns `None` for an enum reached through an `import`, but the case value
blob still has the tag at its real width. `EnumTagKindFromByteWidth` reads the width, and assumes signed, because Slang
enums default to `int`. The decode itself (`DecodeEnumTag`) has no Slang type, so it is unit tested without a compiler.
A `UInt64` tag at or above 2^63 wraps negative, which the size expression evaluator accepts.

**The declaring module** (`FindDeclaringModule`, `BelongsToModule`). The synthetic module of each enum value imports
the module that declares the enum, which can differ from the axis variable's module. The search matches by pointer:
Slang interns one `TypeReflection` per type per session, across module boundaries. `DeclReflection::getType()` returns
the declared type for a type declaration and a wrapper for a variable, so only the declaration matches.

**Defaults.** The source text is the primary reader of an axis initializer. Reflection returns literal initializers only
(`ReadReflectedDefault`), so no value means "unreadable", not "no default", and reflection is a second opinion where it
can read a value. `true` and `false` are not expression keywords, so they are read directly.

**Interface axes.** No API lists the types that conform to an interface, so `stageInterfaceStruct` walks the decls and
tests each struct with `isSubType`. A conformance cannot carry a resource member: binding layout is fixed before
link-time specialization, and codegen fails with E56003.

## 4. Axis influence and relevance

File: [AxisInfluence.cpp](../src/model/AxisInfluence.cpp).

**Instances.** An axis influences an entry point when two variants that differ only in that axis give two different
instances. An instance is everything a consumer reads for one (variant, entry point): the source, the layout (each
visible resource with its footprint), the raster state, the workgroup size, the sampler pairs, and the entry point
constants. The source alone is not enough: an axis that appears only in a size attribute changes a footprint and leaves
the code the same. The capabilities and the workgroup memory follow from the source, so they add nothing.

**First pass** (`MeasureAxisPairs`). For each axis, `OrderByOtherAxes` sorts the variants so that the ones that agree on
every other axis are adjacent, and each run is a group. A group with two instances proves the axis Active. An axis with
no such group is Undetermined. The loop is a `while`, because `begin` already moves to the next group, and a `for`
header would skip that group's first element. `end` is read inside the loop condition, because on the last group it is
out of range until the range test runs.

**Closing pass** (`CloseRelevance`). The first pass proves that variants which agree on the relevant axes hold one value
only along chains of cooked variants, and a sparse policy can break a chain. Example: cook only "all off" and "all on"
of an interaction `if (!LOW_QUALITY && FOG && SHADOWS)`, and no one-axis pair shows it. The closing pass groups the
variants by their relevant axes, and in each group with more than one value it marks every axis that varies within the
group, until no group has one. Each round marks at least one axis, because two variants with different values that agree
on every relevant axis differ in some other axis. The rule marks a set and never picks a pair, so the cooker and the
validator reach the same mask in any variant order. A smaller mask can exist, but it is not unique, and a mask that is
too large only costs pins. `SingleFor` rests on this property.

**Footprints** (`ComputeFootprintInfluence`). The same two passes run over (scope, name) identities instead of entry
points. Before the closing pass, Undetermined counts as Inert: one cooked value changes nothing, and a sparse cook with no
one-axis pair is then closed by the second pass. `VerifyFootprintAxes` compares only `PairAxes`, because the closed set
can hold an axis that a sparse cook could not separate from the one that does.

**Content, not indices.** Every comparison reads content, so `--no-dedupe` gives the same answer. Hashes pick
candidates (`HashInstance`), and `InstancesEqual` decides.

## 5. Interning

Files: [ContentInterner.hpp](../include/model/ContentInterner.hpp), [CookedLibrary.cpp](../src/model/CookedLibrary.cpp).

Three rules: a hash finds a bucket and the bytes decide, and a collision keeps both entries and is counted; every entry
keeps the list of inputs that mapped to it (provenance); `Disable()` gives every input its own entry, and the output must
be correct in both modes. `FreezeModuleTables` consumes the builder and discards the provenance, so only the Interned
stage dump can show which variants collapsed together.

Resources are interned per variant, not per entry point: four resources for four variants of two entry points, while the
sources stay at eight. `SourceFacts` runs parallel to the source table, and the first variant that gives a source gives
its facts, because the code alone decides them.

## 6. Stage 4: derived constants and expressions

Files: [ResolveStage.cpp](../src/model/ResolveStage.cpp), [AttributeExpression.cpp](../src/permute/AttributeExpression.cpp),
[SymbolTable.cpp](../src/compile/SymbolTable.cpp).

**Symbol table of one variant.** The axis values (from the canonical assignment, so every axis is nameable when a
dependent axis is off), the undriven extern constants at their defaults (only Slang knows them, so stage 3 carries them
in `RawModule::ExternDefaults`), and the derived constants that a size attribute reaches.

**`DerivedConstantWalk`.** Depth first from the size attribute arguments, through the identifiers of each expression,
with three colors. A gray constant reached again is a cycle. A black one is done. Each constant is evaluated when its
visit finishes (post-order), so its dependencies are in the table. A diamond evaluates once. Only reached constants are
evaluated, so an unused `static const float` is harmless. Slang rejects a real cycle itself (E39999), so the cycle check
fires only when the text reader and Slang disagree.

**Shadowing.** A derived constant with the name of an axis or an extern constant fails (`DerivedConstantShadowsSymbol`):
Slang and the table could each pick a different declaration. Two declarations of one name with different expressions
fail (`DerivedConstantAmbiguous`). A stable sort keeps the first declaration.

**Scope.** The symbol table scans lines and cannot tell scopes apart, so it also captures function-local constants.
`BuildModuleScopeVariableNames` reads the module-scope names from reflection, including `__include` fragments, and the
compiler filters the derived constants by that list. The reserved keyword list holds only reserved words and type names,
never intrinsics like `dot`: an author can shadow an intrinsic with an axis name.

**Expression semantics** (`ExpressionParser`). Every failure is an error, never a default: a size that evaluates to zero
allocates an empty buffer and fails later somewhere else, and a constraint that evaluates to zero prunes a variant the
author wanted. Comparisons and logical operators give 0 or 1, as in C and Slang, so `(IFFT_SIZE > 256) * 4` reads as
expected. `&&` and `||` do not short circuit, because the parser evaluates as it descends. So a divide by zero on the
right of a false `&&` still fails. `<=` is tested before `<`, and `parseShift` consumes every `<<` and `>>` first, so the
comparison level can trust that a single `<` or `>` is a comparison.

## 7. The SPIR-V reader

File: [SpvModuleReader.cpp](../src/target/impl/SpvModuleReader.cpp).

**Parse.** One flat record per id, sized from the header's id bound. `OpMemberName` and member decorations come before
`OpTypeStruct`, so they are deferred and applied after the parse (`ApplyPendingMembers`). Constants record raw bits.
`OpSpecConstantOp` and composites are marked derived, with no value. The words are copied, because names point into them
and the source alignment is unknown.

**Matrix layout.** Slang writes `ColMajor` in SPIR-V for what its reflection calls `RowMajor` (measured on KsGeometry
`ViewProjection`, 2026-09-26), so a comparison inverts it.

**Push constant extent** (`ReadPushConstantExtent`). The end of the last member, from the member offsets and the type
sizes, with no trailing padding.

**Workgroup memory** (`ReadWorkgroupMemoryBytes`, `LayOutStd430`). `Workgroup` variables have no layout decorations, so
the reader lays them out with std430: a three-component vector aligns as four, an array or matrix strides by
`RoundUp(align, size)` of its element, a struct rounds its size to its alignment, and a boolean counts as 32 bits. Each
variable counts `RoundUp(16, RoundUp(align, size))`. That is Tint's rule (`core::ir::GetWorkgroupInfo`, which Dawn checks
against `maxComputeWorkgroupStorageSize`), and an upper bound of the Vulkan rule: Vulkan-Docs `shaders.adoc`,
`workgroup-padding`, makes the size implementation-dependent but at most std430 in the worst order. So one number serves
both targets. Both readers give 144 bytes for the same four variables (`SpvValidatorTest`, `WgslValidatorTest`).

**Capability names.** `spv::CapabilityToString` can return an alias, such as `StorageUniform16` for
`UniformAndStorageBuffer16BitAccess`. `vk.xml` uses the primary name, so the reader stores
`vulkan::PrimaryCapabilityName`. Extension names start with `SPV_`, so they never collide with capability names.

**Binding arrays** (`ClassifyVariable`). A binding array is a variable whose pointer type points to one `OpTypeArray` of the
resource type, so the reader takes the length from the constant and classifies the element type, and `SpvValidator`
compares the length with `ReflectedBinding::ArrayCount`. A runtime array is the unbounded form (Indexed access model), and a
specialization constant length is refused: both stay unsupported. Vulkan allows no array of arrays here, and `spirv-val`
rejects one before the reader runs. Slang states no capability for a dynamic index into the array, so
`CollectDynamicIndexingCapabilities` reads each access chain on a binding array (`ChainAccess`) and adds the capability
name when the first index is not a constant. Texel buffer arrays are not covered.

**Capability axes** (`VerifyCapabilityAxes`). The check has no declared set of names: a measurement cannot say what an axis
should turn on, but it can say that an axis of kind `capability` turns on nothing. `ForEachOneAxisGroup` yields the groups of
variants that agree on every axis but one (the pair scan of the axis influence). In each group the names of an entry point
are compared as stored, sorted vectors in the target's own spelling, so no portable map is involved. A gated axis is skipped,
because a wave size refines its parent and changes no capability. For a boolean axis the group holds the two values, and
`std::ranges::includes` tests that off is a subset of on. The check passes an axis that controls two capabilities and leaves
one in its off path, because the pair still differs in the other. A declaration of names would catch it, and the author
refused one.

**Cross-profile facts** (`VerifySourceFactsAcrossProfiles`). Only a portable subset of the capability names can agree:
`f16` with `Float16`, and `subgroups` with every `GroupNonUniform*` name (Slang emits `GroupNonUniform` for any wave
operation, plus one name for each operation class). Names with no spelling in the other target, such as
`StorageInputOutput16` or `DrawParameters`, are not compared. The workgroup bytes compare as equal numbers, which holds
because both readers use the Tint rule. A profile whose reader did not run (`CookedProfile::CodeFactsRead` false) is
skipped.

## 8. The Vulkan capability table

Files: [generate-vulkan-capability-table.py](../scripts/generate-vulkan-capability-table.py),
[VulkanCapabilityTable.hpp](../client/include/VulkanCapabilityTable.hpp).

Each SPIR-V name in `vk.xml` lists `<enable>` entries, and a device needs any one: a version, an extension, a feature
(struct and member), or a property (struct, member, and a bit or value). A bitmask of feature names cannot hold that
"or", which is why the manifest stores SPIR-V names and the table maps them. `CoreVersion` is the lowest version that
enables a name alone, and the cook drops names at or below the profile's Vulkan version. The struct alternatives are
often one feature under two struct names (`VkPhysicalDeviceVulkan11Features` and `VkPhysicalDevice16BitStorageFeatures`).
The dependency closure of extensions is the renderer's job (DiamondDogs' `ExtensionWrangler`).

## 9. The manifest writer and its checks

Files: [ShaderManifestEmitter.cpp](../src/emit/ShaderManifestEmitter.cpp),
[ShaderManifestVerify.cpp](../src/emit/ShaderManifestVerify.cpp),
[ShaderManifestValidator.cpp](../client/src/ShaderManifestValidator.cpp).

**Determinism.** One string table serves the whole cook, so the call order decides every string index. The order is
fixed: module names, module shapes, profiles, extents. Every extent is built before the header region, because extents
add strings and the string table is part of the header region. The variant tables are written in key order whatever
order the cook appended them, because the reader binary searches the keys.

**Root axes** (`MergeIntoRootAxes`). A module axis shares a root axis when the name, kind, domain, and binding time agree
and the module's values land at increasing root positions. Digits follow the module's value order and set bits follow the
root order, so the two agree only then. The value mask is 32 bits, which is the hard limit on values per module axis.
The declared default is per module (`ModuleAxis::DefaultDigit`), because modules sharing a root axis can declare
different defaults.

**Alignment.** Every section starts on 8 bytes, because records have 64-bit fields and the reader maps them in place.
Records are trivially copyable with zeroed reserved fields, so equal bytes mean equal content, and a removed field
becomes `Reserved` rather than padding.

**Round trip.** `VerifyManifestRoundTrip` compares field by field against the model, never through the writer, so a
writer fault cannot hide. `CheckSourceFacts` compares capability names by text, so a wrong bit and a wrong name table
both fail. `--no-dedupe` makes member indices unique, so `ResourcesEqual` compares members, not indices.

**Open.** The validators run in an order where each may read what the earlier ones proved in bounds. Counts are divided
before they multiply, so a huge count cannot overflow. Placement order and pair positions are checked per instance, not
per list, because two variants can share a visibility list with different resource lists. Capability names are checked
against the string table only, so their sort order is the cooker's promise.

## 10. The client query

Files: [ShaderManifestIndex.cpp](../client/src/ShaderManifestIndex.cpp), [ShaderManifestQuery.cpp](../client/src/ShaderManifestQuery.cpp).

**Band narrowing** (`filterKeys`). Keys are a mixed-radix packing and sorted, so the smallest and largest allowed digit
of each axis give a [minKey, maxKey] band that every match fits in. A `WhenActive` constraint keeps digit 0 in the band,
because an inactive axis stores its canonical digit 0. `matches` tests the axis-active mask as well as the digit.

**Merging constraints.** (active and A) or (inactive or B) is (inactive or A or B), so the union of a `Required` and a
`WhenActive` constraint is `WhenActive` over both value sets.

**`SingleFor`.** Matches that differ only in axes that cannot change any named entry point give the same instances, so
they are one answer, and the smallest key keeps the answer stable between calls.

**The device filter** (`ForDevice`, `EntryPointInstanceView::Fit`). The mask is built from the builder's own environment,
because a capability index only means something in one environment's name table. The test order is capability, workgroup
memory, invocations, size on each side, push constants, and the first failure is the reason. Workgroup size checks run for
compute stages only: other stages carry the default 1x1x1. A rejected variant is removed for the whole query, so a pipeline
of two entry points that share a variant needs both to fit, and `SingleFor` is the way to ask about one.

## 11. Compilation, threads, and the module cache

Files: [ThreadPool.cpp](../src/compile/impl/ThreadPool.cpp), [SlangModuleContext.cpp](../src/compile/impl/SlangModuleContext.cpp),
[SlangCompiler.cpp](../src/compile/SlangCompiler.cpp), [EmbeddedFileSystem.hpp](../src/compile/impl/EmbeddedFileSystem.hpp).

**Thread pool.** Each worker primes its own session from the bootstrap's serialized modules. A worker whose setup fails
reports to its thread sink and keeps its thread, because the latch of each later batch counts it. A job is left over only
when every thread failed its setup, and it gets `WorkerSetupFailed`. The calling thread helps after dispatch. Setup
errors print before the job errors, so the cause shows before its effects.

**Module cache.** One writer, from the bootstrap blobs, after the workers start, so the write overlaps their setup. The
workers used to write too, all at once, while other sessions read the folder as a search path. Writes go through a
temporary file and a rename, so a reader never sees a partial file. On Windows an open reader blocks the replace, which
costs one cache miss. The folder is named `<Slang build tag>-<configuration>`, read from the loaded Slang library: Slang
rejects a module of another IR version (E00131), or crashes.

**Embedded builtins.** The builtins are served from memory under `k_BuiltinSearchPath`, through an ordinary import. A
builtin loaded with `loadModuleFromSourceString` broke every worker session with no diagnostic. The file system
implements `ISlangFileSystemExt`, because Slang wraps a plain `ISlangFileSystem` in a cache that reports a content hash
in place of a path.

**Codegen errors.** Slang can report an error and return a success code: E55205 for a specialization constant in
`numthreads` on WGSL gave `@workgroup_size(1, 1, 1)`. Codegen fails on any failure record.

## 12. Diagnostics

File: [SlangDiagnosticParser.cpp](../src/compile/SlangDiagnosticParser.cpp).

The machine-readable form is one tab-separated record per line, field order from `slang-rich-diagnostics-render.cpp`.
The split takes exactly the field count, so a tab inside the message cannot shift columns. Slang prefixes the message
that ended a compile with `abort compilation: ` and then repeats it as a record. An attachment with nothing above it
becomes a record of its own. A line that cannot be read becomes an "unreadable" record: the parser never loses a line.

## 13. Output

Files: [OutputSink.cpp](../src/emit/OutputSink.cpp), [DepFile.cpp](../src/emit/DepFile.cpp).

Each artifact is written to a temporary file and renamed into place, and the size is read back, so a write the stream
accepted and the disk lost fails. A cook removes the artifacts of an earlier cook before it starts. In the depfile, Make
reads spaces as separators and `#` as comments, so both take a backslash, and Make and Ninja spell a literal `$` as `$$`.

## 14. The validators

Moved from `CLAUDE.md` on 2026-10-25. Each bullet is one validator of the cook.

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
  shader-slang/slang#13789. Since 2026-10-25 no workaround is needed: the used bindings come from the emitted code
  (`KeepDeclaredBindings`, section 2), because spirv-opt removes bindings in the same way above `--O0`. A reflected
  binding that the code does not declare is not a mismatch. `SlangMetadataCanaryTest` still fails when Slang fixes the
  issue.

  A second Slang fault, measured on 2026-10-23: the direct SPIR-V emitter gives an array of write-only storage images no
  `NonReadable` (`maybeEmitWriteOnlyImageDecoration` in `slang-emit-spirv.cpp` does not unwrap the array, shader-slang/slang#13895), so the code
  declares `ReadWrite` and reflection reads `WriteOnly`. `IsSlangNonReadableArrayFault` (in `SpvValidator.cpp`) accepts
  that one difference, for an array of storage images only, and prints a warning. `SlangNonReadableCanaryTest` fails when
  Slang decorates the array.
- **Stored values in the reflection cross-check** (`spirv-modern`). `ResolvedLibraryValidator::ValidateEntryPoint` passes
  the stored values apart from the sorted bound bindings. `SpvValidator::CompareStoredValues` finds the container in the
  code (the `Block` uniform buffer at the stored group and binding, or the push constant block), walks the structs to the
  member that starts at the value's offset (`FindMemberAtOffset`), and requires the value's name and a `uint2` (Handle) or
  `PhysicalStorageBuffer` pointer (Pointer). A container the code removed is skipped. The pairs of handles come from
  `TraceHeapHandle`: from the heap access chain under an `OpSampledImage` operand, back through `OpCompositeExtract`,
  `OpLoad`, the copies, constant access chains, and the one store of a Function variable, to a block member. The byte
  offset is the sum of the `Offset` decorations on the way. Measured 2026-10-25: Slang loads the handle with one access
  chain at `--O2`, and copies the wrapper into a Function variable at `--O0`.
- **The delivered form check**, after the reflection cross-check of each variant. `VerifyDeliveredForm` (in
  `model/DeliveredForm.cpp`) runs only when the cook compiled a base form beside the delivered form. An optimizer only
  removes code, so the delivered form of each entry point instance must be a subset of the base form in one target: the
  used bindings, the sampler pairs (compared by binding index, since a position moves when a binding before it goes),
  the capability names, the workgroup memory, and the entry point constants (delivery and size, not the position).
  `SPV_KHR_non_semantic_info` is the one name the delivered form may add, for debug information. It is skipped for a
  variant with a reflection mismatch, which fails the cook already. A difference fails with `DeliveredFormExceedsBase`.
- **The sampler pair cross-check**, after the last module and before the emit. `VerifySamplerPairsAcrossProfiles`
  (in `model/CookedLibrary.cpp`) compares the pairs of each variant that two profiles cooked, by resource scope
  and name. The two readers are independent, so agreement is a second opinion. It skips an entry point with a
  combined texture sampler, which SPIR-V samples with no `OpSampledImage`. It reads the base form, so spirv-opt cannot
  remove a binding from one profile only. A difference fails the cook with `SamplerPairsTargetMismatch`.
- **The source facts cross-check**, next to it. `VerifySourceFactsAcrossProfiles` (in `model/CookedLibrary.cpp`)
  compares the workgroup memory bytes, and a portable subset of the capability names, of each entry point that two
  profiles cooked for one variant. The map is `f16` with `Float16`, and `subgroups` with the `GroupNonUniform` family.
  A name that only one target spells (`StorageInputOutput16`, `DrawParameters`) is not compared. A profile with no
  reader (`CookedProfile::CodeFactsRead` false) is skipped, because it stores empty facts. A difference fails the
  cook with `SourceFactsTargetMismatch`. Both cross-profile checks share `VisitSharedVariants`. Both read the base form
  and compare exactly. The delivered form can differ: measured 2026-10-25, KitchenSink `KsVolume` `GenerateCS` needs 0
  bytes in SPIR-V and 256 in WGSL at `--O1`.
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
