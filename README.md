# Lodestone: Because It's Time To Take Shaders Seriously

Stop me if you've experienced any of these before:
- Explosive piles of shader variants with edge cases that compromise workflows or content builds
- An inability to get precise metrics on the actual depth and breadth of your shader variants in a title
- Shader feature parameterization that doesn't play nice with editor workflows (`#ifdef` is knocking)
- Editors or engines that have a UI hang or freeze as a new variant or shader is built
- Finicky live-edit workflows that can be even more explosive than baked content builds

Lodestone aims to answer all of this. No matter how much we deny it, almost every engine and game project out there will need to deeply parameterize their shader code into sets of toggleable, varying featuresets. It usually starts with some includes, and then some automated resource binding management, and then the ability to have a client drive the shape of the compiled shader with various boolean feature flags or integral dimensionality settings (e.g, setting texture sizes for a lighting pass based on engine scalability/performance settings). Most of us are in an endless crunch for time, and end up with systems that work but make compromises on workflow or compromises on performance. This all comes down to one fact: *not viewing a shader system as a data transformation pipeline between two problem domains*

### A Short Example

Let's consider the Ocean-FFT shaders (available to view in `tests/assets/compute/ocean`). This (as of my last testing) exposes 3 entry points, and 3 permutation parameters:

| Parameter Name | Parameter Type | Parameter Usage | Valid Parameter Values |
| --- | --- | --- | --- |
| `IFFT_SIZE` | uint32 | The range of FFT grid dims | 128, 256, 512, 1024 |
| `IFFT_USE_WAVE_OPS` | bool | A toggle for subgroups based on hardware support | true, false |
| `IFFT_WAVE_SIZE` | uint32 | The range of wave sizes we built our algorithm around | 16, 32, 64 |

The potential range of variants, naively, would be the multiples of the lengths of each axis times the number of entrypoints that use them. In this case, that would be `4 * 2 * 3 * 3` - 72 potential variants.

We do better than that though: first, we are able to identify dependent values. If `IFFT_USE_WAVE_OPS` is false, we'll never need `IFFT_WAVE_SIZE`

(todo: update this!)

Example of variant and source collapse in final manifest:

sources: 105 -> 77 unique
layouts: 105 -> 21 unique

We forced all hashes to collide, as part of a test to fully verify contents as unique (showing that a hash is an accelerator, not the sole source of uniqueness)
hash collisions resolved by byte compare: 0
byte comparisons forced by a hash hit: 112

## The Problem

Shader authors (like tech artists) or content creators (like artists working with base materials) have their problem domain: they need intuitive and easy to edit shaders, a workflow that is reliable and stable but which still gives them room to experiment.

Engineers working on the runtime side - actually getting content into the engine - have another problem domain: performance, memory, and stability. We need to make sure shaders can be loaded fast when requested, we need to make sure we're using as little memory as possible to package all of our shaders, and we need to make sure that faults or failures (e.g, an artist trying to use an unsupported parameter set) don't bring the whole engine or editor crashing down around us. 

What we are describing is a compiler, though working with a DSL. Over another compiler. Which feels silly. But it's true! And we need to take this problem seriously: how many of us have been at studios or worked on titles where compiling shaders was a chore everyone dreaded? Where a bad shader variant, able to sneak through due to a hash collision or lying 9-10 static branches of `#ifdefs` deep breaks something? Where we've seen entire days of studio work lost to these issues? I certainly have. And I've always wondered at how I could engineer a way out of that problem.

Additionally, in many cases shader parameters or acceptable ranges of shader parameters are dependent on compiled code, or we need to make sure CPU-side code using and declaring resource sizes, ranges, and member offsets stays exactly in sync with shader code. There are many systems that do this, but in my opinion most of them still end up placing ungainly dependencies on compiled code that require binary builds to fix - there's a better way.

## The Solution

The solution is a set of changes to how we approach shader pipelines. 

