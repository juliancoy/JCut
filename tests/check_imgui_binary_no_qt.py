#!/usr/bin/env python3

import argparse
import pathlib
import platform
import subprocess
import sys


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that a binary does not link Qt, or one Qt component."
    )
    parser.add_argument(
        "--component",
        help="Only forbid this Qt component (for example, Widgets).",
    )
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()

    binary = args.binary
    if not binary.exists():
        parser.error(f"binary not found: {binary}")

    system = platform.system()
    if system == "Linux":
        cmd = ["ldd", str(binary)]
    elif system == "Darwin":
        cmd = ["otool", "-L", str(binary)]
    else:
        print(f"unsupported platform for Qt linkage check: {system}", file=sys.stderr)
        return 77

    result = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        print(result.stdout, end="")
        print(result.stderr, end="", file=sys.stderr)
        return result.returncode

    output = result.stdout
    if args.component:
        component = args.component.strip()
        if not component:
            parser.error("--component must not be empty")
        forbidden_markers = (
            f"Qt6{component}",
            f"Qt5{component}",
            f"Qt{component}",
        )
        linkage_description = f"Qt {component}"
    else:
        forbidden_markers = ("Qt6", "Qt5", "libQt")
        linkage_description = "Qt"

    if any(marker in output for marker in forbidden_markers):
        print(output, end="")
        print(
            f"{binary.name} must not link {linkage_description} libraries",
            file=sys.stderr,
        )
        return 1

    print(f"{binary.name} has no {linkage_description} linkage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
