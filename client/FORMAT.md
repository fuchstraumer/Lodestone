# Manifest format history

This file describes what each format version added to the binary manifest, and how the client reads it. CLAUDE.md holds the layout rules that every version follows.

## Version 7

Each profile records its `ShaderCodeFormat` (since format version 7). A SPIR-V source is read as words, so
`Open` rejects a SPIR-V source that is not on a 4-byte boundary or not whole words. The sources are packed
with no padding: every SPIR-V module is whole words, so each offset stays aligned. Codegen fails a SPIR-V
payload that is not whole words or has no SPIR-V magic number.

## Version 8

Each module axis records the default that the shader declares, as `ModuleAxis::DefaultDigit`, a digit of the
module's own key (format version 8). It is per module and not on the root axis, because two modules can
share a root axis and declare different defaults. The cook reads the default from the initializer text
(the symbol table), and compares it with Slang reflection where reflection can read it: reflection returns
literal initializers only, and never the `= Impl` of an `extern struct`. An axis with no readable default
fails the cook. The declared default is not the canonical value: canonicalization still fills an unused
axis with the first value (`PermutationAxis::GetCanonicalValue`), so the default does not change any key.

## Version 9

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

## Version 10

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

## Version 11

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

## Version 12

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

## Version 13

Each binding record has a footprint axis mask (format version 13, task L16): `EnvironmentHeader::FootprintAxisTableOffset`,
one mask for each binding record in the word layout of the axis-active mask. Bit k is set when axis k changes the
resource's element count or extent. `model/AxisInfluence` measures it by scope and name with the two passes of the
axis relevance, and a variant that does not declare the resource does not take part. Before the closing pass an
Undetermined axis counts as Inert: one cooked value changes nothing, and a sparse cook with no one-axis pair is then
closed by the second pass. The client reads it with `ResolvedResource::FootprintAxes` and `IsFootprintAxis`.
`DescribeCreation` gives a `CreationFields` value for a resource: for each creation field (size, usage, dimension,
format, extent, layers, mips, samples, sampler state, and the descriptor count of a binding array), whether the shader fixes it, fixes it for each variant, or
the renderer must supply it. A resource with no `Cpu` field is complete. `ShaderLibrary.resources.txt` prints it
for every resource that an entry point reads, and counts the complete buffers only: no shader states a texture's
mip count, so a texture is never complete. The report keeps its data (`ResourceReport`) apart from its text, so a
JSON form is a second writer over the same data.

## Version 14

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

## Version 15

Format version 15 adds `ProfileFlags::CodeFactsRead`: a clear bit means no reader ran for the profile, which is a
different fact from a source that needs nothing (`EnvironmentView::HasCodeFacts`). `manifest_dump` writes the facts
only when the bit is set.

## Version 16

Format version 16 (phase F step F4.A) gives `Binding::ArrayCount` a value of 0: an unbounded binding array
(`Texture2D views[]`), whose descriptor count the renderer chooses. Reflection returns `SLANG_UNBOUNDED_SIZE`, and stage 3
stores 0 (before, a `--no-validate` cook wrote 4294967295). The SPIR-V reader reads an `OpTypeRuntimeArray` as a count of 0,
so the cross-check compares it like any other length. The client reads it with `ResolvedResource::IsUnbounded`, and
`DescribeCreation` gives `CreationFields::BindingCount` as `Cpu` (as `Shader` for a fixed array of more than one), so an
unbounded buffer array is never complete. Slang states `RuntimeDescriptorArray`, and the non-uniform capabilities for
`NonUniformResourceIndex`; the reader adds `...ArrayDynamicIndexing` for a dynamic index as for a fixed array. Slang places
an unbounded array at an ordinary binding of its set, beside the other bindings, and two arrays share a set. A renderer
that wants `VARIABLE_DESCRIPTOR_COUNT` needs the array at the highest binding of its set, which Slang does not promise, so
it can declare a fixed upper bound with `PARTIALLY_BOUND` instead. WGSL cannot cook an unbounded array.

## Version 17

Format version 17 (phase F steps F4.2 and F4.3) adds stored values: a binding whose `PlacementKind` is `Handle` or `Pointer`
stores its value in a container. `Binding::ValueContainer` is a `StoredValueContainer` (a uniform buffer, or push constants),
`PlacementPayload` holds the container's group and binding (0 and 0 for push constants), and `Binding::ValueOffset` the byte
offset. The container is named by placement and not by record index, so a handle record stays one value across variants
whose containers differ. `Open` checks the fields of each record (`InvalidStoredValue`) and, for each entry point instance,
that the container is in the same visibility list or that the instance has push constants (`StoredValueContainerMissing`).
A visibility list is ordered by `PlacementOrderKey`: bound placements by group and binding, then stored values by container,
group, binding, and offset. The merged layout merges by the same key, so a value never collides with its container.
`Profile::Forms` (a `PlacementForms`) replaces the profile's one access model. The client reads a stored value with
`ResolvedResource::Placement`, `PlacementValue`, `ValueContainer`, and `ValueOffset`.
