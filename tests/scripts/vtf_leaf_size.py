"""Measures what the count of lights in one leaf costs, over the whole tree.

Run it with:  py tests/scripts/vtf_leaf_size.py

This is a measurement and not a test. It prints numbers and it asserts nothing.

**Why this script exists.** `vtf_size_binning.py` measured the same question and counted the lowest
level alone. That model said a leaf of 4 lights costs 51 percent less than a leaf of 32. The model is
wrong in a way that always favours a narrow leaf: a narrow bottom needs more levels, and the model
charged nothing for them.

This script counts every test at every level. `count_traversal_tests` in `vtf_support.py` holds the
model, and the doc comment there states it.

**What a test is.** The traversal enters the root without a test. It then tests each child of every
node it enters. A test against a node of the lowest level is a sphere test, because a child of that
level is a light. Every other test is a box against a box. The two cost about the same, so the total
is the number to read.

**The floor.** `count_clusters_per_light` gives the (cluster, light) pairs that really touch. No tree
can make that number smaller, and the ratio of the tests to it is the tree efficiency.

**What this does not measure.** The build cost and the memory of the tree. A narrow bottom holds more
nodes, and `VtfBuildBVH` writes each one. The node count is in the table for that reason.
"""

import numpy as np

import vtf_support as vtf

LIGHT_COUNT = 4096
BRANCHING = 32
LEAF_SIZES = (2, 4, 8, 16, 32, 64)

# The traversal group holds this many threads today, and each thread visits one child.
GROUP_THREADS = 32

# The price of one test, in nanoseconds. vtf_timing.py measured it on 2026-08-29, on a desktop
# RTX 4080. The value is the cost of one sphere test alone. It does not include the appends, because
# every row of each table below performs the same count of appends.
#
# An earlier value of 0.053 came from a different machine, and it also charged the appends to the
# tests. Measure this again on each new machine. The direction of a row survives a change of machine.
# The magnitude does not.
TEST_PRICE = 0.0265

# The cost of one turn of the traversal loop is not measured. These are the prices the table tries.
TURN_PRICES = (0.0, 1.0, 5.0, 20.0)


def entered_of(row: dict) -> int:
    """Gives the count of nodes the traversal enters, over every cluster and every level.

    Each entered node is one turn of the traversal loop. A turn holds two group barriers and one
    pop that thread 0 performs alone. So this count is the cost that does not follow the test count,
    and a tree that lowers the tests can raise this.
    """
    return sum(level["visits"] for level in row["per_level"])


def change_of(value: float, baseline: float) -> float:
    return 100.0 * (value - baseline) / baseline


def lane_use(arity: int) -> float:
    """The share of the group that has a child to test, for one turn of the loop.

    The loop gives one child to each thread. An arity below the thread count therefore leaves the
    rest of the group idle, and a narrow tree pays for that at every turn.
    """
    return 100.0 * min(arity, GROUP_THREADS) / GROUP_THREADS


