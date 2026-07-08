#!/usr/bin/env python3
"""Generate compiler -H include trees from a CMake compile database."""

from __future__ import annotations

import argparse
import json
import pathlib
import shlex
import subprocess
import sys


def load_compile_database(path: pathlib.Path) -> list[dict]:
    if not path.exists():
        raise FileNotFoundError(
            f"{path} does not exist. Configure with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON."
        )
    return json.loads(path.read_text())


def entry_arguments(entry: dict) -> list[str]:
    if "arguments" in entry:
        return list(entry["arguments"])
    return shlex.split(entry["command"])


def syntax_only_command(entry: dict, compiler: str | None) -> list[str]:
    args = entry_arguments(entry)
    if compiler:
        args[0] = compiler

    cleaned: list[str] = []
    skip_next = False
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg == "-c":
            continue
        if arg == "-o":
            skip_next = True
            continue
        if arg.startswith("-o") and len(arg) > 2:
            continue
        cleaned.append(arg)

    return cleaned[:1] + ["-H", "-fsyntax-only"] + cleaned[1:]


def matches_target(entry_file: pathlib.Path, target: str) -> bool:
    target_path = pathlib.Path(target)
    if target_path.is_absolute():
        return entry_file == target_path
    return entry_file.name == target or str(entry_file).endswith(target)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate compiler -H include trees for selected translation units."
    )
    parser.add_argument(
        "targets",
        nargs="+",
        help="Translation-unit basenames or path suffixes, e.g. render_vulkan_shared.cpp",
    )
    parser.add_argument(
        "-p",
        "--compile-db",
        default="build/compile_commands.json",
        help="Path to compile_commands.json",
    )
    parser.add_argument(
        "-o",
        "--out-dir",
        default="artifacts/include_audit/compiler",
        help="Output directory for *.includes.txt and *.cmd.txt",
    )
    parser.add_argument(
        "--compiler",
        default=None,
        help="Override compiler, e.g. clang++. By default the compile database compiler is used.",
    )
    args = parser.parse_args()

    compile_db_path = pathlib.Path(args.compile_db).resolve()
    out_dir = pathlib.Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    entries = load_compile_database(compile_db_path)
    found: set[str] = set()
    exit_code = 0

    for target in args.targets:
        match = None
        for entry in entries:
            entry_file = pathlib.Path(entry.get("file", "")).resolve()
            if matches_target(entry_file, target):
                match = entry
                break
        if not match:
            print(f"missing target in compile database: {target}", file=sys.stderr)
            exit_code = 1
            continue

        entry_file = pathlib.Path(match["file"]).resolve()
        stem = entry_file.name
        cmd = syntax_only_command(match, args.compiler)
        proc = subprocess.run(
            cmd,
            cwd=match.get("directory", str(entry_file.parent)),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        (out_dir / f"{stem}.cmd.txt").write_text(
            " ".join(shlex.quote(part) for part in cmd) + "\n"
        )
        (out_dir / f"{stem}.includes.txt").write_text(proc.stderr)
        (out_dir / f"{stem}.stdout.txt").write_text(proc.stdout)

        print(
            f"{stem}: exit {proc.returncode}, "
            f"{len(proc.stderr.splitlines())} include/output lines"
        )
        found.add(target)
        if proc.returncode != 0:
            exit_code = proc.returncode

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