First, recognizing them as a discretized set of data transformations that can be treated as separable and discretely verifiable steps. From the start, this gives us many more chances to validate our data and catch issues before they reach a `CreatePipelines` style of function call in native engine code.

Second, we need to create a data contract and common vocabulary between the shader cooking pipeline and the runtime that is going to use those results. It is both ends to shim and transform to/from that data: the shader cooker will package and extract the data the contract requests, matching that schema, and the runtime will then in turn use it's own transformations to unpack that data. For shipped content builds this can collapse down to a compressed memory read: or game engines can add CI steps that package shader cooker builds further into larger packages, creating filters that will unpack the data from propietary package formats. This is done in four key ways:

- The runtime links against a header, providing it a **concrete** layout and schema to implement against.
- A new shader therefore **does not** force the engine to rebuild
- The binary manifest files and a live cooker are, in turn, **just two implementations of this interface**. The engine doesn't care who satisfies the interface: just that it's satisfied.
- The interface **can be consumed as a C ABI**, as manifest files are just flat bytes with uint32 indices and no pointers. A live cooker can just serve data from memory.

Third, we need to make it as hard as we can for the CPU and GPU to disagree about the dimensionality and layout of a resource. So many bugs - be they really difficult to diagnose TDRs or bizarre UB (hi there, vertex soup!) - come from this category of problem. By adding a light annotation language to our shader code, we can do two things:

- Encourage shader authors to declare and parameterize the resources their shader will use expressively, instead of implicitly
- Allow the CPU to automatically create most of the resources a shader desires, and know that because it's reading data the pipeline output - which it verified as valid and functional - it will be able to safely write and set up the resource.

As an example, consider declaring an input to a compute shader - in this case one of my IFFT shaders - where we parametrize the size as a variant axis, along with a cascade count. Shader authors can write the following:

```slang
[ls_element_count("IFFT_SIZE * IFFT_SIZE * IFFT_NUM_WAVE_CASCADES")]
uniform RWStructuredBuffer<half4> IfftInput;
```

Fourth, because shaders are now declaratively paramaterized upfront, we can walk and validate this entire tree of variants for the whole shader library (or module, depending on how the boundaries fall). This is a feature model. It's what things like `kconfig` use in the Linux Kernel: Validity is a boolean satisfiability problem. This is NP-hard, but for us a depth-first walk with constraint propagation is enough. The gist is that we can linearize our indexing schema, we can look up variants in tables or manifests without needing to specify every variant parameter (just the ones we care about), and we can provide precise constraints on variant count. This library also makes it possible to specify a `VariantPolicy` - meaning you can constrain how much damage a single variant is able to do to your variant count.

[TODO: You gotta talk about the verification steps, how we validate dedupe works, how we roundtrip and A/B, etc etc. It provides determinism and validated performance of compression by deduplicating.]

### A Planned Fifth Solution

Lastly, we don't need to make an entire shader library compile fail on a bad variant or misconfigured shader: We can embed the error and failure into the content, and still write out the rest of the shader blob. This can even be checked into source control just fine: *the error state is now a property of the content*. Because the schema is what your runtime links and builds against, you can also validate it yourself using content tools - like those use for auditing content depots before ship. Your UI/UX systems can surface these errors to users as well, so that they know what they're seeing is invalid - and the runtime can avoid allocating or creating any graphics-API-specific resources for those broken shader variants. This means that bad and malformed content is not silently squirreled away, or hidden, but is explicitly handled without compromising the integrity of whole shader content builds.

Currently, we still fail on a bad variant - mostly because for me internally it provides useful debugging guarantees, and wasn't a feature I needed to get this code usable for one of my projects. This feature is one of my top 3 priorities, however.

## How It Works

This library performs a set of passes in a pipeline - much like a compiler - to transform your sets of slang modules into target source code that is portable across RHIs, binding models, and device featuresets.

