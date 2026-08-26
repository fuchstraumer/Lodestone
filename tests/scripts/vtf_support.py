"""Support code for the VolumeTiledForwardShading verification scripts.

The scripts drive the real compute shaders on a real device and compare each result against a numpy
reference. They exist because these shaders have no python reference of their own, unlike OceanFft.

Two things here are worth reading before you write a new test.

**Binding.** Each VTF entry point takes `uniform ParameterBlock<T> resources`, which is an entry point
parameter and not a module-scope global. `ComputeKernel.dispatch(vars=...)` cannot reach an entry point
parameter. It raises nothing, the dispatch runs, and every resource reads as zero. Use `Dispatch`
below, which goes through `ShaderCursor.find_entry_point`, and which raises on a name that does not
exist.

**Permutation.** `LinkedKernel` builds the same synthetic module that the cooker builds: a one-line
Slang module holding `export static const <type> <NAME> = <value>;`, linked next to the real module.
The Slang linker matches it to the `extern static const` declaration by name. Passing `constants=` to
`LinkedKernel` therefore cooks one variant, and a test can compare two variants against each other.
"""

import pathlib

import numpy as np
import slangpy as spy

ASSET_ROOT = pathlib.Path(__file__).resolve().parents[1] / "assets"
VTF_DIR = ASSET_ROOT / "compute" / "VolumeTiledForwardShading"
VTF_MODULE = VTF_DIR / "VolumeTiledForwardShading.slang"

INCLUDE_PATHS = [
    ASSET_ROOT,
    ASSET_ROOT / "common",
    ASSET_ROOT / "materials",
    ASSET_ROOT / "render",
    VTF_DIR,
]

# One color format serves every raster test. It is float and not 8-bit unorm, so a test can compare
# the target against its source value for value. An 8-bit target would quantize, and the test would
# then have to hold a tolerance that hides a small error.
COLOR_FORMAT = spy.Format.rgba32_float

# Every buffer in these tests is read, written, and copied back, so one usage mask serves all of them.
BUFFER_USAGE = (
    spy.BufferUsage.shader_resource
    | spy.BufferUsage.unordered_access
    | spy.BufferUsage.copy_source
    | spy.BufferUsage.copy_destination
)

# Mirrors ddCommonTypes.slang. float3 Min, float Pad0, float3 Max, float Pad1.
AABB_DTYPE = np.dtype([("Min", np.float32, 3), ("Pad0", np.float32),
                       ("Max", np.float32, 3), ("Pad1", np.float32)])

# Mirrors VtfTypes.slang. Enabled is a uint and not a bool, because WGSL forbids bool in a storage
# buffer.
POINT_LIGHT_DTYPE = np.dtype([
    ("Position", np.float32, 4),
    ("PositionViewSpace", np.float32, 4),
    ("Color", np.float32, 3),
    ("Range", np.float32),
    ("Intensity", np.float32),
    ("Enabled", np.uint32),
    ("Padding", np.float32, 2),
])

SPOT_LIGHT_DTYPE = np.dtype([
    ("Position", np.float32, 4),
    ("PositionViewSpace", np.float32, 4),
    ("Direction", np.float32, 4),
    ("DirectionViewSpace", np.float32, 4),
    ("Color", np.float32, 3),
    ("SpotLightAngle", np.float32),
    ("Range", np.float32),
    ("Intensity", np.float32),
    ("Enabled", np.uint32),
    ("Padding", np.float32),
])

assert POINT_LIGHT_DTYPE.itemsize == 64, POINT_LIGHT_DTYPE.itemsize
assert SPOT_LIGHT_DTYPE.itemsize == 96, SPOT_LIGHT_DTYPE.itemsize
assert AABB_DTYPE.itemsize == 32, AABB_DTYPE.itemsize


class Results:
    """Counts the checks. Each test script builds one and returns its failure count as the exit code."""

    def __init__(self) -> None:
        self.passed = 0
        self.failed = 0

    def check(self, condition: bool, description: str, detail: str = "") -> bool:
        if condition:
            self.passed += 1
            print(f"  PASS  {description}")
        else:
            self.failed += 1
            print(f"  FAIL  {description}")
            if detail:
                print(f"        {detail}")
        return condition


