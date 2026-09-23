# Phase F preparation/edge case fixes
- Split Manifest into whole-cook and per-module data: all stored in one bundle, but sectioned separately.
  - String tables and source tables go in the root whole cook segment, along with any high-level metadata
  - We may want to consider storing all the axis names and their values here too. Then each module
    can use a similar tabled approach to what we've already used, an index into the root manifests axes
    table (sorted by declaration order), along with a mask of what values were active for that module
    [okay yes actually definitely do this]
  - To make the above more coherent, key root axes by name. That keeps consistency alive, where
    an axis like `QUALITY` means the same thing in two different modules
- Make test shader set not use OceanFft and such: create a complex multi-module shader set, which
  will let us test our multi-module functionality better. Create contrived source code, but make sure
  we're exercising all of our variant machinery and manifest module segmentation
- Add a per-variant mask over axes, specifying which axes are active in a variant. The bit index
  of a set bit gives the *local* index in *the current modules* axes span, which should fill in gaps
  considerably
  - This should probably be encoded upfront as a uint64_t, but have handling for cases where 
    we use more than 64 axes. That would suggest a kind of "chunk size" parameter, which can vary
    per-module, and then the accessors will have to read that to interpret the masks
- add query presets, which are module-agnostic
- make the query system very clearly single-module-only. it can reuse a preset across modules, though
- make it clear that variant keys are per-session use only. to save or persist a variant, save something 
  like `DecodedVariant` from the index, which can be expanded back into a valid variant key even if
  the manifest changed underneath us (invalidating previous keys)
    - a load of persisted variants will then need to first decode it's `Values` vector, to compute
      the key, and see if it matches. The `Key` field is somewhat redundant, I suppose, at least in
      persisted data
- Variant data will need expansion. Each variant should record what capability requirements it was 
  baked for, and member offsets for shader data must be recorded per target profile. Modules then
  become arrays of baked data for different access models. What should this segmentation look like?
  - Three scopes: whole-cook (strings, source, axes, target profiles), per-module logical
    (the variant table + ModuleAxis runs, profile-invariant), per-(module, profile) (baked layout,
    offsets, capability requirement). The query layer reads the logical scope only, so it stays
    profile-agnostic.
  - A profile is (target, access model, capability floor), a cooked form. Not a device power tier -
    device power is an axis picked at runtime. Store target even while WebGPU-only, so a later SPIR-V
    "bound" profile doesn't collide with the WebGPU one.
- Decide the variant key radix now: pack against the root value count (root radix), not the module's
  active subset. Decode stays "digit is position in the value list" and reuses the sparse-key handling.
  The active-value mask then stays metadata, off the decode path.
  - [2026-09-19 SUPERSEDED] We decided variant keys are per-module, not whole-cook global (see
    agent-handoff.md 9.5, and the `axis-name-scope-open-question` memory). A module keys in its own
    radix over its own axes. The root axes table then demotes from an identity mechanism to a storage
    and dedup optimization, and the per-module value mask stops being a decode-path reconciliation.
    Shared meaning across modules comes from shared types and query presets, not a global axis. So do
    NOT build root-radix keys as written above.
- Manifest is one file per cook, in the container-with-directory shape (agent-handoff.md 9.6). A fixed
  16-byte prefix (`uint32 magic`, `uint32 version`, `uint64 HeaderSize`) lets a reader load the header
  alone, then seek to one module or one profile. Keep a module directory (or fixed-size module headers)
  for an O(1) seek, and a per-profile directory inside each module header. Interior offsets are uint32
  today (4 GiB cap); decide whether to widen them against what the bundle stores.
- Per-module value mask: use uint32_t, not uint16_t. A hard 16-value cap is too low for a tuning axis.
  The mask width is an ABI limit. Put the "too many values" nudge in cook diagnostics (like the
  influence matrix), not in the format width.
- Placement must stop being group+binding. Bound is group+binding, pointer is a byte offset, indexed is
  a heap index. Model placement as a variant so the manifest carries all three access models. This is
  the field most likely to harden into a WebGPU assumption. Phase F D4, cheap now.
- Reserve schema slots for push constants and specialization constants now, even without the extract
  machinery. Then later it is just a Slang-side "what does this shader use?" extract change. Phase F E6.
- The index hands out a profile-scoped builder (QueryFor(profile)) for the runtime path. Scope the
  profile on the builder, not the whole index, so one index serves tooling, precache-all, and a renderer
  that mixes access models across passes.
