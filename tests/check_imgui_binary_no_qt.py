#!/usr/bin/env python3

import pathlib
import platform
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_imgui_binary_no_qt.py <binary>", file=sys.stderr)
        return 2

    binary = pathlib.Path(sys.argv[1])
    if not binary.exists():
        print(f"binary not found: {binary}", file=sys.stderr)
        return 2

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
    forbidden_markers = ("Qt6", "Qt5", "libQt")
    if any(marker in output for marker in forbidden_markers):
        print(output, end="")
        print("jcut_imgui must not link Qt libraries", file=sys.stderr)
        return 1

    print("jcut_imgui has no Qt linkage")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