class VtfDevice:
    """Holds the device and the loaded VTF module."""

    def __init__(self) -> None:
        self.device = spy.Device(
            compiler_options={"include_paths": [str(p) for p in INCLUDE_PATHS]}
        )
        self.module = self.device.load_module(str(VTF_MODULE))
        self.entry_points = {ep.name: ep for ep in self.module.entry_points}

    def buffer(self, data: np.ndarray) -> spy.Buffer:
        """Creates a buffer that holds `data`. The element stride comes from the array dtype.

        create_buffer takes a plain array and not a structured one, so a structured array travels as
        its own bytes. The element count and the stride still come from the dtype, so the buffer keeps
        the layout that the shader struct expects.
        """
        contiguous = np.ascontiguousarray(data)
        element_count = contiguous.shape[0]
        struct_size = contiguous.dtype.itemsize
        payload = contiguous.view(np.uint8).reshape(-1)

        return self.device.create_buffer(
            element_count=element_count,
            struct_size=struct_size,
            usage=BUFFER_USAGE,
            data=payload,
        )

    def zeros(self, count: int, dtype=np.uint32) -> spy.Buffer:
        return self.buffer(np.zeros(count, dtype=dtype))

    def upload(self, buffer: spy.Buffer, data: np.ndarray) -> None:
        """Writes `data` into a buffer that already exists.

        A structured array travels as its own bytes, the same way `buffer` sends it. A benchmark
        needs this, because allocating a buffer for each arm measures the allocator too.
        """
        contiguous = np.ascontiguousarray(data)
        buffer.copy_from_numpy(contiguous.view(np.uint8).reshape(-1))

    def read(self, buffer: spy.Buffer, dtype=np.uint32) -> np.ndarray:
        return buffer.to_numpy().view(dtype)

    def constant_module(self, name: str, value) -> spy.SlangModule:
        """Builds the one-line module that drives one permutation constant.

        This is the same text that `MakeExportedConstantSource` in src/PermutationSpace.cpp builds, so
        a test here exercises the mechanism the cooker uses.
        """
        if isinstance(value, bool):
            type_name, literal = "bool", "true" if value else "false"
        elif isinstance(value, int):
            type_name, literal = "uint", f"{value}"
        else:
            raise TypeError(f"axis {name} takes a bool or an int, not {type(value)}")

        source = f"export static const {type_name} {name} = {literal};\n"
        return self.device.load_module_from_source(f"{name}_{literal}", source)

    def kernel(self, entry_point: str, constants: dict | None = None) -> "LinkedKernel":
        return LinkedKernel(self, entry_point, constants or {})

    # -- timing ---------------------------------------------------------------------------------

    def time_dispatch(self, kernel: "LinkedKernel", thread_count, resources: dict,
                      repeat: int = 64, warmup: int = 8, reset: list | None = None) -> dict:
        """Times one dispatch on the device clock, and returns the milliseconds it takes.

        The timestamps sit on the GPU timeline directly around the dispatch, so the result holds no
        submit cost and no driver cost. Every run goes into one command buffer, and a global barrier
        separates them, because two dispatches with no barrier between them can overlap and the
        second one would then appear to take no time.

        The first `warmup` runs are recorded and thrown away. They hold the cost of the first touch
        of each resource, and the clock of a device that has not yet reached its working frequency.

        Read the **median**. A single run can meet another process on the device, and the mean then
        follows that and not the shader.
        """
        total = warmup + repeat
        pool = self.device.create_query_pool(spy.QueryType.timestamp, 2 * total)

        encoder = self.device.create_command_encoder()
        for run in range(total):
            # `reset` holds the buffers a run writes and the next run must not inherit. An append
            # counter is the usual one. Without the clear it keeps rising across the runs, the output
            # list overflows, and every run after the first measures a shader that ran out of room.
            # The clear sits outside the timestamps, so it does not enter the result.
            for buffer in (reset or []):
                encoder.clear_buffer(buffer)
            if reset:
                encoder.global_barrier()

            encoder.write_timestamp(pool, 2 * run)
            with encoder.begin_compute_pass() as compute_pass:
                shader_object = compute_pass.bind_pipeline(kernel.pipeline)
                cursor = spy.ShaderCursor(shader_object).find_entry_point(0)["resources"]
                for field, value in resources.items():
                    cursor[field] = value
                compute_pass.dispatch(thread_count)
            encoder.write_timestamp(pool, 2 * run + 1)
            # Without this the next dispatch can start before this one finishes.
            encoder.global_barrier()

        self.device.submit_command_buffer(encoder.finish())
        self.device.wait()

        stamps = np.asarray(pool.get_timestamp_results(0, 2 * total), dtype=np.float64)
        milliseconds = (stamps[1::2] - stamps[0::2])[warmup:] * 1000.0

        return {
            "median": float(np.median(milliseconds)),
            "min": float(milliseconds.min()),
            "mean": float(milliseconds.mean()),
            "spread": float(np.percentile(milliseconds, 90) - np.percentile(milliseconds, 10)),
            "runs": int(len(milliseconds)),
        }

    # -- raster ---------------------------------------------------------------------------------

    def texture(self, data: np.ndarray) -> spy.Texture:
        """Creates a sampled texture that holds `data`, which is height by width by 4 float32."""
        contiguous = np.ascontiguousarray(data, dtype=np.float32)
        height, width, channels = contiguous.shape
        assert channels == 4, f"a float4 texture needs 4 channels, not {channels}"

        return self.device.create_texture(
            width=width,
            height=height,
            format=COLOR_FORMAT,
            usage=spy.TextureUsage.shader_resource | spy.TextureUsage.copy_destination,
            data=contiguous,
        )

    def render_target(self, width: int, height: int) -> spy.Texture:
        return self.device.create_texture(
            width=width,
            height=height,
            format=COLOR_FORMAT,
            usage=spy.TextureUsage.render_target | spy.TextureUsage.copy_source,
        )

    def point_sampler(self) -> spy.Sampler:
        """A sampler that takes one texel and does not mix it with its neighbours.

        A test that compares the target against the source texel by texel needs this. Linear
        filtering would mix each texel with the next one, and the comparison would then measure the
        filter and not the shader.
        """
        return self.device.create_sampler(
            min_filter=spy.TextureFilteringMode.point,
            mag_filter=spy.TextureFilteringMode.point,
            mip_filter=spy.TextureFilteringMode.point,
            address_u=spy.TextureAddressingMode.clamp_to_edge,
            address_v=spy.TextureAddressingMode.clamp_to_edge,
        )

    def raster(self, vertex_entry: str, fragment_entry: str,
               constants: dict | None = None) -> "LinkedRasterPipeline":
        return LinkedRasterPipeline(self, vertex_entry, fragment_entry, constants or {})


