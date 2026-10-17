"""Compares the stage dumps of a module against the known good files.

The cooker writes six stage dumps for each module and target. `tests/known_good/` holds one accepted
copy of each. This script
cooks the module once for each target, compares each dump against its accepted copy, and prints one
line for each stage.

A diff does not always mean a bug, of course. We could have updated the schema by adding, removing,
or changing fields. The script just reports what changed and where, and that it's correct. To accept
the reported changes, use `--accept` to overwrite and update the known_good files.

Note that this does invoke the cooker if given a module path, which it runs as a subprocess. This
uses the `lodestone_cooker_console` target, which is a target that allows for better granularity
in terms of stage dumps and output writing. If the API/args to that change, this script will
likely break weirdly.

Usage:

    python scripts/check-known-good.py
    python scripts/check-known-good.py --accept
    python scripts/check-known-good.py --module tests/assets/EntryPointParams.slang
    python scripts/check-known-good.py --config Debug --preset ninja-msvc

Exit code 0 means every stage matched. Exit code 1 means at least one differed, or the cook failed.

Line endings are normalized before the comparison. `.gitattributes` keeps JSON at LF in the working
tree, so a difference here should never be a line ending. The normalization is a second defence, and
it stops a checkout on another machine from reporting a false failure.
"""

import argparse
import difflib
import pathlib
import shutil
import subprocess
import sys
import tempfile

STAGES = ("Space", "Variants", "Raw", "Resolved", "Interned", "Cooked")
# The cooker names a dump `<module>_<target>_<Stage>.json`. The known good copy has the same name.
# Each target cooks on its own, so a dump of one target never depends on the other.
TARGETS = ("wgsl", "spirv")
# The default regime cooks each KitchenSink module on its own, against the KitchenSink policy, and
# compares its six dumps. Each module's dumps are named by its stem, so the sets never collide.
DEFAULT_MODULES = (
    "tests/assets/KitchenSink/KsGeometry.slang",
    "tests/assets/KitchenSink/KsMaterial.slang",
    "tests/assets/KitchenSink/KsPost.slang",
    "tests/assets/KitchenSink/KsShading.slang",
    "tests/assets/KitchenSink/KsVolume.slang"
)
KNOWN_GOOD = "tests/known_good"


def find_repository_root() -> pathlib.Path:
    """The script lives in scripts/, so the repository is its parent."""
    return pathlib.Path(__file__).resolve().parent.parent


def find_cooker(root: pathlib.Path, preset: str, config: str) -> pathlib.Path:
    """Finds the cooker console for one preset and configuration."""
    candidate = (
        root / "build" / preset / "tools" / "cooker_console" / config / "lodestone_cooker_console.exe"
    )
    if candidate.exists():
        return candidate

    # Fall back to a search, because the output directory has moved before.
    matches = sorted((root / "build" / preset).rglob("lodestone_cooker_console*"))
    for match in matches:
        if config in match.parts:
            return match

    raise SystemExit(
        f"no cooker console under build/{preset} for {config}. Build it first with scripts/build.bat."
    )


def normalize(path: pathlib.Path) -> list:
    """Reads one file as lines, with the line endings removed."""
    text = path.read_bytes().decode("utf-8")
    return text.replace("\r\n", "\n").split("\n")