I'm going to explain and diagram this dataflow now, omitting some simple steps - namely the common OS operations like initial loads from file, creating a cache directory, output, etc. The diagram below will serve as a high-level view of the whole process.

In it, square blocks are actual steps that generate an output or mutate the data as it flows through the pipeline: hexagons are *validators* that ensure the data moving between stages is sanitized and able to be processed in the next step.

### <p align="center">Compiler Flowchart</p>

<div align="center">

```mermaid
flowchart TD
    Entry["RunCook / RunCookOnce<br/>CookerDriver.cpp<br/>CookerOptions, OutputSink"]

    subgraph Preamble["Preamble - no shader content yet"]
        S1["STAGE 1 Declare<br/>PermutationSpace.cpp<br/>FindPermutationSpaceForModule<br/>PermutationAxis, PermutationSpace"]
        V1{{"VerifyAxisNamesAreDeclared<br/>ReportUndrivenExternConstants"}}
        S2["STAGE 2 Enumerate<br/>PermutationSpace.cpp<br/>EnumerateVariants<br/>VariantSet, VariantDescriptor"]
    end

    subgraph PerVariant["CompileModuleVariants - once for each VariantDescriptor"]
        S3["STAGE 3 Compile<br/>SlangCompiler.cpp<br/>CompileVariantRaw<br/>RawVariant, RawBinding, RawSizeAttribute<br/>ONLY file that talks to Slang"]
        S4["STAGE 4 Resolve<br/>ResolveStage.cpp<br/>MakeResolveContext, ResolveVariant<br/>CompiledVariant, DerivedSize<br/>names no Slang type"]
        V2{{"ValidateVariantReflection<br/>WgslBindingScanner.cpp"}}
        S5["STAGE 5 Normalize<br/>empty on purpose"]
        S6["STAGE 6 Intern<br/>CookedLibrary.cpp<br/>AppendVariantToModule<br/>ContentInterner, LibraryVariant"]
    end

    S7["STAGE 7 Key<br/>CookedLibrary.cpp<br/>FreezeModuleTables<br/>CookedModule"]
    V3{{"VerifyLibraryRoundTrip"}}
    V4{{"ComputeAxisInfluence + EnforceModulePolicy<br/>DedupeReport.cpp"}}
    S8["STAGE 8 Emit<br/>ShaderLibraryEmitter.cpp<br/>ShaderManifestEmitter.cpp<br/>DedupeReport.cpp"]
    V5{{"VerifyManifestRoundTrip"}}
    Out["Artifacts, through OutputSink"]

    Entry --> S1 --> V1 --> S2 --> S3 --> S4 --> V2 --> S5 --> S6
    S6 -.->|next descriptor| S3
    S6 --> S7 --> V3 --> V4 --> S8 --> V5 --> Out
```

</div>

#### Step 1. Create a Slang Compiler session

##### Actions
- First, we initialize the slang compiler backend: this creates our root global session that future compiles of variants will actually use. At this point, we know if the module path exists and that our input configuration for Slang (at the least) is valid
- Second, we will load the permutation space for the current module being built
##### Validations
- We will verify that the axis names stated in the permutation space can actually be found in the source code
- We will identify values declared as permutable, but not actually bound to any axis: we call these "undriven"
- *One validation action*: We will set default values for these undriven defaults using `compiler.ResolveExternConstantDefaults`

In the future, I plan to remove this default-resolve and make it a harder requirement to actually permute variables declared as intended permutation axes. 

#### Step 2. Enumerate and expand the active permutation space

##### Actions
- Call `EnumerateActiveCombinations` to perform a breadth-first traversal of the permutation space, doing classic backtrack-based constraint solving
    - This runs as a fold-left operation, for now. It will be improved in time
- After expansion completes and we've evaluated our space, we then perform canonicalization: we fill in the empty spaces in the evaluated concrete
  variants array to equalize (literally, canonicalize) the variant permutations for uniformity even with variants that have whole axes disabled