class LinkedKernel:
    """One entry point, linked against a chosen set of permutation constants."""

    def __init__(self, ctx: VtfDevice, entry_point: str, constants: dict) -> None:
        self.ctx = ctx
        self.name = entry_point
        self.constants = dict(constants)

        components = [ctx.module]
        for axis_name, value in self.constants.items():
            components.append(ctx.constant_module(axis_name, value))
        components.append(ctx.entry_points[entry_point])

        link_options = spy.SlangLinkOptions()
        self.program = ctx.device.link_program(components[:-1], [components[-1]], link_options)
        self.pipeline = ctx.device.create_compute_pipeline(program=self.program)

    def dispatch(self, thread_count, resources: dict) -> None:
        """Binds `resources` into the entry point ParameterBlock and runs one dispatch."""
        encoder = self.ctx.device.create_command_encoder()
        with encoder.begin_compute_pass() as compute_pass:
            shader_object = compute_pass.bind_pipeline(self.pipeline)
            cursor = spy.ShaderCursor(shader_object).find_entry_point(0)["resources"]
            for field, value in resources.items():
                # A wrong name raises here rather than reading as zero on the device.
                cursor[field] = value
            compute_pass.dispatch(thread_count)
        self.ctx.device.submit_command_buffer(encoder.finish())
        self.ctx.device.wait()


class LinkedRasterPipeline:
    """One vertex stage and one fragment stage, linked against a chosen set of permutation constants.

    This is the raster twin of LinkedKernel, and it links the same way. The one difference that
    matters is the resource cursor. A compute pipeline holds one entry point, so the ParameterBlock
    is always at entry point 0. A render pipeline holds two, and the VTF raster passes take their
    ParameterBlock on the fragment stage. So the cursor asks for entry point 1.
    """

    def __init__(self, ctx: VtfDevice, vertex_entry: str, fragment_entry: str,
                 constants: dict) -> None:
        self.ctx = ctx
        self.vertex_entry = vertex_entry
        self.fragment_entry = fragment_entry
        self.constants = dict(constants)

        modules = [ctx.module]
        for axis_name, value in self.constants.items():
            modules.append(ctx.constant_module(axis_name, value))

        entry_points = [ctx.entry_points[vertex_entry], ctx.entry_points[fragment_entry]]

        link_options = spy.SlangLinkOptions()
        self.program = ctx.device.link_program(modules, entry_points, link_options)
        self.pipeline = ctx.device.create_render_pipeline(
            program=self.program,
            input_layout=None,  # the vertex stage reads SV_VertexID and no vertex buffer
            # Each descriptor below takes a dict and not keyword arguments. This is how slangpy binds
            # its descriptor structs, and a keyword argument raises a TypeError.
            targets=[spy.ColorTargetDesc({"format": COLOR_FORMAT})],
        )

    def draw(self, target: spy.Texture, vertex_count: int, resources: dict) -> np.ndarray:
        """Draws `vertex_count` vertices into `target` and returns the target as a numpy array.

        The color attachment clears to zero first. A pass that writes nothing therefore gives a black
        image, which no correct pass gives, so the clear color is itself a control.
        """
        encoder = self.ctx.device.create_command_encoder()

        # The view must stay in a local. The attachment below does not keep it alive, so a view built
        # inline in the dict is destroyed before the pass runs, and the process stops with no message.
        view = target.create_view()

        color = spy.RenderPassColorAttachment({
            "view": view,
            "clear_value": spy.float4(0.0, 0.0, 0.0, 0.0),
            "load_op": spy.LoadOp.clear,
            "store_op": spy.StoreOp.store,
        })
        render_pass_desc = spy.RenderPassDesc({"color_attachments": [color]})

        # A render pass ends with an explicit call to end(). It is not a context manager, unlike the
        # compute pass that LinkedKernel uses. A `with` statement here does not raise. It stops the
        # process with no message and no python traceback.
        render_pass = encoder.begin_render_pass(render_pass_desc)

        shader_object = render_pass.bind_pipeline(self.pipeline)
        if resources:
            cursor = spy.ShaderCursor(shader_object).find_entry_point(1)["resources"]
            for field, value in resources.items():
                # A wrong name raises here rather than reading as zero on the device.
                cursor[field] = value

        state = spy.RenderState()
        state.viewports = [spy.Viewport.from_size(target.width, target.height)]
        state.scissor_rects = [spy.ScissorRect.from_size(target.width, target.height)]
        render_pass.set_render_state(state)

        render_pass.draw(spy.DrawArguments({"vertex_count": vertex_count}))
        render_pass.end()

        self.ctx.device.submit_command_buffer(encoder.finish())
        self.ctx.device.wait()
        return target.to_numpy()


