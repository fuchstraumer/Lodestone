# SlangCompiler
- Right now, we map to a super small subset of formats and features. We should support the full range, and extract them untouched
    - Then, during output format mapping we collapse to what that platform
    actually supports
    - Alternatively, use foresight about output target to fail builds if unsupported
# Resource Layer
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler,
  or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Permutation system
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- Let a size expression name a *derived* constant, not only an axis or an `extern static const`. Today the resolve namespace (`MakeResolveContext`) holds axis values plus captured undriven `extern` defaults, and nothing else. An `internal static const` that is computed from axes (e.g. `VTF_CLUSTER_COUNT = GRID_X * GRID_Y * GRID_Z`) is invisible, so `[ls_element_count("VTF_CLUSTER_COUNT")]` fails with `AttributeExpressionUnknownSymbol`. The author must inline the product or promote the constant to `extern`, which is a usability wart: the shader already states the relationship once, and we make them state it again.
  - Approach: capture each module-scope `const` name together with its defining *expression string* (not a folded value) during the same source scan that reads the extern defaults. The `SymbolTable` already tokenizes every source line (it absorbed the old `ExternConstantScanner`), so it is the natural place to grab these too.
  - Add them to the evaluator as derived symbols. When a name resolves to a derived symbol, evaluate its stored expression recursively against the same context, so the leaves bottom out at axes and extern defaults and the value tracks the per-variant axis values. A folded value captured from reflection would be wrong: it freezes at the declaration defaults and ignores the axes.
  - Guard against a cycle in the derived-symbol graph (a derived const that names another), and cache a name's evaluated result per variant so a diamond is not recomputed.
# Cook driver and manifest
- Resolve a synthetic module's import from the *declaring module of the root type*, not the axis
  variable's module. `RawAxisDeclaration::RootModule` (surfaced as `PermutationAxis::Module()`) is
  captured as the module the axis *variable* lives in, which is the enum/interface type's module only in
  the common case (the type declared alongside the axis or in the root module). An enum whose type is
  imported from a third module -- or an interface type declared apart from its extern -- would make the
  synthetic module import the wrong module and fail to resolve the type. Find the actual declaring module
  by reflecting the type itself (walk the type's `DeclReflection` parent chain to its enclosing module,
  or the equivalent reflection path) so both enum and interface axes resolve regardless of where the type
  lives. `Module()` was named generically (not `EnumModule()`) to anticipate the interface case wanting
  the same lookup. May also need the enum case to resolve to a fully module-qualified name for the
  literal. This is the general fix behind the current "enum/interface type must live in the axis
  variable's module" limitation.
- Test the enum tag-value read, which nothing checks today. `EnumAxisCookTest` cooks an enum axis but
  cannot observe the case values, because `StageDump` emits the qualified case name and the manifest
  stores names. So a sign or width bug in `ReadSlangEnumCaseBlobAs<T>` (`SlangModuleContext.cpp`) passes
  silent. Three pieces:
  - A pure unit test of the blob decode. Factor the dispatch Slang-free: map `slang::ScalarType` to a
    small local scalar-kind enum at the wall, and expose a `DecodeEnumTag(kind, bytes) -> int64` that
    names no Slang type. Test each width with a positive value, a negative value for each signed width
    (this checks sign extension), the maximum unsigned value, and a `UInt64` value above 2^63 (the
    accepted wrap case).
  - Surface the enum case values in a stage dump (raw or space), then add `EnumAxisTest` to the
    known-good regime, so the reflection read of `Low=5/High=1/Medium=10` is verified end to end.
    `check-known-good.py` cooks one module today, so this needs a second invocation or a small change to
    iterate modules.
  - Add an `Enum`-domain axis to the client `ManifestIndexTest`. Check that the `Domain` tag survives,
    the values resolve as string indices to names, decode returns the case name, a query by name
    resolves, and a mistyped case name gets a suggestion.
- Clean up cookerdriver. This is getting a bit ridiculous: it's a hugely complex.cpp, all the anonymous namespace functions are declared
  and defined together, it could all be condensed considerably. Might be worth using the FSM approach we tried in VeloxRhi, where we use
  a variant of discrete states and step through them. Then each state and it's functionality could go in a file, and it would help
  make control flow more clear.
- `VerifyManifestRoundTrip` was not running. `EmitLibraryModules` (which emits each manifest and runs
  the round trip) had no caller, so the manifest validator was silently dead, against rule 3 of
  `CLAUDE.md` ("the round trip checks are never optional"). It is wired back in now. Add a guard that
  fails loudly if a cook emits no manifest, so this validator cannot be orphaned again without a test
  going red.
- Verify that the cook actually wrote the files it claims. Part of this bug hid behind stale artifacts
  on disk, not missing ones: a reader saw an old file and thought the cook succeeded. After a cook,
  check with `std::filesystem::last_write_time` that each expected artifact is newer than the cook's
  start time, and that every expected artifact exists. `check-known-good.py` should also refuse to
  compare an artifact whose timestamp predates the cook it just ran.
- Own cook-scoped resources in a `CookContext`/`CookSession`. The `PermutationSpace` was a local in
  `CookModule`, but the interned module and then the cooked module hold pointers into it, so the
  pointers dangled the moment `CookModule` returned. The temporary fix owns the space in a `unique_ptr`
  reset between cooks. The real fix is an object that owns the per-cook resources (symbol table,
  permutation space, ...) for the span of one cook, with the pointer holders scoped to its lifetime.
  `CookerDriver` likely becomes `CookSession` in the process.
- Tear out the dead primary-content write. `first.GetContent()` / `sink.Write(...)` is the old
  compiled-in header path; the C++ emitter is gone, so the primary content is empty and the `-o` target
  is really an output directory that `FileOutputSink` still treats as a file name. Reconcile the sink
  around a directory target, and remove the empty primary write.
- Make `RunCookOnce` and the determinism path emit through one code path. The determinism branch
  replays memory artifacts to the real sink with its own `sink.Write`/`WriteArtifact` loop instead of
  the shared emit, so the two paths can diverge (they did). Route both through the same emit.
# Error reporting
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
# Found in review
- SlangCompiler.cpp, Line 1234: We extract the raw global bindings not once, but individually for each variant. We should be able to do this at a higher level, even if this specific variant doesn't actually use all of the entrypoints. We will need to identify further axes for data reuse like this to scale to much higher variant counts without terrible performance.