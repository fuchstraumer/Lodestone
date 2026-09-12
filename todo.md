# SlangCompiler
- Right now, we map to a super small subset of formats and features. We should support the full range, and extract them untouched
    - Then, during output format mapping we collapse to what that platform
    actually supports
    - Alternatively, use foresight about output target to fail builds if unsupported
# General Cleanup
- Break apart some of the really long functions in the codebase
- Subdivide functionality into some smaller objects, and reorg folder structure to be a little less flat
# Feature additions/removals:
- Remove the header/cpp generator. It's not necessary with manifest files, and is much less efficient. 
- Add the live process cooker. Not sure if persistent .exe or hotloaded DLL on windows with dllMain. Probably not too hard to support both.
# Resource Layer
- We should provide a way for clients to call something like `SetDeviceLimits` or `SetApiLimits` - we can use this to validate resource sizing expressions when being run as a live compiler,
  or we can use it against cooked content (in the device form) to make sure we don't try to create a shader a device can't support
# Permutation system
- Given the above, we'll also want a way to flag the "domain" of a parameter and it's optionality? Rootness? How important it is to the output, and if we expect it to depend on device properties.
  This could accelerate queries and allow for internal optimizations to help section data into platform/scalability presets perhaps.
- Policy file target keys are flat (`targets.wgsl`, later `targets.dxil`, `targets.spirv`). A platform split is coming: one target profile likely needs several device presets under it, e.g. `minspec`, `recommended`, `mobile`. A preset picks a different `MaxVariants`, `CookValues`, and binding-time lowering. Decide whether a preset nests under a target (`targets.wgsl.mobile`) or forms its own axis in the policy schema. Do this after E5 lands the flat form, so the schema change has a working baseline to move from.
- `ExpectedInfluence` is a kludgy and unintuitive way to declare "This axis should be inert for this entrypoint". Rename this to "InertAxesForEntryPoint" and allow it to be specified as a list of axes that shouldn't affect a named EP. This will save typing, and will also be more intuitive for users
- E6 loose end: `VerifyAxisNamesAreDeclared` is now dead. It is declared in `permute/PermutationSpace.hpp` with no caller and no definition. Delete the declaration.
- E6 loose end: fold the undriven-`extern static const` read into `SymbolTable`. `ExternConstantScanner` still does a line-by-line scan for `CollectUndrivenExternDefaults`, but the tokenizer already visits every source and already skips the `extern static const` lines. Capture the declared name and its default value there instead, then delete `ExternConstantScanner` and `ExternConstantScannerTest`. The consumers (`CollectUndrivenExternDefaults`, `ReportUndrivenExternConstants`) move to a query against the table, most naturally in `PrepareRawModule`. Do this after E7, as its own commit.
# Error reporting
- Route more errors through the diagnostic sink, especially all of the `std::println` statements. Those would just be plain better handled by diagostic sinks
- Use some kind of macro for the above case, which would wrap a local `std::source_location::current()` unpacking and shove it into the `Diagnostic` put into the sink. This way, all the user has to do is provide a sink pointer and the actual message + severity level
- `CookResult<T>`'s error type could be refactored to effectively be a `Diagnostic` entry, meaning that failures can be passed up the chain... but would also complicate having something like `MaxErrorCount`. Evaluate this space to see if it could potentially clean up the code, and help create more unified (but still expressive) error reporting 

# Found in review
- SlangCompiler.cpp, Line 1234: We extract the raw global bindings not once, but individually for each variant. We should be able to do this at a higher level, even if this specific variant doesn't actually use all of the entrypoints. We will need to identify further axes for data reuse like this to scale to much higher variant counts without terrible performance.