# ---------------------------------------------------------------------------------------------
# numpy references. Each one mirrors one shader, and the two must agree.
# ---------------------------------------------------------------------------------------------


def reference_morton_cells(positions: np.ndarray, root_min: np.ndarray, root_max: np.ndarray,
                           bits: int = 10) -> np.ndarray:
    """Mirrors QuantizePosition in VtfComputeMortonCodes.

    Every step runs in float32, in the order the shader writes it: subtract, multiply by the
    reciprocal, saturate, scale, truncate. float64 would disagree with the shader for a light that
    lands on a cell boundary, and the disagreement would say nothing about the shader.
    """
    positions = positions.astype(np.float32)
    lower = root_min.astype(np.float32)
    upper = root_max.astype(np.float32)

    extent = np.maximum(upper - lower, np.float32(1.0e-6))
    inverse_range = (np.float32(1.0) / extent).astype(np.float32)
    normalized = np.clip((positions - lower) * inverse_range, np.float32(0.0), np.float32(1.0))
    scale = np.float32((1 << bits) - 1)
    return (normalized * scale).astype(np.uint32)


def morton_from_cells(cells: np.ndarray, bits: int = 10) -> np.ndarray:
    """Interleaves the bits of each axis. Bit i of x lands at 3i, y at 3i + 1, and z at 3i + 2."""
    codes = np.zeros(len(cells), dtype=np.uint32)
    for i in range(bits):
        mask = np.uint32(1 << i)
        shift = 2 * i
        codes |= (cells[:, 0] & mask) << shift
        codes |= (cells[:, 1] & mask) << (shift + 1)
        codes |= (cells[:, 2] & mask) << (shift + 2)
    return codes


def morton_bit_schedule(bits=(10, 10, 10)) -> list:
    """Gives the axis of each bit of an anisotropic Morton code, most significant first.

    An axis with more bits must get more splits, and those splits must spread through the whole code
    and not sit at the bottom of it. A bit at the bottom separates only two points that share one
    cell, and at these light counts no two points share a cell. So bits at the bottom do nothing.

    Bit k of an axis that holds b bits, counted from the most significant, stands at depth
    (k + 0.5) / b through that axis. Sorting every bit of every axis by that depth spreads each axis
    evenly. A tie takes the order z, y, x, which is the order the isotropic code already uses.
    """
    ordered = []
    for axis in (2, 1, 0):  # z is the highest axis of each group of three
        for k in range(bits[axis]):
            ordered.append(((k + 0.5) / bits[axis], axis))
    ordered.sort(key=lambda entry: entry[0])
    return [axis for _, axis in ordered]


def morton_from_cells_anisotropic(cells: np.ndarray, bits=(10, 10, 10)) -> np.ndarray:
    """Interleaves the axes with a bit budget that need not be equal.

    With bits at (10, 10, 10) this returns exactly what morton_from_cells returns, and
    test_vtf_curves.py checks that. So the general form cannot drift away from the shipped one.
    """
    schedule = morton_bit_schedule(bits)
    remaining = list(bits)
    codes = np.zeros(len(cells), dtype=np.uint32)

    for position, axis in enumerate(schedule):
        remaining[axis] -= 1
        source_bit = np.uint32(remaining[axis])       # the next bit of that axis, high to low
        target_bit = np.uint32(len(schedule) - 1 - position)
        codes |= ((cells[:, axis] >> source_bit) & np.uint32(1)) << target_bit

    return codes


def cells_from_morton(codes: np.ndarray, bits: int = 10) -> np.ndarray:
    """Takes the interleave apart again, so a mismatch can be read as a cell and not as a bit soup."""
    cells = np.zeros((len(codes), 3), dtype=np.uint32)
    for i in range(bits):
        shift = 3 * i
        for axis in range(3):
            cells[:, axis] |= ((codes >> np.uint32(shift + axis)) & np.uint32(1)) << np.uint32(i)
    return cells


def reference_morton_codes(positions: np.ndarray, root_min: np.ndarray, root_max: np.ndarray,
                           bits: int = 10) -> np.ndarray:
    return morton_from_cells(reference_morton_cells(positions, root_min, root_max, bits), bits)


# ---------------------------------------------------------------------------------------------
# The 3D Hilbert curve.
#
# The rules come from David Walker, "Algorithms for Encoding and Decoding 3D Hilbert Orderings",
# UTC Research Institute, August 2023. HILBERT_ENCODE_RULES is Table 3 of that paper and
# HILBERT_DECODE_RULES is Table 4.
#
# The paper indexes the 8 octants by the base ordering of its Figure 1, and not by the bits of the
# coordinate. The order is a Gray code, so each octant touches the one before it on a face. Section 3.1
# states which coordinate is large in which octant, and OCTANT_OF_BITS below holds that statement.
#
# Both functions take the depth as a parameter and always run that many steps. The paper gives an
# optimization that skips the leading zero digits, and this code does not use it. The number of steps
# it skips depends on the data, so on a GPU it would make the lanes of one wave diverge. A loop of a
# constant length is what the shader must run, so it is what the reference runs.
# ---------------------------------------------------------------------------------------------

# Index by (x_high | y_high << 1 | z_high << 2) and read the octant number of the base ordering.
# Section 3.1: x is large in octants 1, 2, 5, 6; y in 4, 5, 6, 7; z in 2, 3, 4, 5.
OCTANT_OF_BITS = np.array([0, 1, 7, 6, 3, 2, 4, 5], dtype=np.uint32)