def cook(cooker: pathlib.Path, module: pathlib.Path, target: str, out_directory: pathlib.Path) -> None:
    """Runs one cook with every stage dump turned on."""
    result = subprocess.run(
        [
            str(cooker),
            "-o",
            str(out_directory),
            f"--target={target}",
            "--dump-stage=all",
            str(module),
            "--policy-file",
            str("tests/assets/KitchenSink/KitchenSink.toml")
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"the cook failed with exit code {result.returncode}")


def diff_report(left: list, right: list, from_name: str, to_name: str, context: int) -> list:
    """Returns a unified diff of two dumps as a list of lines.

    difflib's matcher is slow on long JSON with many repeated lines (`},` and `],`): when thousands of lines
    differ, one file took minutes. A dump whose values change keeps its line count, so two files with the same
    count are compared line by line, which is linear. Only the report uses this. The pass or fail decision is
    the exact comparison of the two line lists."""
    if len(left) != len(right):
        return list(difflib.unified_diff(left, right, fromfile=from_name, tofile=to_name, n=context, lineterm=""))

    changed = [i for i, (a, b) in enumerate(zip(left, right)) if a != b]
    report = [f"--- {from_name}", f"+++ {to_name}"]
    position = 0
    while position < len(changed):
        # one hunk: changed lines closer than 2 * context apart share their context lines
        end = position
        while end + 1 < len(changed) and changed[end + 1] - changed[end] <= 2 * context:
            end += 1
        first = max(changed[position] - context, 0)
        last = min(changed[end] + context, len(left) - 1)
        count = last - first + 1
        report.append(f"@@ -{first + 1},{count} +{first + 1},{count} @@")
        for i in range(first, last + 1):
            if left[i] == right[i]:
                report.append(f" {left[i]}")
            else:
                report.append(f"-{left[i]}")
                report.append(f"+{right[i]}")
        position = end + 1
    return report


def check_module(cooker: pathlib.Path, module: pathlib.Path, target: str, known_good: pathlib.Path,
                 context: int, accept: bool) -> list:
    """Cooks one module for one target and compares its six dumps. Returns a label for each stage
    that differs.

    Each dump is named by the module stem and the target, so two cooks never write the same file. The
    report prints the stem and the target beside the stage, so a mixed run stays readable."""
    stem = f"{module.stem}_{target}"
    differing = []
    with tempfile.TemporaryDirectory() as temporary:
        out_directory = pathlib.Path(temporary)
        cook(cooker, module, target, out_directory)

        for stage in STAGES:
            name = f"{stem}_{stage}.json"
            produced = out_directory / name
            accepted = known_good / name

            if not produced.exists():
                print(f"  MISSING   {stem} {stage:<9} the cook wrote no dump for this stage")
                differing.append(f"{stem}/{stage}")
                continue

            if not accepted.exists():
                print(f"  NEW       {stem} {stage:<9} no known good file exists yet")
                differing.append(f"{stem}/{stage}")
                if accept:
                    shutil.copyfile(produced, accepted)
                continue

            left = normalize(accepted)
            right = normalize(produced)
            if left == right:
                print(f"  same      {stem} {stage}")
                continue

            report = diff_report(left, right, f"known_good/{name}", f"cooked/{name}", context)
            changed = sum(1 for line in report if line[:1] in "+-" and not line.startswith(("+++", "---")))
            print(f"  DIFFERS   {stem} {stage:<9} {changed} lines")
            differing.append(f"{stem}/{stage}")

            for line in report:
                print(f"      {line}")

            if accept:
                shutil.copyfile(produced, accepted)

    return differing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", default=None,
                        help="cook only this module, instead of the default set")
    parser.add_argument("--preset", default="ninja-msvc", help="build preset directory")
    parser.add_argument("--config", default="RelWithDebInfo", help="Debug or RelWithDebInfo")
    parser.add_argument(
        "--accept",
        action="store_true",
        help="copy the new dumps over the known good files. Use only for an intended change.",
    )
    parser.add_argument("--context", type=int, default=3, help="lines of context in a report")
    arguments = parser.parse_args()

    root = find_repository_root()
    cooker = find_cooker(root, arguments.preset, arguments.config)

    entries = [arguments.module] if arguments.module else list(DEFAULT_MODULES)
    modules = []
    for entry in entries:
        module = root / entry
        if not module.exists():
            raise SystemExit(f"no module at {module}")
        modules.append(module)

    known_good = root / KNOWN_GOOD

    differing = []
    for module in modules:
        for target in TARGETS:
            differing.extend(check_module(cooker, module, target, known_good, arguments.context,
                                          arguments.accept))

    total_stages = len(modules) * len(TARGETS) * len(STAGES)
    if not differing:
        print("all stages match the known good dumps")
        return 0

    if arguments.accept:
        print(f"accepted {len(differing)} changed dumps into {KNOWN_GOOD}")
        return 0

    print(f"{len(differing)} of {total_stages} stages differ")
    print("Read the report. Run again with --accept only when the change is intended.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
