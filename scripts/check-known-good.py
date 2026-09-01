"""Compares the stage dumps of a module against the known good files.

The cooker writes six stage dumps. `tests/known_good/` holds one accepted copy of each. This script
cooks the module, compares each dump against its accepted copy, and prints one line for each stage.

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

STAGES = ("space", "variants", "raw", "resolved", "interned", "cooked")
DEFAULT_MODULE = "tests/assets/compute/Ocean/OceanFft.slang"
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


def cook(cooker: pathlib.Path, module: pathlib.Path, out_directory: pathlib.Path) -> None:
    """Runs one cook with every stage dump turned on."""
    result = subprocess.run(
        [
            str(cooker),
            "-o",
            str(out_directory / "dump"),
            "--dump-stage=all",
            str(module),
        ],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit(f"the cook failed with exit code {result.returncode}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--module", default=DEFAULT_MODULE, help="module to cook")
    parser.add_argument("--preset", default="ninja-clang-cl", help="build preset directory")
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
    module = root / arguments.module
    if not module.exists():
        raise SystemExit(f"no module at {module}")

    known_good = root / KNOWN_GOOD
    stem = module.stem

    with tempfile.TemporaryDirectory() as temporary:
        out_directory = pathlib.Path(temporary)
        cook(cooker, module, out_directory)

        differing = []
        for stage in STAGES:
            name = f"{stem}.stage-{stage}.json"
            produced = out_directory / name
            accepted = known_good / name

            if not produced.exists():
                print(f"  MISSING   {stage:<9} the cook wrote no dump for this stage")
                differing.append(stage)
                continue

            if not accepted.exists():
                print(f"  NEW       {stage:<9} no known good file exists yet")
                differing.append(stage)
                if arguments.accept:
                    shutil.copyfile(produced, accepted)
                continue

            left = normalize(accepted)
            right = normalize(produced)
            if left == right:
                print(f"  same      {stage}")
                continue

            changed = sum(1 for line in difflib.ndiff(left, right) if line[0] in "+-")
            print(f"  DIFFERS   {stage:<9} {changed} lines")
            differing.append(stage)

            for line in difflib.unified_diff(
                left, right, fromfile=f"known_good/{name}", tofile=f"cooked/{name}",
                n=arguments.context, lineterm="",
            ):
                print(f"      {line}")

            if arguments.accept:
                shutil.copyfile(produced, accepted)

        if not differing:
            print("all stages match the known good dumps")
            return 0

        if arguments.accept:
            print(f"accepted {len(differing)} changed dumps into {KNOWN_GOOD}")
            return 0

        print(f"{len(differing)} of {len(STAGES)} stages differ")
        print("Read the report. Run again with --accept only when the change is intended.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