# The reverse map. Row o holds the corner of octant o, and decoding starts from it.
BITS_OF_OCTANT = np.array([
    [0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1],
    [0, 1, 1], [1, 1, 1], [1, 1, 0], [0, 1, 0],
], dtype=np.int64)


def _hilbert_encode_step(x, y, z, octant, w):
    """Table 3. Returns the coordinate inside the octant, ready for the next digit."""
    rules = [
        lambda: (z,             x,             y),
        lambda: (y,             z,             x - w),
        lambda: (y,             z - w,         x - w),
        lambda: (w - x - 1,     y,             2 * w - z - 1),
        lambda: (w - x - 1,     y - w,         2 * w - z - 1),
        lambda: (2 * w - y - 1, 2 * w - z - 1, x - w),
        lambda: (2 * w - y - 1, w - z - 1,     x - w),
        lambda: (z,             w - x - 1,     2 * w - y - 1),
    ]
    new_x, new_y, new_z = np.zeros_like(x), np.zeros_like(y), np.zeros_like(z)
    for value in range(8):
        selected = octant == value
        if not selected.any():
            continue
        rule_x, rule_y, rule_z = rules[value]()
        new_x = np.where(selected, rule_x, new_x)
        new_y = np.where(selected, rule_y, new_y)
        new_z = np.where(selected, rule_z, new_z)
    return new_x, new_y, new_z


def _hilbert_decode_step(x, y, z, octant, w):
    """Table 4. Returns the coordinate in the box that is twice as wide."""
    rules = [
        lambda: (y,             z,                 x),
        lambda: (z + w,         x,                 y),
        lambda: (z + w,         x,                 y + w),
        lambda: (w - x - 1,     y,                 2 * w - z - 1),
        lambda: (w - x - 1,     y + w,             2 * w - z - 1),
        lambda: (z + w,         2 * w - x - 1,     2 * w - y - 1),
        lambda: (z + w,         2 * w - x - 1,     w - y - 1),
        lambda: (w - y - 1,     2 * w - z - 1,     x),
    ]
    new_x, new_y, new_z = np.zeros_like(x), np.zeros_like(y), np.zeros_like(z)
    for value in range(8):
        selected = octant == value
        if not selected.any():
            continue
        rule_x, rule_y, rule_z = rules[value]()
        new_x = np.where(selected, rule_x, new_x)
        new_y = np.where(selected, rule_y, new_y)
        new_z = np.where(selected, rule_z, new_z)
    return new_x, new_y, new_z


def hilbert_from_cells(cells: np.ndarray, bits: int = 10) -> np.ndarray:
    """Gives the Hilbert index of each cell. This is the twin of morton_from_cells.

    The index needs 3 bits for each level, so `bits` at 10 gives a 30-bit result. That is the same
    width the Morton code takes, and the two are therefore directly comparable as sort keys.
    """
    x = cells[:, 0].astype(np.int64)
    y = cells[:, 1].astype(np.int64)
    z = cells[:, 2].astype(np.int64)

    index = np.zeros(len(cells), dtype=np.int64)
    w = np.int64(1) << np.int64(bits - 1)

    for _ in range(bits):
        bit_pattern = ((x >= w).astype(np.int64)
                       | ((y >= w).astype(np.int64) << 1)
                       | ((z >= w).astype(np.int64) << 2))
        octant = OCTANT_OF_BITS[bit_pattern].astype(np.int64)
        index = index * 8 + octant
        x, y, z = _hilbert_encode_step(x, y, z, octant, w)
        w >>= np.int64(1)

    return index.astype(np.uint32)


def cells_from_hilbert(index: np.ndarray, bits: int = 10) -> np.ndarray:
    """Takes a Hilbert index apart again, so a mismatch reads as a cell and not as a bit soup."""
    index = index.astype(np.int64)

    corner = BITS_OF_OCTANT[index & np.int64(7)]
    x, y, z = corner[:, 0].copy(), corner[:, 1].copy(), corner[:, 2].copy()

    for level in range(1, bits):
        w = np.int64(1) << np.int64(level)
        octant = (index >> np.int64(3 * level)) & np.int64(7)
        x, y, z = _hilbert_decode_step(x, y, z, octant, w)

    return np.stack([x, y, z], axis=1).astype(np.uint32)


def reference_hilbert_codes(positions: np.ndarray, root_min: np.ndarray, root_max: np.ndarray,
                            bits: int = 10) -> np.ndarray:
    """The Hilbert twin of reference_morton_codes. It quantizes the same way, so only the curve differs."""
    return hilbert_from_cells(reference_morton_cells(positions, root_min, root_max, bits), bits)


def reference_light_aabb(positions: np.ndarray, ranges: np.ndarray):
    """Mirrors VtfReduceLightAABBs. The box that holds every light sphere."""
    lower = (positions - ranges[:, None]).min(axis=0)
    upper = (positions + ranges[:, None]).max(axis=0)
    return lower, upper


