# Phase F preparation/edge case fixes
- Allow declaration of presets in the policy TOML and cook them into the manifest. Data is easier to iterate on than compiled code. Currently we build these as objects/value types at the client level, inside compiled client code.
- Decide the persisted form of a variant. `DecodedVariant` holds string views into one manifest and
  no axis names, so it cannot outlive that manifest. A persisted form needs (axis name, value) pairs
  with owned strings, and a load that re-keys them through a query.
# Resource Layer
- Reflection joins a scope with `_` (`JoinScopeName` in `SlangReflector.cpp`), which is the WGSL
  spelling. SPIR-V uses `.`. Store `.`, and let each validator make its own form. `SpvValidator` compares
  the last name segment only until then.
- `SpvValidator` reads more than it compares. Pass the `CompiledEntryPoint` to the validator, then compare
  the LocalSize, the member layout (Offset, ArrayStride, MatrixStride, and the inverted matrix major), the
  vertex inputs, the color targets, and `WritesFragDepth`. Add a per-variant hook for the vertex-to-fragment
  link check.
- Record the capabilities that each entry point needs. Today no cook records them, because Slang warning
  41012 is off (phase F plan, O7). Read them from the target output (SPIR-V `OpCapability`). Collate them
  for each variant. `src/target/impl/SpvModuleReader` already parses each module and lists its
  `OpCapability` names (`ParseState::Capabilities`), so this can read them without the comparison.
- A query takes the device capability set and removes each variant that needs more. This filter is the base preset. Every other query builds on it.
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler, or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Resource Metadata
- In most shader markup languages, it's possible to specify default values, mins, maxes, etc for each uniform parameter expected to be used as a content-facing editable parameter. 
- Add this capability to our resource system, using the existing annotation system but expanded with the aforementioned features
- Add an additionally "clamp mode" parameter: artists often want to push values beyond the min max we specify, so we should use this loose clamping as default. Sometimes, going outside of the range could cause UB or break a shader though - so we want to have a `HardClamp` sort of attribute
- Lodestone doesn't take action on these, it should just pack them into the manifest. The most elegant solution may be a typeless attribute system of some kind, just a bundle of tagged bytes.
# Permutation system
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- Let a module policy section inherit another target section, for example `inherit = "wgsl"`.
# Cook driver and manifest
- Tune the worker thread count against memory, and measure it. `ThreadPool::Initialize` uses `hardware_concurrency` and ignores `SlangCompilerCreateInfo::ExpectedBatchSize`. Nothing measures the count against anything.
    - Each worker has its own global session and its own copy of the core module (the `k_UseSlangWorkaround` path). Measured on 2026-10-12, KitchenSink, RelWithDebInfo, 16 workers: a one-target cook peaks at 1.62 GB of commit `--single-threaded` peaks at 0.89 GB. That is about 45 MB for each worker, before any variant work.
    - Slang does not check `malloc` (`source/core/slang-allocator.h`). Out of commit, a cook does not fail with an error. It crashes with 0xC0000005 (a null write in `List::reserve`), 0xC0000409 (`bad_alloc` to `std::terminate`), or 0xC00000FD (a stack page that cannot commit). On 2026-10-12 a PC with no page file and about 8 GB of free commit gave all three, and three parallel cooks crashed in most runs.
    - Threading across environments (several targets or modules at once) multiplies the sessions. Decide the count from a memory budget and the variant count of the batch, not from the core count alone. Measure peak commit and wall time against the count first: 1, 2, 4, 8, 16.
    - Consider one global session and core module shared by the workers, if Slang allows it. Measure the global-session convoy again first (handoff fault 9).
- Make `RunCookOnce` and the determinism path emit through one code path. The determinism branch replays memory artifacts to the real sink with its own `sink.Write`/`WriteArtifact` loop instead of the shared emit, so the two paths can diverge (they did). Route both through the same emit.
# Error reporting
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting
- Better error reporting out of the attribute expression evaluation system
# Phase F: portable geometry (vertex/index pulling)
- Planned and mapped out, worked into roadmap for near future
# Use SPIRV-Reflect
- Our SpvValidator originally had a lightweight SPIR-V parser (and was written for fun, which it did deliver) to handle some basic binding validation. The scope has now grown to parsing most of the SPIR-V, and keeps growing as this library does
- Replace it with SPIRV-Reflect, which is effectively 3 files: one C source file, one C header, one C++ `.hpp` wrapper. It builds cook and covers all of our use cases, and would let us offload some maintenance burden.
- It also may allow us to move descriptors, which isn't something we use yet - but which I could see eventually being beneficial. or at least interesting.
# Profiling
- Currently, we can't reliably profile DiamondDogs or identify hotspots inefficiencies in our code (beyond basic complexity and O(n) analysis we do ourselves), because Slang is where nearly all of our wall-clock time goes (and then it absorbs all the samples in profilers). Create an artifically massive variant set, and bake it once upfront: dump the Slang output of this into a binary file, and then have a future run feed and load the rest of Lodestone from this file, so that only our code is running. 
- Use this to profile and clean up some of our code. I have a hunch that some of our containers could be replaced with things like `inplace_vector` or `flat_map` type structs. 
- Not a huge priority: Slang is where most of our time goes, but we should still strive to keep this code efficient.
# Testing
- Subdivide tests into areas of responsibility based on where in the pipeline they fall (basically partition like the source code folders they rely on)
- Partition the docs more as well
- Better integrate CTest: avoiding GTest is good, but I'd like to get the nice pass/fail visualizations gtest has back since it's a nice editor integration to have around