def main() -> None:
    print("Loading VolumeTiledForwardShading ...")
    ctx = vtf.VtfDevice()
    print(f"device: {ctx.device.info.adapter_name} ({ctx.device.info.api_name})")

    grid = vtf.ClusterGrid()
    cluster_min, cluster_max = grid.cluster_aabbs(ctx)
    print(f"\ncluster grid {grid.grid[0]} by {grid.grid[1]} by {grid.grid[2]}, "
          f"{grid.cluster_count} clusters, near {grid.near}, far {grid.far}")

    print("scene: make_lights_realistic, uniform world density with a local room")
    lights, positions, ranges = vtf.make_lights_realistic(grid, LIGHT_COUNT)

    touches = vtf.count_clusters_per_light(positions, ranges, cluster_min, cluster_max)
    irreducible = int(touches.sum())
    print(f"{LIGHT_COUNT} lights, {irreducible} (cluster, light) pairs that really touch")

    # Every arm sorts the same way. Only the leaf size differs, so a difference belongs to it.
    root_min = (positions - ranges[:, None]).min(axis=0)
    root_max = (positions + ranges[:, None]).max(axis=0)
    cells = vtf.reference_morton_cells(positions, root_min, root_max, 10)
    order = np.argsort(vtf.morton_from_cells(cells, 10), kind="stable")

    print("\n" + "=" * 92)
    print("Every test, at every level")
    print("=" * 92)
    print(f"\n  {'lights per leaf':>15} {'levels':>7} {'nodes':>8} {'total tests':>11} "
          f"{'ratio':>7} {'vs 32':>8} {'entered':>10} {'vs 32':>8}")

    measured = {}
    for leaf_size in LEAF_SIZES:
        levels = vtf.build_bvh_levels(positions, ranges, order, leaf_size, BRANCHING)
        measured[leaf_size] = vtf.count_traversal_tests(levels, cluster_min, cluster_max)

    baseline = measured[BRANCHING]["total_tests"]
    base_entered = entered_of(measured[BRANCHING])
    for leaf_size in LEAF_SIZES:
        row = measured[leaf_size]
        print(f"  {leaf_size:>15} {row['level_count']:>7} {row['node_count']:>8} "
              f"{row['total_tests']:>11} {row['total_tests'] / irreducible:>7.2f} "
              f"{change_of(row['total_tests'], baseline):>7.1f}% "
              f"{entered_of(row):>10} {change_of(entered_of(row), base_entered):>7.1f}%")

    print("\nThe old model, which counted the lowest level alone")
    print(f"\n  {'lights per leaf':>15} {'sphere':>11} {'lowest box':>11} {'total':>11} {'vs 32':>8}")
    old_baseline = (measured[BRANCHING]["sphere_tests"]
                    + measured[BRANCHING]["per_level"][-1]["visits"])
    for leaf_size in LEAF_SIZES:
        row = measured[leaf_size]
        old = row["sphere_tests"] + row["per_level"][-1]["visits"]
        change = 100.0 * (old - old_baseline) / old_baseline
        print(f"  {leaf_size:>15} {row['sphere_tests']:>11} "
              f"{row['per_level'][-1]['visits']:>11} {old:>11} {change:>7.1f}%")

    # A uniform tree needs one constant. A tree whose bottom differs from its interior needs new
    # level tables and a traversal that visits several bottom nodes at once. So the two arms cost
    # very different amounts of work, and the table says whether the second one buys anything.
    print("\n" + "=" * 92)
    print("A uniform tree, where every level holds the same count of children")
    print("=" * 92)
    print(f"\n  {'arity':>15} {'levels':>7} {'nodes':>8} {'total tests':>11} "
          f"{'ratio':>7} {'vs 32':>8} {'entered':>10} {'vs 32':>8} {'lanes':>6}")

    uniform = {}
    for arity in LEAF_SIZES:
        levels = vtf.build_bvh_levels(positions, ranges, order, arity, arity)
        row = vtf.count_traversal_tests(levels, cluster_min, cluster_max)
        uniform[arity] = row
        print(f"  {arity:>15} {row['level_count']:>7} {row['node_count']:>8} "
              f"{row['total_tests']:>11} {row['total_tests'] / irreducible:>7.2f} "
              f"{change_of(row['total_tests'], baseline):>7.1f}% "
              f"{entered_of(row):>10} {change_of(entered_of(row), base_entered):>7.1f}% "
              f"{lane_use(arity):>5.0f}%")

    # The turn count and the test count move in opposite directions, so neither one alone ranks the
    # arms. This table gives the total time under a range of prices for one turn. It holds the price
    # of one test at TEST_PRICE, which a device measured. A turn costs two group barriers and one pop
    # that thread 0 performs alone, so the true price is tens of nanoseconds and not one.
    #
    # Read the 0 ns column as the best case a narrow tree can reach. Read every other column as the
    # case that the hardware really gives.
    print("\nWhat a narrow tree costs, against a range of prices for one turn of the loop")
    print(f"\n  {'arity':>15}" + "".join(f"{price:>11.0f} ns" for price in TURN_PRICES))

    for arity in LEAF_SIZES:
        row = uniform[arity]
        cells = []
        for price in TURN_PRICES:
            time_of = row["total_tests"] * TEST_PRICE + entered_of(row) * price
            base = baseline * TEST_PRICE + base_entered * price
            cells.append(f"{change_of(time_of, base):>10.1f}% ")
        print(f"  {arity:>15}" + "".join(cells))

    # The level table for the two arms that matter. It shows where a narrow bottom spends.
    for leaf_size in (BRANCHING, 4):
        print(f"\nWhere a leaf of {leaf_size} lights spends, from the root down")
        print(f"  {'level':>6} {'nodes':>8} {'entered':>12} {'tests':>12}")
        for depth, level in enumerate(measured[leaf_size]["per_level"]):
            print(f"  {depth:>6} {level['nodes']:>8} {level['visits']:>12} {level['tests']:>12}")


if __name__ == "__main__":
    main()