def reference_bvh_leaves(positions: np.ndarray, ranges: np.ndarray, order: np.ndarray,
                         branching: int = 32):
    """Mirrors the leaf level of VtfBuildBVH. Each node holds one run of `branching` lights."""
    sorted_pos = positions[order]
    sorted_range = ranges[order]
    node_count = (len(order) + branching - 1) // branching

    lower = np.full((node_count, 3), np.inf, dtype=np.float64)
    upper = np.full((node_count, 3), -np.inf, dtype=np.float64)
    for node in range(node_count):
        lo = node * branching
        hi = min(lo + branching, len(order))
        lower[node] = (sorted_pos[lo:hi] - sorted_range[lo:hi, None]).min(axis=0)
        upper[node] = (sorted_pos[lo:hi] + sorted_range[lo:hi, None]).max(axis=0)
    return lower, upper


def build_bvh_levels(positions: np.ndarray, ranges: np.ndarray, order: np.ndarray,
                     leaf_size: int = 32, branching: int = 32):
    """Builds every node level of the tree, from the level above the lights up to the root.

    The lights are not a level. A node of the lowest level holds `leaf_size` lights, and a node of
    each level above holds `branching` nodes of the level below it. The two counts are equal in the
    shipped tree, and this function keeps them apart so that a narrow bottom can be measured.

    Each level is a tuple of three arrays: the lower corners, the upper corners, and the count of
    children each node holds. The last run of a level can be short, so the child count is an array
    and not one number.

    The returned list runs from the bottom to the root.
    """
    lower, upper = reference_bvh_leaves(positions, ranges, order, leaf_size)

    light_count = len(order)
    node_count = len(lower)
    children = np.full(node_count, leaf_size, dtype=np.int64)
    children[-1] = light_count - (node_count - 1) * leaf_size

    levels = [(lower, upper, children)]

    while len(lower) > 1:
        child_count = len(lower)
        parent_count = (child_count + branching - 1) // branching

        parent_lower = np.empty((parent_count, 3), dtype=np.float64)
        parent_upper = np.empty((parent_count, 3), dtype=np.float64)
        for parent in range(parent_count):
            start = parent * branching
            stop = min(start + branching, child_count)
            parent_lower[parent] = lower[start:stop].min(axis=0)
            parent_upper[parent] = upper[start:stop].max(axis=0)

        parent_children = np.full(parent_count, branching, dtype=np.int64)
        parent_children[-1] = child_count - (parent_count - 1) * branching

        lower, upper = parent_lower, parent_upper
        levels.append((lower, upper, parent_children))

    return levels


def count_traversal_tests(levels, cluster_min: np.ndarray, cluster_max: np.ndarray) -> dict:
    """Counts every test the traversal performs, at every level of the tree.

    `VtfAssignLightsToClusters` walks the tree once for each cluster. It enters the root without a
    test. It then tests each child of every node it enters, and it enters a child whose box touches
    the cluster box. A child box is inside its parent box, so a node is entered exactly when its box
    touches the cluster. Therefore the count of tests on one level is the count of (cluster, node)
    pairs of the level above, multiplied by the children each of those nodes holds.

    A test against a node of the lowest level is a sphere test, because a child of that level is a
    light. Every other test is a box against a box.

    `count_cluster_overlaps` gives the count of clusters that touch each node, so the sum over a
    level is the count of (cluster, node) pairs of that level.

    Earlier work counted the lowest level alone. That undercounts a narrow bottom, because a narrow
    bottom needs more levels and each new level charges its own tests.
    """
    cluster_count = len(cluster_min)
    top_down = list(reversed(levels))

    per_level = []
    sphere_tests = 0
    box_tests = 0

    for depth, (lower, upper, children) in enumerate(top_down):
        if depth == 0:
            # Every cluster enters the root, and no test decides that.
            visits = np.full(len(lower), cluster_count, dtype=np.int64)
        else:
            visits = count_cluster_overlaps(lower, upper, cluster_min, cluster_max)

        tests = int((visits * children).sum())
        if depth == len(top_down) - 1:
            sphere_tests += tests
        else:
            box_tests += tests

        per_level.append({
            "nodes": len(lower),
            "visits": int(visits.sum()),
            "tests": tests,
        })

    return {
        "sphere_tests": sphere_tests,
        "box_tests": box_tests,
        "total_tests": sphere_tests + box_tests,
        "node_count": sum(len(lower) for lower, _, _ in levels),
        "level_count": len(levels),
        "per_level": per_level,
    }