- Add client introspection accessors: AxisNames() and AxisValueNames(axis) on the index. Building a
  query UI or a preset needs the names, and today that means reaching into the view by hand.
- The tiering touches validators. VerifyManifestRoundTrip, CheckManifestLayout, and DedupeInfluenceTest
  all assume the current table shape. Re-prove each per tier, and the cross-tier references too. Round
  trips are never optional.
- The cook is the distribution bundle, not the module. Strings and source live in the whole-cook scope,
  so a module is no longer self-contained. The module is a logical view inside the bundle.
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
- Resolve a synthetic module's required import statements for type and enum axes as the declaring
  module for the type being used. Currently `RawAxisDeclaration::RootModule` holds that, but it's
  just captured as where the variable was found during the bootstrap (not where type is defined).
    - We can find this by reflecting the type as a DeclReflection, then walking that chain up
      until we hit the declaring module.
    - This should be what both enum and interface axes use for finding the definitions.
- Clean up cookerdriver. This is getting a bit ridiculous: it's a hugely complex.cpp, all the anonymous
  namespace functions are declared and defined together, it could all be condensed considerably. Might be
  worth using the FSM approach we tried in VeloxRhi, where we use a variant of discrete states and step
  through them. Then each state and it's functionality could go in a file, and it would help make control
  flow more clear.
- Add a guard that fails loudly if a cook emits no manifest, so this validator cannot be orphaned again
  without a test going red.
- Verify that the cook actually wrote the files it claims. Part of a bug hid behind stale artifacts
  on disk, not missing ones: a reader saw an old file and thought the cook succeeded. After a cook,
  check with `std::filesystem::last_write_time` that each expected artifact is newer than the cook's
  start time, and that every expected artifact exists. `check-known-good.py` should also refuse to
  compare an artifact whose timestamp predates the cook it just ran.
- Make `RunCookOnce` and the determinism path emit through one code path. The determinism branch
  replays memory artifacts to the real sink with its own `sink.Write`/`WriteArtifact` loop instead of
  the shared emit, so the two paths can diverge (they did). Route both through the same emit.
# Error reporting
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
# Found in review
- SlangCompiler.cpp, Line 1234: We extract the raw global bindings not once, but individually for each variant. We should be able to do this at a higher level, even if this specific variant doesn't actually use all of the entrypoints. We will need to identify further axes for data reuse like this to scale to much higher variant counts without terrible performance.
# Testing
- Build a purpose-made "kitchen-sink" test shader that exercises the whole capability space in one
  module, and retire the borrowed content shaders (`OceanFft`, `Vtf`) from the test tree. Those are
  real content meant for other projects, and each capability now needs its own asset (a separate shader
  just for enum axes, another for interface axes). One synthetic module should cross an interface axis,
  an enum axis, a boolean axis, and a tuning axis, with an `ActiveWhen` gate and a `Require` constraint,
  so the cook tests exercise the axis interactions the single-purpose assets cannot. It should carry a
  size expression that names an axis, so the resolve path is covered too. Do this when the query and
  cook code is otherwise finished; it is a test-asset consolidation, not a blocker.
## Done 2026-09-22 (new binding schema tests)
- `WgslBindingScannerTest` retired. The text scanner is gone, so the test is now `WgslValidatorTest`
  (`tests/WgslValidatorTests.cpp`), which drives `WgslValidator` on Tint. It covers a match, a kind /
  shape / access / name mismatch, a depth texture and a comparison sampler, storage-buffer shape
  orthogonality, and a parse failure. The stale `WgslBindingScannerTest.exe` was deleted from the build
  tree.
- `ReflectionSchemaTest` added (`tests/ReflectionSchemaTests.cpp`), Slang-free: `GetBaseShape`, the
  flag predicates, the `ToString` tables, and `ReflectedUniformMember` equality over matrix layout and
  element stride.
- Four more tests were stale against the reshaped schema and are fixed: `StageDumpTest`,
  `DedupeInfluenceTest`, `ManifestIndexTest`, `ShaderManifestRejectTest`. Each used the removed
  `ResourceShape::Buffer` (now `StructuredBuffer`); `StageDumpTest` also used the removed
  `RawBinding::SamplerType` / `SamplerBindingType`, which is dropped (a plain sampler is the
  `IsComparisonSampler = false` default).
## Testing findings to resolve
- `run-tests.bat` line 43 runs `CookTest.exe`, but no `CookTest` target exists in `tests/CMakeLists.txt`
  (only the five named cook variants build from `CookTest.cpp`). So `[FAIL] CookTest` is stale
  scaffolding, not a real failure: the base OceanFft `--verify-deterministic` cook is unbuilt. Decide
  whether to restore the target or retire the script line. This is the end-to-end coverage the manifest
  work will lean on.