class ClusterGrid:
    """One cluster grid, and the camera it belongs to.

    VtfComputeClusterAABBs needs both, and so does any measurement that compares a light against a
    cluster. They live together here because a grid and a projection that disagree give boxes that
    look correct and cover the wrong part of the world.

    The grid divides x and y uniformly across the screen, and z by a constant ratio between the near
    plane and the far plane. So a slice far from the eye is much deeper than a slice near to it.
    """

    def __init__(self, grid=(24, 16, 24), screen=(1920.0, 1080.0),
                 near: float = 0.1, far: float = 500.0, fov_y_degrees: float = 60.0) -> None:
        self.grid = grid
        self.screen = screen
        self.near = near
        self.far = far
        self.cluster_count = grid[0] * grid[1] * grid[2]

        # The depth ratio between one slice and the next, chosen to give grid[2] slices.
        self.near_k = (far / near) ** (1.0 / grid[2])
        self.cluster_pixels = (screen[0] / grid[0], screen[1] / grid[1])

        self.aspect = screen[0] / screen[1]
        self.fov_y = np.radians(fov_y_degrees)
        focal = 1.0 / np.tan(self.fov_y * 0.5)

        projection = np.zeros((4, 4), dtype=np.float32)
        projection[0, 0] = focal / self.aspect
        projection[1, 1] = focal
        projection[2, 2] = far / (near - far)
        projection[2, 3] = near * far / (near - far)
        projection[3, 2] = -1.0

        self.projection = projection
        self.inverse_projection = np.linalg.inv(projection.astype(np.float64)).astype(np.float32)
        self.identity = np.eye(4, dtype=np.float32)

    def cluster_params(self) -> dict:
        return {
            "GridDim": self.grid,
            "ViewNear": self.near,
            "ClusterSizeInPixels": (int(self.cluster_pixels[0]), int(self.cluster_pixels[1])),
            "NearK": self.near_k,
            "LogGridDimY": 1.0 / np.log(self.near_k),
        }

    def matrices(self) -> dict:
        """The view matrix is the identity, so view space and world space are the same here."""
        return {
            "viewMatrix": self.identity,
            "projectionMatrix": self.projection,
            "viewProjectionMatrix": self.projection,
            "inverseViewMatrix": self.identity,
            "inverseProjectionMatrix": self.inverse_projection,
            "inverseViewProjectionMatrix": self.inverse_projection,
        }

    def cluster_aabbs(self, ctx: "VtfDevice"):
        """Runs VtfComputeClusterAABBs and returns the lower and upper corner of each cluster."""
        out = ctx.buffer(np.zeros(self.cluster_count, dtype=AABB_DTYPE))
        ctx.kernel("VtfComputeClusterAABBs").dispatch([self.cluster_count, 1, 1], {
            "Clusters": self.cluster_params(),
            "Matrices": self.matrices(),
            "ClusterAABBs": out,
        })
        boxes = ctx.read(out, AABB_DTYPE)
        return boxes["Min"].astype(np.float64), boxes["Max"].astype(np.float64)


def make_lights_in_frustum(grid: ClusterGrid, count: int, seed: int = 11,
                           near: float = 2.0, far: float = 200.0,
                           radius_range=(1.0, 8.0)):
    """Random point lights that all fall inside the frustum of `grid`.

    make_lights puts the lights in a box, and a box does not agree with a frustum. A light outside
    the frustum overlaps no cluster, so it adds nothing to the measurement and makes each average
    smaller than it should be. Every light here is inside the frustum, and each one can be reached.

    Depth follows a constant ratio and not a uniform spread, because the cluster grid divides depth
    the same way. A uniform spread would put almost every light in the far slices, where the clusters
    are largest, and the measurement would then describe the far field only.
    """
    rng = np.random.default_rng(seed)

    # Depth first. A constant ratio in depth gives the same count of lights in each slice of the grid.
    depth = near * (far / near) ** rng.uniform(0.0, 1.0, size=count)
    half_height = depth * np.tan(grid.fov_y * 0.5)
    half_width = half_height * grid.aspect

    positions = np.zeros((count, 3), dtype=np.float32)
    positions[:, 0] = (rng.uniform(-1.0, 1.0, size=count) * half_width).astype(np.float32)
    positions[:, 1] = (rng.uniform(-1.0, 1.0, size=count) * half_height).astype(np.float32)
    # View space looks down -z.
    positions[:, 2] = (-depth).astype(np.float32)

    ranges = rng.uniform(radius_range[0], radius_range[1], size=count).astype(np.float32)

    lights = np.zeros(count, dtype=POINT_LIGHT_DTYPE)
    lights["Position"][:, :3] = positions
    lights["Position"][:, 3] = 1.0
    lights["PositionViewSpace"][:, :3] = positions
    lights["PositionViewSpace"][:, 3] = 1.0
    lights["Color"] = rng.uniform(0.2, 1.0, size=(count, 3)).astype(np.float32)
    lights["Range"] = ranges
    lights["Intensity"] = 1.0
    lights["Enabled"] = 1
    return lights, positions, ranges


def make_lights_realistic(grid: ClusterGrid, count: int, seed: int = 11,
                          near: float = 1.0, far: float = 80.0,
                          local_fraction: float = 0.10, local_far: float = 10.0,
                          radius_median: float = 4.0, radius_sigma: float = 0.6):
    """Random point lights placed the way a level holds them, not the way a random number gives them.

    make_lights_in_frustum draws depth on a constant ratio and radius on a flat range, and the two do
    not know about each other. So it puts large lights directly in front of the eye, and about 16 in
    each 100 lights hold the camera inside them. A level does not do that.

    Three changes make it behave:

    * **Density is uniform in world volume.** The count of lights at depth z therefore grows with the
      square of z, because the frustum is that much wider there. A constant ratio in depth instead
      puts as many lights between 2 and 4 as between 100 and 200, and the near ones are what cost.
    * **Radius follows a log normal spread.** Most lights are small and a few are large, which is what
      a level holds. A flat range gives too many large ones.
    * **A part of the lights are local.** Uniform density alone gives almost no light that holds the
      camera, because the volume near the eye is very small. A camera in a level is not at a random
      point of a large volume. It stands in a room, and that room has its own lights close by.
      `local_fraction` of the lights go in that room, which reaches to `local_far`.

    Together these give a low count of lights that hold the camera, and the count is not zero. That is
    what a camera moving through a level meets.
    """
    rng = np.random.default_rng(seed)

    local_count = int(count * local_fraction)
    distant_count = count - local_count

    def depth_from_uniform_density(sample_count, low, high):
        """Depth of `sample_count` points at uniform density between `low` and `high`.

        The volume out to z grows with z cubed, so the cube root of a flat random number between the
        two cubes gives the depth.
        """
        uniform = rng.uniform(0.0, 1.0, size=sample_count)
        return (low ** 3 + uniform * (high ** 3 - low ** 3)) ** (1.0 / 3.0)

    depth = np.concatenate([
        depth_from_uniform_density(local_count, near, local_far),
        depth_from_uniform_density(distant_count, near, far),
    ])
    rng.shuffle(depth)

    half_height = depth * np.tan(grid.fov_y * 0.5)
    half_width = half_height * grid.aspect

    positions = np.zeros((count, 3), dtype=np.float32)
    positions[:, 0] = (rng.uniform(-1.0, 1.0, size=count) * half_width).astype(np.float32)
    positions[:, 1] = (rng.uniform(-1.0, 1.0, size=count) * half_height).astype(np.float32)
    positions[:, 2] = (-depth).astype(np.float32)

    ranges = (radius_median * np.exp(rng.normal(0.0, radius_sigma, size=count))).astype(np.float32)

    lights = np.zeros(count, dtype=POINT_LIGHT_DTYPE)
    lights["Position"][:, :3] = positions
    lights["Position"][:, 3] = 1.0
    lights["PositionViewSpace"][:, :3] = positions
    lights["PositionViewSpace"][:, 3] = 1.0
    lights["Color"] = rng.uniform(0.2, 1.0, size=(count, 3)).astype(np.float32)
    lights["Range"] = ranges
    lights["Intensity"] = 1.0
    lights["Enabled"] = 1
    return lights, positions, ranges


def count_cluster_overlaps(leaf_min: np.ndarray, leaf_max: np.ndarray,
                           cluster_min: np.ndarray, cluster_max: np.ndarray) -> np.ndarray:
    """Gives the count of clusters that touch each node box.

    This mirrors AABBIntersectAABB in ddCommonFunctions.slang, which is the test the traversal applies
    to an internal node. The comparison uses `<=`, so two boxes that share a face count as touching,
    and the traversal counts them the same way.
    """
    counts = np.zeros(len(leaf_min), dtype=np.int64)
    for node in range(len(leaf_min)):
        overlaps = np.all(leaf_min[node] <= cluster_max, axis=1) & \
                   np.all(leaf_max[node] >= cluster_min, axis=1)
        counts[node] = int(overlaps.sum())
    return counts


def count_clusters_per_light(positions: np.ndarray, ranges: np.ndarray,
                             cluster_min: np.ndarray, cluster_max: np.ndarray,
                             chunk: int = 128) -> np.ndarray:
    """Gives the count of clusters each light really touches. This is the irreducible cost.

    No tree can make this number smaller. A light that covers 400 clusters must be written into 400
    cluster lists, whatever orders the lights and whatever holds them. So this is the floor, and the
    distance from it to the count of tests the traversal performs is the whole of the tree overhead.

    The test mirrors SphereInsideAABBFast in ddCommonFunctions.slang: take the squared distance from
    the sphere center to the box, and compare it against the squared radius. The traversal applies
    exactly this test at the leaf level, where one leaf holds one light.
    """
    counts = np.zeros(len(positions), dtype=np.int64)

    for start in range(0, len(positions), chunk):
        stop = min(start + chunk, len(positions))
        center = positions[start:stop].astype(np.float64)[:, None, :]
        radius = ranges[start:stop].astype(np.float64)[:, None]

        below = np.minimum(center - cluster_min[None, :, :], 0.0)
        above = np.maximum(center - cluster_max[None, :, :], 0.0)
        squared_distance = (below * below + above * above).sum(axis=2)

        counts[start:stop] = (squared_distance <= radius * radius).sum(axis=1)

    return counts


def make_lights(count: int, seed: int = 11, spread: float = 40.0):
    """Random point lights in view space, in front of the eye."""
    rng = np.random.default_rng(seed)
    lights = np.zeros(count, dtype=POINT_LIGHT_DTYPE)

    positions = rng.uniform(-spread, spread, size=(count, 3)).astype(np.float32)
    # View space looks down -z, so push every light in front of the eye.
    positions[:, 2] = -rng.uniform(2.0, spread, size=count).astype(np.float32)
    ranges = rng.uniform(1.0, 8.0, size=count).astype(np.float32)

    lights["Position"][:, :3] = positions
    lights["Position"][:, 3] = 1.0
    lights["PositionViewSpace"][:, :3] = positions
    lights["PositionViewSpace"][:, 3] = 1.0
    lights["Color"] = rng.uniform(0.2, 1.0, size=(count, 3)).astype(np.float32)
    lights["Range"] = ranges
    lights["Intensity"] = 1.0
    lights["Enabled"] = 1
    return lights, positions, ranges