- `HashReflectedBinding` (`src/model/ShaderDataSchema.cpp`) hashes each member's `Offset`, `Size`, and
  `ArrayCount`, but not `ElementStride` or `MatrixLayout`. Dedup stays correct, because
  `ReflectedUniformMember::operator==` includes both and the interner decides equality by byte
  comparison, not the hash. The cost is extra bucket collisions for structured buffers that differ only
  in layout, plus a landmine if the hash is ever treated as equality. Close it or leave it, but know it.
# Phase F: portable geometry (vertex/index pulling)
Design explored 2026-09-22. This is Phase F lowering work, not scheduled yet. The goal is a shader
that reads vertices and indices the same way whether the target binds a vertex buffer through the
input assembler or pulls from a storage buffer, so one shader is truly portable across the fixed
pipeline and a GPU-driven one.
- Add `IVertexSource` and `IIndexSource` as library builtins, in a new `.slang` file beside
  `LodestoneAttributes.slang`. The reflector reads them by name, the same way it reads the attributes.
- Keep the interface stage-agnostic: `Vertex load(uint index)` and `uint load(uint i)`. The caller
  supplies the index (the input assembler / `SV_VertexID` in a vertex shader, a thread-computed index
  in compute). Do NOT bake `SV_VertexID` into the interface, or it cannot run in compute. This is the
  factoring that lets the same mesh-decode code run in a vertex shader and a compute geometry pass, so
  it is the enabler for a Nanite-like software-raster / meshlet system (the access floor, not the
  system).
- Realize the choice by selection, not transformation. Vertex sourcing is a technique axis backed by
  link-time specialization (interface conformances), which Slang already composes for us
  (`IComponentType::specialize` / `link`, `ITypeConformance`). Slang exposes composition, not AST
  mutation: there is no public AST-rewrite API, so text substitution and AST surgery are both off the
  table. This is the same mechanism the interface axes already use.
- Add an attribute (e.g. `ls_vertex_attribute`) that marks the pullable fields. One declaration, two
  consumers: the bound arm emits the input-assembler layout, the pulled arm synthesizes the buffer's
  element layout. Both reflection substrates already exist (`ReflectedVertexInput`, and the
  structured-buffer member walk with stride and matrix layout).
- Derisk first: the bound (IA) arm needs the varying inputs on the entry point, because Slang ties
  varyings to entry-point parameters, not to an interface implementation. So the bound arm needs either
  a generated thin wrapper entry point (built from the marked struct) or a generic entry-point template
  the body plugs into. Prototype this seam on a trivial mesh, both arms, before baking
  `ls_vertex_attribute` into the manifest schema. Everything else here is cheap or proven; this is the
  one unknown.
- Do not require the interface globally. Make it an opt-in portability capability/tier. Inside the tier
  the interface is mandatory and raw vertex access cannot be written down; outside it, raw varyings stay
  legal. This keeps the escape hatch and does not tax simple raster shaders.
- The enforcement is an ingestion-surface contract check ("does this pull-capable shader obey the
  contract?"), not one of the four cross-check validators. It is checkable on the reflected entry-point
  inputs: a pull-capable entry point must not declare a raw vertex-semantic varying that did not come
  from the builtin. Slang gives the interface and the conformance; it does not enforce "no raw
  varyings" — that policy is ours.
- The bound source is vertex-stage-only (there is no input assembler in compute), so it is a natural
  `ActiveWhen` gate (stage == vertex) that drops into the constraint engine. Compute geometry is always
  pulled.
- Index / EBO pulling is the same pattern one level up (`IIndexSource`). It interacts with indirect draw
  arguments and with base-vertex / base-instance. Note Slang rebases `SV_VertexID` / `SV_InstanceID`
  (`gl_VertexIndex - gl_BaseVertex`, needs the `DrawParameters` capability on SPIR-V), so the pulled
  index keeps HLSL zero-based semantics across targets, but the base offset and that capability are a
  per-profile concern.
- This does not disturb the frozen client contract: the pulled arm is an ordinary structured-buffer
  binding, the bound arm is vertex inputs, and both are already modeled.
- The pulled buffer's layout (interleaved vs planar, AoS vs SoA) is device-sensitive, so by the phase F
  rule it is a Lodestone tuning knob. Drive it from policy later, once the selection machinery exists.