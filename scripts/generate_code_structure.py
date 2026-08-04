#!/usr/bin/env python3
"""Generate a procedural, repository-wide source structure index.

The JSON artifact is the canonical output.  It contains every discovered
first-party source file, its dependencies, and its structural symbols.  The
Markdown artifact is a refactor-oriented view of the same data.
"""

from __future__ import annotations

import argparse
import ast
import concurrent.futures
import datetime as dt
import fnmatch
import hashlib
import json
import re
import shutil
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

try:
    from scripts.clang_ast_structure import load_compile_commands, parse_translation_unit
    from scripts.code_dependency_graph import build_dependency_graph
except ModuleNotFoundError:
    from clang_ast_structure import load_compile_commands, parse_translation_unit
    from code_dependency_graph import build_dependency_graph


SCHEMA = "jcut_code_structure_v6"
OWNERSHIP_SCHEMA = "jcut_code_ownership_v1"
GENERATOR_VERSION = "6"
VIEWER_SCHEMA = "jcut_code_structure_viewer_v1"
VIEWER_ASSET_NAMES = ("index.html", "app.css", "app.js")
SOURCE_SUFFIXES = {
    ".c": "C",
    ".cc": "C++",
    ".cpp": "C++",
    ".cxx": "C++",
    ".h": "C++",
    ".hh": "C++",
    ".hpp": "C++",
    ".hxx": "C++",
    ".inl": "C++",
    ".inc": "C++",
    ".cu": "CUDA",
    ".cuh": "CUDA",
    ".py": "Python",
    ".sh": "Shell",
    ".bash": "Shell",
    ".cmake": "CMake",
    ".js": "JavaScript",
    ".mjs": "JavaScript",
    ".ts": "TypeScript",
    ".html": "HTML",
    ".htm": "HTML",
    ".css": "CSS",
    ".qml": "QML",
    ".vert": "GLSL",
    ".frag": "GLSL",
    ".comp": "GLSL",
    ".geom": "GLSL",
    ".tesc": "GLSL",
    ".tese": "GLSL",
    ".rgen": "GLSL",
    ".rchit": "GLSL",
    ".rmiss": "GLSL",
    ".rahit": "GLSL",
    ".glsl": "GLSL",
}
SPECIAL_FILENAMES = {
    "CMakeLists.txt": "CMake",
    "Makefile": "Make",
    "Dockerfile": "Dockerfile",
}
DEFAULT_EXCLUDES = (
    ".git/**",
    ".cache/**",
    ".deps/**",
    ".pytest_cache/**",
    "__pycache__/**",
    "build/**",
    "build-*/**",
    "CMakeFiles/**",
    "Testing/**",
    "generated/**",
    "artifacts/**",
    "profiling/**",
    "testbench_assets/**",
    "external/**",
    "third_party/**",
    "ffmpeg/**",
    "ffbuild/**",
    "ffmpeg-build/**",
    "ffmpeg-install/**",
    "nv-codec-headers/**",
    "rtaudio/**",
    "loiacono/rtaudio/**",
    "birefnet/**",
)
STRUCTURAL_KINDS = {
    "class",
    "struct",
    "union",
    "enum",
    "namespace",
    "function",
    "method",
    "macro",
    "target",
    "project",
    "module",
    "interface",
}


def language_for(path: Path) -> str | None:
    if path.name in SPECIAL_FILENAMES:
        return SPECIAL_FILENAMES[path.name]
    if path.name.endswith(".Dockerfile"):
        return "Dockerfile"
    return SOURCE_SUFFIXES.get(path.suffix.lower())


def is_excluded(relative: str, patterns: Iterable[str]) -> bool:
    normalized = relative.replace("\\", "/")
    return any(
        fnmatch.fnmatch(normalized, pattern)
        or (pattern.endswith("/**") and normalized == pattern[:-3])
        for pattern in patterns
    )


def discover_files(root: Path, excludes: tuple[str, ...]) -> list[Path]:
    command = [
        "git",
        "ls-files",
        "--cached",
        "--others",
        "--exclude-standard",
        "-z",
    ]
    result = subprocess.run(command, cwd=root, capture_output=True, check=False)
    if result.returncode == 0:
        candidates = [Path(item.decode("utf-8", "surrogateescape"))
                      for item in result.stdout.split(b"\0") if item]
    else:
        candidates = [path.relative_to(root) for path in root.rglob("*")
                      if path.is_file()]
    return sorted(
        path for path in candidates
        if (root / path).is_file()
        and language_for(path)
        and not is_excluded(path.as_posix(), excludes)
    )


def sha256_file(path: Path) -> str | None:
    if not path.is_file():
        return None
    return hashlib.sha256(path.read_bytes()).hexdigest()


def display_path(root: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(root).as_posix()
    except ValueError:
        return str(path.resolve())


def git_output(root: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", *arguments],
        cwd=root,
        capture_output=True,
        check=False,
    )
    return result.stdout if result.returncode == 0 else b""


def git_commit(root: Path) -> str | None:
    value = git_output(root, "rev-parse", "HEAD").decode("ascii", "replace").strip()
    return value or None


def git_dirty_state(root: Path, paths: Iterable[Path]) -> tuple[str, bool, int]:
    relevant = sorted({path.as_posix() for path in paths})
    if not relevant:
        payload = b""
    else:
        payload = git_output(
            root,
            "status",
            "--porcelain=v1",
            "-z",
            "--untracked-files=all",
            "--",
            *relevant,
        )
    entries = [entry for entry in payload.split(b"\0") if entry]
    digest = hashlib.sha256(payload)
    if entries:
        for relative in relevant:
            path = root / relative
            file_digest = sha256_file(path)
            if file_digest is not None:
                digest.update(relative.encode("utf-8", "surrogateescape"))
                digest.update(b"\0")
                digest.update(file_digest.encode("ascii"))
                digest.update(b"\0")
    return digest.hexdigest(), bool(entries), len(entries)


def corpus_fingerprint(files: Iterable[dict[str, Any]]) -> str:
    digest = hashlib.sha256()
    for item in sorted(files, key=lambda entry: entry["path"]):
        digest.update(item["path"].encode("utf-8", "surrogateescape"))
        digest.update(b"\0")
        digest.update(item["sha256"].encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest()


def freshness_metadata(
    root: Path,
    files: list[dict[str, Any]],
    ownership_manifest_path: Path,
    build_file_path: Path,
    compile_commands_path: Path,
) -> dict[str, Any]:
    relevant_paths = [Path(item["path"]) for item in files]
    for path in (ownership_manifest_path, build_file_path, compile_commands_path):
        if path.exists():
            try:
                relevant_paths.append(path.resolve().relative_to(root))
            except ValueError:
                pass
    dirty_fingerprint, dirty, dirty_entries = git_dirty_state(root, relevant_paths)
    generator_path = Path(__file__).resolve()
    return {
        "generator_version": GENERATOR_VERSION,
        "generator_sha256": sha256_file(generator_path),
        "git_commit": git_commit(root),
        "git_dirty": dirty,
        "git_dirty_entries": dirty_entries,
        "dirty_state_fingerprint": dirty_fingerprint,
        "corpus_fingerprint": corpus_fingerprint(files),
        "ownership_manifest": {
            "path": display_path(root, ownership_manifest_path),
            "sha256": sha256_file(ownership_manifest_path),
        },
        "build_file": {
            "path": display_path(root, build_file_path),
            "sha256": sha256_file(build_file_path),
        },
        "compile_commands": {
            "path": display_path(root, compile_commands_path),
            "sha256": sha256_file(compile_commands_path),
        },
    }


def current_file_records(root: Path, paths: Iterable[Path]) -> list[dict[str, str]]:
    records = []
    for relative in paths:
        digest = sha256_file(root / relative)
        if digest is not None:
            records.append({"path": relative.as_posix(), "sha256": digest})
    return records


def validate_current(
    root: Path,
    index: dict[str, Any],
    paths: list[Path],
    ownership_manifest_path: Path,
    build_file_path: Path,
    compile_commands_path: Path,
) -> list[str]:
    errors: list[str] = []
    recorded_files = index.get("files")
    recorded_freshness = index.get("freshness")
    if index.get("schema") != SCHEMA:
        errors.append(f"schema changed: recorded {index.get('schema')!r}, current {SCHEMA!r}")
    if not isinstance(recorded_files, list):
        return errors + ["artifact has no files array"]
    if not isinstance(recorded_freshness, dict):
        return errors + ["artifact has no freshness metadata"]

    current_files = current_file_records(root, paths)
    recorded_by_path = {item["path"]: item.get("sha256") for item in recorded_files}
    current_by_path = {item["path"]: item["sha256"] for item in current_files}
    added = sorted(current_by_path.keys() - recorded_by_path.keys())
    removed = sorted(recorded_by_path.keys() - current_by_path.keys())
    changed = sorted(
        path for path in current_by_path.keys() & recorded_by_path.keys()
        if current_by_path[path] != recorded_by_path[path]
    )
    errors.extend(f"indexed file added: {path}" for path in added)
    errors.extend(f"indexed file removed: {path}" for path in removed)
    errors.extend(f"indexed file changed: {path}" for path in changed)

    current_freshness = freshness_metadata(
        root,
        current_files,
        ownership_manifest_path,
        build_file_path,
        compile_commands_path,
    )
    scalar_fields = (
        "generator_version",
        "generator_sha256",
        "git_commit",
        "dirty_state_fingerprint",
        "corpus_fingerprint",
    )
    for field in scalar_fields:
        if recorded_freshness.get(field) != current_freshness.get(field):
            errors.append(
                f"{field} changed: recorded {recorded_freshness.get(field)!r}, "
                f"current {current_freshness.get(field)!r}"
            )
    for field in ("ownership_manifest", "build_file", "compile_commands"):
        if recorded_freshness.get(field) != current_freshness.get(field):
            errors.append(
                f"{field} changed: recorded {recorded_freshness.get(field)!r}, "
                f"current {current_freshness.get(field)!r}"
            )
    return errors


def load_ownership_manifest(path: Path | None) -> dict[str, Any] | None:
    if path is None or not path.exists():
        return None
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if manifest.get("schema") != OWNERSHIP_SCHEMA:
        raise RuntimeError(f"Unsupported ownership manifest schema in {path}")
    if not isinstance(manifest.get("rules"), list):
        raise RuntimeError(f"Ownership manifest has no rules array: {path}")
    return manifest


def resolve_declared_ownership(
    relative: str,
    manifest: dict[str, Any] | None,
) -> tuple[dict[str, Any] | None, list[str]]:
    if manifest is None:
        return None, []
    matches = []
    for rule in manifest["rules"]:
        if any(fnmatch.fnmatch(relative, pattern) for pattern in rule.get("patterns", [])):
            matches.append(rule)
    if not matches:
        return None, ["missing_declared_owner"]
    priority = max(int(rule.get("priority", 0)) for rule in matches)
    winners = [rule for rule in matches if int(rule.get("priority", 0)) == priority]
    identities = {(rule.get("owner"), rule.get("layer")) for rule in winners}
    violations = ["ambiguous_declared_owner"] if len(identities) > 1 else []
    winner = winners[0]
    return {
        "declared_owner": winner.get("owner"),
        "layer": winner.get("layer"),
        "declared_target": winner.get("declared_target"),
        "matched_rule_priority": priority,
    }, violations


def build_targets_from_ninja(root: Path, ninja_path: Path | None) -> dict[str, list[str]]:
    if ninja_path is None:
        ninja_path = root / "build" / "build.ninja"
    if not ninja_path.exists():
        return {}
    result: dict[str, set[str]] = defaultdict(set)
    root_prefix = str(root) + "/"
    for line in ninja_path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith("build CMakeFiles/") or ".dir/" not in line or ".o:" not in line:
            continue
        target = line.split("CMakeFiles/", 1)[1].split(".dir/", 1)[0]
        for token in line.split():
            if token.startswith(root_prefix):
                result[token[len(root_prefix):]].add(target)
    return {path: sorted(targets) for path, targets in result.items()}


def production_build_targets(targets: Iterable[str]) -> list[str]:
    return sorted(target for target in targets if not target.startswith("test_"))


def line_count(data: bytes) -> int:
    if not data:
        return 0
    return data.count(b"\n") + (not data.endswith(b"\n"))


def symbol(
    *,
    name: str,
    kind: str,
    line: int,
    end_line: int | None = None,
    scope: str = "",
    signature: str = "",
    access: str = "",
    parser: str,
) -> dict[str, Any]:
    end = max(line, end_line or line)
    qualified = f"{scope}::{name}" if scope else name
    return {
        "name": name,
        "qualified_name": qualified,
        "kind": kind,
        "line": line,
        "end_line": end,
        "span_lines": end - line + 1,
        "scope": scope,
        "signature": signature,
        "access": access,
        "parser": parser,
    }


def python_structure(text: str, path: str) -> tuple[list[dict[str, Any]], list[str], str | None]:
    try:
        tree = ast.parse(text, filename=path)
    except SyntaxError as error:
        return [], [], f"SyntaxError at line {error.lineno}: {error.msg}"

    symbols: list[dict[str, Any]] = []
    dependencies: list[str] = []

    def visit(nodes: list[ast.stmt], scope: str = "") -> None:
        for node in nodes:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                prefix = "async " if isinstance(node, ast.AsyncFunctionDef) else ""
                args = [arg.arg for arg in node.args.posonlyargs + node.args.args]
                if node.args.vararg:
                    args.append(f"*{node.args.vararg.arg}")
                args.extend(arg.arg for arg in node.args.kwonlyargs)
                if node.args.kwarg:
                    args.append(f"**{node.args.kwarg.arg}")
                symbols.append(symbol(
                    name=node.name,
                    kind="method" if scope else "function",
                    line=node.lineno,
                    end_line=getattr(node, "end_lineno", node.lineno),
                    scope=scope,
                    signature=f"{prefix}({', '.join(args)})",
                    parser="python-ast",
                ))
                visit(node.body, f"{scope}.{node.name}" if scope else node.name)
            elif isinstance(node, ast.ClassDef):
                symbols.append(symbol(
                    name=node.name,
                    kind="class",
                    line=node.lineno,
                    end_line=getattr(node, "end_lineno", node.lineno),
                    scope=scope,
                    parser="python-ast",
                ))
                visit(node.body, f"{scope}.{node.name}" if scope else node.name)
            elif isinstance(node, (ast.Import, ast.ImportFrom)):
                if isinstance(node, ast.Import):
                    dependencies.extend(alias.name for alias in node.names)
                else:
                    dots = "." * node.level
                    if node.module:
                        dependencies.append(dots + node.module)
                    else:
                        dependencies.extend(
                            dots + alias.name for alias in node.names
                            if alias.name != "*"
                        )

    visit(tree.body)
    return symbols, sorted(set(dependencies)), None


INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SHELL_SOURCE_RE = re.compile(r'^\s*(?:source|\.)\s+["\']?([^\s"\']+)', re.MULTILINE)
CMAKE_DEP_RE = re.compile(
    r"\b(?:include|add_subdirectory)\s*\(\s*[\"']?([^\s\"')]+)", re.IGNORECASE
)


def text_dependencies(language: str, text: str) -> list[str]:
    if language in {"C", "C++", "CUDA", "GLSL"}:
        return sorted(set(INCLUDE_RE.findall(text)))
    if language == "Shell":
        return sorted(set(SHELL_SOURCE_RE.findall(text)))
    if language == "CMake":
        return sorted(set(CMAKE_DEP_RE.findall(text)))
    return []


SHADER_FUNCTION_RE = re.compile(
    r"(?m)^\s*(?!if\b|for\b|while\b|switch\b)"
    r"(?:[A-Za-z_]\w*\s+)+([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{"
)


def closing_brace_line(text: str, opening_offset: int) -> int | None:
    depth = 0
    state = "normal"
    escaped = False
    line = text.count("\n", 0, opening_offset) + 1
    index = opening_offset
    raw_terminator = ""
    while index < len(text):
        char = text[index]
        following = text[index:index + 2]
        if char == "\n":
            line += 1
            if state == "line-comment":
                state = "normal"
        if state in {"string", "character"}:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "normal"
        elif state == "line-comment":
            pass
        elif state == "block-comment":
            if following == "*/":
                state = "normal"
                index += 1
        elif state == "raw-string":
            if text.startswith(raw_terminator, index):
                state = "normal"
                index += len(raw_terminator) - 1
        elif following == "//":
            state = "line-comment"
            index += 1
        elif following == "/*":
            state = "block-comment"
            index += 1
        elif char == "R" and index + 1 < len(text) and text[index + 1] == '"':
            delimiter_end = text.find("(", index + 2, index + 19)
            if delimiter_end != -1:
                delimiter = text[index + 2:delimiter_end]
                if not any(one.isspace() or one in "\\()" for one in delimiter):
                    raw_terminator = ")" + delimiter + '"'
                    state = "raw-string"
                    index = delimiter_end
        elif char == '"':
            state = "string"
        elif char == "'" and not (
            index > 0
            and index + 1 < len(text)
            and text[index - 1].isalnum()
            and text[index + 1].isalnum()
        ):
            state = "character"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return line
        index += 1
    return None


def unmatched_braces(text: str) -> list[dict[str, Any]]:
    findings: list[dict[str, Any]] = []
    stack: list[tuple[int, int]] = []
    state = "normal"
    escaped = False
    line = 1
    column = 1
    index = 0
    raw_terminator = ""
    state_start = (1, 1)
    while index < len(text):
        char = text[index]
        following = text[index:index + 2]
        advance = 1
        if state in {"string", "character"}:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "character" and char == "'"):
                state = "normal"
        elif state == "line-comment":
            if char == "\n":
                state = "normal"
        elif state == "block-comment":
            if following == "*/":
                state = "normal"
                advance = 2
        elif state == "raw-string":
            if text.startswith(raw_terminator, index):
                state = "normal"
                advance = len(raw_terminator)
        elif following == "//":
            state = "line-comment"
            state_start = (line, column)
            advance = 2
        elif following == "/*":
            state = "block-comment"
            state_start = (line, column)
            advance = 2
        elif char == "R" and index + 1 < len(text) and text[index + 1] == '"':
            delimiter_end = text.find("(", index + 2, index + 19)
            if delimiter_end != -1:
                delimiter = text[index + 2:delimiter_end]
                if not any(one.isspace() or one in "\\()" for one in delimiter):
                    raw_terminator = ")" + delimiter + '"'
                    state = "raw-string"
                    state_start = (line, column)
                    advance = delimiter_end - index + 1
        elif char == '"':
            state = "string"
            state_start = (line, column)
        elif char == "'" and not (
            index > 0
            and index + 1 < len(text)
            and text[index - 1].isalnum()
            and text[index + 1].isalnum()
        ):
            state = "character"
            state_start = (line, column)
        elif char == "{":
            stack.append((line, column))
        elif char == "}":
            if stack:
                stack.pop()
            else:
                findings.append({
                    "kind": "unmatched-closing-brace",
                    "line": line,
                    "column": column,
                })

        consumed = text[index:index + advance]
        newline_count = consumed.count("\n")
        if newline_count:
            line += newline_count
            column = len(consumed.rsplit("\n", 1)[-1]) + 1
        else:
            column += advance
        index += advance

    findings.extend(
        {"kind": "unmatched-opening-brace", "line": brace_line, "column": brace_column}
        for brace_line, brace_column in stack
    )
    if state in {"block-comment", "string", "character", "raw-string"}:
        findings.append({
            "kind": f"unterminated-{state}",
            "line": state_start[0],
            "column": state_start[1],
        })
    return findings


def shader_structure(text: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    result = []
    for match in SHADER_FUNCTION_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        opening = text.find("{", match.start(), match.end())
        closing_line = closing_brace_line(text, opening)
        result.append(symbol(
            name=match.group(1),
            kind="function",
            line=line,
            end_line=closing_line or line_count(text.encode("utf-8")),
            signature=f"({match.group(2).strip()})",
            parser="shader-outline",
        ))
    return result, unmatched_braces(text)


def build_outline(symbols: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Build a lexical parent/child tree referencing entries in the symbol table."""
    roots: list[dict[str, Any]] = []
    stack: list[tuple[int, dict[str, Any], dict[str, Any]]] = []
    for index, entry in enumerate(symbols):
        while stack:
            _, parent, _ = stack[-1]
            contains = (
                parent["line"] <= entry["line"]
                and parent["end_line"] >= entry["end_line"]
                and (parent["line"], parent["end_line"])
                != (entry["line"], entry["end_line"])
            )
            if contains:
                break
            stack.pop()
        node = {"symbol_index": index, "children": []}
        if stack:
            stack[-1][2]["children"].append(node)
        else:
            roots.append(node)
        if entry["end_line"] > entry["line"]:
            stack.append((index, entry, node))
    return roots


def run_ctags(root: Path, paths: list[Path]) -> tuple[dict[str, list[dict[str, Any]]], list[str]]:
    executable = shutil.which("ctags")
    if not executable:
        raise RuntimeError("Universal Ctags is required (the 'ctags' executable was not found)")
    command = [
        executable,
        "--output-format=json",
        "--fields=+neKStia",
        "--extras=-F",
        "--kinds-C++=+p",
        "--kinds-C=+p",
        "-f",
        "-",
        *(f"./{path.as_posix()}" for path in paths),
    ]
    result = subprocess.run(command, cwd=root, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"ctags failed: {result.stderr.strip()}")
    by_path: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for raw_line in result.stdout.splitlines():
        tag = json.loads(raw_line)
        if tag.get("_type") != "tag" or "line" not in tag:
            continue
        kind = tag.get("kind", "symbol")
        if kind in {"header", "file"}:
            continue
        scope = tag.get("scope", "")
        tag_path = Path(tag["path"]).as_posix()
        if tag_path.startswith("./"):
            tag_path = tag_path[2:]
        by_path[tag_path].append(symbol(
            name=tag["name"],
            kind=kind,
            line=int(tag["line"]),
            end_line=int(tag.get("end", tag["line"])),
            scope=scope,
            signature=tag.get("signature", ""),
            access=tag.get("access", ""),
            parser="universal-ctags",
        ))
    warnings = [line for line in result.stderr.splitlines() if line.strip()]
    return by_path, warnings


def run_clang_ast(
    root: Path,
    paths: list[Path],
    compile_commands_path: Path,
    jobs: int,
) -> tuple[
    dict[str, list[dict[str, Any]]],
    dict[str, list[dict[str, Any]]],
    set[str],
    dict[str, str],
    dict[str, list[str]],
    list[str],
    dict[str, int],
]:
    indexed_paths = {
        path.as_posix() for path in paths
        if language_for(path) in {"C", "C++", "CUDA"}
    }
    commands = load_compile_commands(compile_commands_path, root, indexed_paths)
    indexed_paths_tuple = tuple(sorted(indexed_paths))
    symbols_by_path: dict[str, dict[tuple[Any, ...], dict[str, Any]]] = defaultdict(dict)
    calls_by_path: dict[str, dict[tuple[Any, ...], dict[str, Any]]] = defaultdict(dict)
    covered_paths: set[str] = set()
    failures: dict[str, str] = {}
    diagnostics_by_path: dict[str, list[str]] = defaultdict(list)
    warnings: list[str] = []
    succeeded = 0

    def consume(result: dict[str, Any], source: str) -> None:
        nonlocal succeeded
        try:
            source_path = Path(source).resolve().relative_to(root).as_posix()
        except ValueError:
            source_path = source
        error = result.get("error")
        if error:
            failures[source_path] = str(error)
            diagnostics_by_path[source_path].append(str(error))
            warnings.append(f"Clang AST fallback for {source_path}: {error}")
            return
        succeeded += 1
        covered_paths.add(source_path)
        covered_paths.update(result.get("covered_paths", []))
        for path, entries in result.get("symbols", {}).items():
            for entry in entries:
                key = (
                    entry.get("usr") or entry["qualified_name"],
                    entry["line"],
                    entry["kind"],
                )
                symbols_by_path[path][key] = entry
        for path, entries in result.get("calls", {}).items():
            for entry in entries:
                key = (
                    entry["caller_usr"] or entry["caller"],
                    entry["callee_usr"] or entry["callee"],
                    entry["line"],
                    entry["column"],
                )
                calls_by_path[path][key] = entry
        for diagnostic in result.get("diagnostics", []):
            diagnostics_by_path[source_path].append(diagnostic)
            warnings.append(f"Clang AST diagnostic for {source_path}: {diagnostic}")

    if jobs <= 1:
        for command in commands:
            consume(
                parse_translation_unit(command, str(root), indexed_paths_tuple),
                command.file,
            )
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=jobs) as executor:
            pending = {
                executor.submit(
                    parse_translation_unit,
                    command,
                    str(root),
                    indexed_paths_tuple,
                ): command
                for command in commands
            }
            for future in concurrent.futures.as_completed(pending):
                command = pending[future]
                try:
                    result = future.result()
                except Exception as error:
                    result = {"source": command.file, "error": f"worker failed: {error}"}
                consume(result, command.file)

    return (
        {path: list(entries.values()) for path, entries in symbols_by_path.items()},
        {path: list(entries.values()) for path, entries in calls_by_path.items()},
        covered_paths,
        failures,
        dict(diagnostics_by_path),
        warnings,
        {
            "compile_commands": len(commands),
            "translation_units_succeeded": succeeded,
            "translation_units_failed": len(failures),
            "files_covered": len(covered_paths),
        },
    )


def symbol_coverage_confidence(
    language: str,
    parser: str,
    parse_succeeded: bool,
) -> dict[str, str]:
    if not parse_succeeded:
        return {"level": "low", "reason": "the parser reported an error"}
    if parser == "clang-ast":
        return {
            "level": "high",
            "reason": "Clang compiled the translation unit and supplied semantic AST cursors",
        }
    if parser == "python-ast":
        return {
            "level": "high",
            "reason": "Python's native AST parsed the complete file",
        }
    if parser == "universal-ctags-fallback":
        return {
            "level": "low",
            "reason": "Ctags is a lexical fallback without compile or type resolution",
        }
    if parser == "shader-outline":
        return {
            "level": "medium",
            "reason": "brace validation succeeded, but shader symbols use a structural outline parser",
        }
    if parser == "inventory-only":
        return {"level": "low", "reason": "the file is inventoried without symbol parsing"}
    return {
        "level": "medium",
        "reason": f"{parser} supplies lexical symbols without semantic compilation",
    }


def diagnostics_for_path(warnings: Iterable[str], relative_path: str) -> list[str]:
    basename = Path(relative_path).name
    return sorted({
        warning for warning in warnings
        if relative_path in warning or basename in warning
    })


def analyze(
    root: Path,
    paths: list[Path],
    excludes: tuple[str, ...],
    ownership_manifest: dict[str, Any] | None = None,
    build_targets: dict[str, list[str]] | None = None,
    compile_commands_path: Path | None = None,
    clang_jobs: int = 1,
) -> dict[str, Any]:
    ctags_paths = [path for path in paths
                   if language_for(path) not in {"Python", "GLSL", "CSS"}]
    ctags_symbols, warnings = run_ctags(root, ctags_paths) if ctags_paths else ({}, [])
    ctags_warnings = list(warnings)
    if compile_commands_path is None:
        compile_commands_path = root / "build" / "compile_commands.json"
    (
        clang_symbols,
        clang_calls,
        clang_covered_paths,
        clang_failures,
        clang_diagnostics,
        clang_warnings,
        clang_summary,
    ) = run_clang_ast(root, paths, compile_commands_path, max(1, clang_jobs))
    warnings.extend(clang_warnings)
    files: list[dict[str, Any]] = []
    parse_errors: list[dict[str, str]] = []
    build_targets = build_targets or {}

    for relative in paths:
        absolute = root / relative
        data = absolute.read_bytes()
        text = data.decode("utf-8", "replace")
        language = language_for(relative) or "Unknown"
        dependencies = text_dependencies(language, text)
        parse_error = None
        calls: list[dict[str, Any]] = []
        parser_fallback_reason = None
        compilation_succeeded: bool | None = None
        diagnostics: list[str] = []
        unmatched_constructs: list[dict[str, Any]] = []
        suspicious_constructs: list[dict[str, Any]] = []
        if language == "Python":
            symbols, dependencies, parse_error = python_structure(text, relative.as_posix())
            if parse_error:
                diagnostics.append(parse_error)
        elif language == "GLSL":
            symbols, unmatched_constructs = shader_structure(text)
            if unmatched_constructs:
                parse_error = f"{len(unmatched_constructs)} unmatched or unterminated construct(s)"
                diagnostics.append(parse_error)
        elif language in {"C", "C++", "CUDA"}:
            if relative.as_posix() in clang_covered_paths:
                symbols = clang_symbols.get(relative.as_posix(), [])
                calls = clang_calls.get(relative.as_posix(), [])
                compilation_succeeded = True
                diagnostics.extend(clang_diagnostics.get(relative.as_posix(), []))
            else:
                symbols = [
                    {**entry, "parser": "universal-ctags-fallback"}
                    for entry in ctags_symbols.get(relative.as_posix(), [])
                ]
                parser_fallback_reason = clang_failures.get(
                    relative.as_posix(),
                    "no successful Clang translation unit covered this file",
                )
                compilation_succeeded = False if relative.as_posix() in clang_failures else None
                diagnostics.extend(clang_diagnostics.get(relative.as_posix(), []))
                diagnostics.extend(diagnostics_for_path(ctags_warnings, relative.as_posix()))
                unmatched_constructs = unmatched_braces(text)
                suspicious_constructs.append({
                    "kind": "semantic-parser-fallback",
                    "detail": parser_fallback_reason,
                })
        elif language == "CSS":
            symbols = []
        else:
            symbols = ctags_symbols.get(relative.as_posix(), [])
            diagnostics.extend(diagnostics_for_path(ctags_warnings, relative.as_posix()))
        if unmatched_constructs and parse_error is None:
            parse_error = (
                f"structural validation failed: {len(unmatched_constructs)} "
                "unmatched or unterminated construct(s)"
            )
            diagnostics.append(parse_error)
        symbols.sort(key=lambda item: (item["line"], -item["span_lines"], item["name"]))
        suspicious_constructs.extend(
            {
                "kind": "unresolved-call",
                "line": call["line"],
                "column": call["column"],
            }
            for call in calls
            if not call.get("callee_usr")
        )
        suspicious_constructs.extend(
            {"kind": "parser-diagnostic", "detail": diagnostic}
            for diagnostic in diagnostics
        )
        outline = build_outline(symbols)
        if parse_error:
            parse_errors.append({"path": relative.as_posix(), "error": parse_error})
        relative_path = relative.as_posix()
        ownership, ownership_violations = resolve_declared_ownership(
            relative_path, ownership_manifest
        )
        actual_targets = build_targets.get(relative_path, [])
        production_targets = production_build_targets(actual_targets)
        is_implementation = relative.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".cu"}
        if is_implementation and len(production_targets) > 1:
            ownership_violations.append("multiple_build_targets")
        if ownership and ownership.get("declared_target") and production_targets:
            if ownership["declared_target"] not in production_targets:
                ownership_violations.append("declared_target_mismatch")
        parser_used = ("+".join(sorted({entry["parser"] for entry in symbols})) if symbols else
                       "python-ast" if language == "Python" else
                       "shader-outline" if language == "GLSL" else
                       "clang-ast" if language in {"C", "C++", "CUDA"}
                       and relative.as_posix() in clang_covered_paths else
                       "universal-ctags-fallback" if language in {"C", "C++", "CUDA"} else
                       "universal-ctags" if language != "CSS" else "inventory-only")
        parse_succeeded = parse_error is None
        compilation_status = (
            "succeeded" if compilation_succeeded is True else
            "failed" if compilation_succeeded is False else
            "not-attempted" if language in {"C", "C++", "CUDA"} else
            "not-applicable"
        )
        file_record = {
            "path": relative.as_posix(),
            "language": language,
            "parser": parser_used,
            "parse_succeeded": parse_succeeded,
            "compilation_succeeded": compilation_succeeded,
            "compilation_status": compilation_status,
            "diagnostics": diagnostics,
            "parser_fallback_reason": parser_fallback_reason,
            "symbol_coverage_confidence": symbol_coverage_confidence(
                language, parser_used, parse_succeeded
            ),
            "unmatched_constructs": unmatched_constructs,
            "suspicious_constructs": suspicious_constructs,
            "lines": line_count(data),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "dependencies": dependencies,
            "symbols": symbols,
            "calls": sorted(calls, key=lambda item: (item["line"], item["column"], item["callee"])),
            "outline": outline,
            "symbol_count": len(symbols),
            "largest_symbol_lines": max((item["span_lines"] for item in symbols), default=0),
            "parse_error": parse_error,
            "declared_owner": ownership.get("declared_owner") if ownership else None,
            "layer": ownership.get("layer") if ownership else None,
            "declared_target": ownership.get("declared_target") if ownership else None,
            "build_targets": actual_targets,
            "ownership_violations": sorted(set(ownership_violations)),
        }
        files.append(file_record)

    language_summary: dict[str, dict[str, int]] = {}
    for language in sorted({item["language"] for item in files}):
        selected = [item for item in files if item["language"] == language]
        language_summary[language] = {
            "files": len(selected),
            "lines": sum(item["lines"] for item in selected),
            "symbols": sum(item["symbol_count"] for item in selected),
        }
    violation_counts = Counter(
        violation for item in files for violation in item["ownership_violations"]
    )
    confidence_counts = Counter(
        item["symbol_coverage_confidence"]["level"] for item in files
    )
    compilation_counts = Counter(item["compilation_status"] for item in files)
    dependency_graph = build_dependency_graph(files)
    return {
        "schema": SCHEMA,
        "generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "root": str(root),
        "discovery": "git tracked plus non-ignored untracked files",
        "excluded_globs": list(excludes),
        "summary": {
            "files": len(files),
            "lines": sum(item["lines"] for item in files),
            "symbols": sum(item["symbol_count"] for item in files),
            "parse_errors": len(parse_errors),
            "ownership_violations": sum(violation_counts.values()),
            "ownership_violation_types": dict(sorted(violation_counts.items())),
            "parser_health": {
                "parse_failures": sum(not item["parse_succeeded"] for item in files),
                "diagnostics": sum(len(item["diagnostics"]) for item in files),
                "unmatched_constructs": sum(
                    len(item["unmatched_constructs"]) for item in files
                ),
                "suspicious_constructs": sum(
                    len(item["suspicious_constructs"]) for item in files
                ),
                "confidence": dict(sorted(confidence_counts.items())),
                "compilation": dict(sorted(compilation_counts.items())),
            },
            "languages": language_summary,
        },
        "parser_warnings": warnings,
        "clang_ast": clang_summary,
        "dependency_graph": dependency_graph,
        "parse_errors": parse_errors,
        "ownership_manifest_schema": ownership_manifest.get("schema") if ownership_manifest else None,
        "files": files,
    }


def markdown_escape(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def viewer_payload(index: dict[str, Any]) -> dict[str, Any]:
    """Return the compact, browser-oriented projection of the canonical index."""
    graph = index["dependency_graph"]
    nodes = []
    for item in index["files"]:
        nodes.append({
            "id": item["path"],
            "path": item["path"],
            "language": item["language"],
            "owner": item.get("declared_owner"),
            "layer": item.get("layer"),
            "targets": item.get("build_targets", []),
            "declared_target": item.get("declared_target"),
            "lines": item["lines"],
            "symbol_count": item["symbol_count"],
            "largest_symbol_lines": item["largest_symbol_lines"],
            "metrics": item["dependency_metrics"],
            "parser": {
                "used": item["parser"],
                "parse_succeeded": item["parse_succeeded"],
                "compilation_status": item["compilation_status"],
                "diagnostics": item["diagnostics"],
                "fallback_reason": item["parser_fallback_reason"],
                "confidence": item["symbol_coverage_confidence"],
                "unmatched": item["unmatched_constructs"],
                "suspicious": item["suspicious_constructs"],
            },
            "symbols": [
                {
                    "name": symbol_item["name"],
                    "qualified_name": symbol_item["qualified_name"],
                    "kind": symbol_item["kind"],
                    "line": symbol_item["line"],
                    "end_line": symbol_item["end_line"],
                    "type": symbol_item.get("type", ""),
                    "signature": symbol_item.get("signature", ""),
                }
                for symbol_item in item["symbols"]
            ],
        })
    return {
        "schema": VIEWER_SCHEMA,
        "generated_at": index["generated_at"],
        "freshness": index["freshness"],
        "summary": index["summary"],
        "clang_ast": index["clang_ast"],
        "graph_summary": graph["summary"],
        "policy": graph["policy"],
        "nodes": nodes,
        "edges": graph["file_edges"],
        "target_edges": graph["target_to_target_edges"],
        "owner_edges": graph["cross_owner_dependencies"],
        "findings": {
            "include_cycles": graph["include_cycles"],
            "layer_direction_violations": graph["layer_direction_violations"],
            "dead_or_unreachable": graph["dead_or_unreachable_implementations"],
            "duplicate_responsibility": graph["duplicate_responsibility_clusters"],
        },
    }


def write_viewer(index: dict[str, Any], output_dir: Path) -> Path:
    viewer_path = output_dir / "code_structure_graph.json"
    viewer_path.write_text(
        json.dumps(viewer_payload(index), separators=(",", ":")) + "\n",
        encoding="utf-8",
    )
    asset_dir = Path(__file__).resolve().with_name("code_structure_viewer")
    for asset_name in VIEWER_ASSET_NAMES:
        source = asset_dir / asset_name
        if not source.is_file():
            raise RuntimeError(f"code structure viewer asset is missing: {source}")
        shutil.copyfile(source, output_dir / asset_name)
    return viewer_path


def viewer_output_errors(index: dict[str, Any], output_dir: Path) -> list[str]:
    errors = []
    graph_path = output_dir / "code_structure_graph.json"
    try:
        graph_payload = json.loads(graph_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        errors.append(f"viewer graph is missing or invalid: {error}")
    else:
        if graph_payload != viewer_payload(index):
            errors.append("viewer graph differs from the canonical artifact")
    asset_dir = Path(__file__).resolve().with_name("code_structure_viewer")
    for asset_name in VIEWER_ASSET_NAMES:
        generated = output_dir / asset_name
        source = asset_dir / asset_name
        try:
            if generated.read_bytes() != source.read_bytes():
                errors.append(f"viewer asset differs from its source: {asset_name}")
        except OSError as error:
            errors.append(f"viewer asset is missing or unreadable: {asset_name}: {error}")
    return errors


def render_markdown(index: dict[str, Any], large_file_lines: int) -> str:
    summary = index["summary"]
    files = index["files"]
    freshness = index["freshness"]
    graph = index["dependency_graph"]
    largest = sorted(files, key=lambda item: (-item["lines"], item["path"]))
    oversized = [item for item in largest if item["lines"] >= large_file_lines]
    lines = [
        "# Generated Code Structure",
        "",
        "> Generated by `scripts/generate_code_structure.py`; do not edit by hand.",
        "> `code_structure.json` is the canonical full symbol index.",
        "> Run `python3 scripts/serve_code_structure.py` for the interactive local viewer.",
        "",
        f"Generated: `{index['generated_at']}`  ",
        f"Corpus: **{summary['files']:,} files**, **{summary['lines']:,} lines**, "
        f"**{summary['symbols']:,} symbols**  ",
        f"Large-file threshold: **{large_file_lines:,} lines**",
        f"Ownership violations: **{summary['ownership_violations']:,}**",
        "",
        "## Artifact provenance",
        "",
        "| Input | Recorded value |",
        "|---|---|",
        f"| Generator | `{freshness['generator_version']}` / `{freshness['generator_sha256']}` |",
        f"| Git commit | `{freshness['git_commit'] or '-'}` |",
        f"| Indexed inputs dirty | `{freshness['git_dirty']}` "
        f"({freshness['git_dirty_entries']} status entries) |",
        f"| Dirty-state fingerprint | `{freshness['dirty_state_fingerprint']}` |",
        f"| Corpus fingerprint | `{freshness['corpus_fingerprint']}` |",
        f"| Ownership manifest | `{freshness['ownership_manifest']['path']}` / "
        f"`{freshness['ownership_manifest']['sha256'] or '-'}` |",
        f"| Build file | `{freshness['build_file']['path']}` / "
        f"`{freshness['build_file']['sha256'] or '-'}` |",
        f"| Compilation database | `{freshness['compile_commands']['path']}` / "
        f"`{freshness['compile_commands']['sha256'] or '-'}` |",
        "",
        "## Parser health",
        "",
        "| Metric | Count |",
        "|---|---:|",
        f"| Parse failures | {summary['parser_health']['parse_failures']:,} |",
        f"| Diagnostics | {summary['parser_health']['diagnostics']:,} |",
        f"| Unmatched constructs | {summary['parser_health']['unmatched_constructs']:,} |",
        f"| Suspicious constructs | {summary['parser_health']['suspicious_constructs']:,} |",
        "",
        "## Dependency graph",
        "",
        "| Metric | Count |",
        "|---|---:|",
        f"| Resolved file edges | {graph['summary']['resolved_file_edges']:,} |",
        f"| Resolved include edges | {graph['summary']['resolved_include_edges']:,} |",
        f"| Resolved symbol-call edges | {graph['summary']['resolved_call_edges']:,} |",
        f"| Raw dependencies resolved | "
        f"{graph['summary']['dependency_resolution'].get('resolved', 0):,} / "
        f"{graph['summary']['raw_dependencies']:,} |",
        f"| Symbol calls resolved | {graph['summary']['symbol_calls_resolved']:,} / "
        f"{graph['summary']['symbol_calls']:,} |",
        f"| Ambiguous dependencies | {graph['summary']['ambiguous_dependencies']:,} |",
        f"| Include cycles | {graph['summary']['include_cycles']:,} |",
        f"| Layer-direction violations | {graph['summary']['layer_direction_violations']:,} |",
        f"| Target-to-target edges | {graph['summary']['target_edges']:,} |",
        f"| Cross-owner file edges | {graph['summary']['cross_owner_file_edges']:,} |",
        f"| Dead or unreachable implementations | "
        f"{graph['summary']['dead_or_unreachable_implementations']:,} |",
        f"| Duplicate responsibility clusters | "
        f"{graph['summary']['duplicate_responsibility_clusters']:,} |",
        "",
        "## Language inventory",
        "",
        "| Language | Files | Lines | Symbols |",
        "|---|---:|---:|---:|",
    ]
    for language, values in sorted(summary["languages"].items(),
                                   key=lambda item: -item[1]["lines"]):
        lines.append(
            f"| {language} | {values['files']:,} | {values['lines']:,} | {values['symbols']:,} |"
        )
    fan_out_files = sorted(
        files,
        key=lambda item: (-item["dependency_metrics"]["fan_out"], item["path"]),
    )[:20]
    lines.extend([
        "",
        "## Dependency hotspots",
        "",
        "| File | Fan-in | Fan-out | Include in/out | Call in/out |",
        "|---|---:|---:|---:|---:|",
    ])
    for item in fan_out_files:
        metrics = item["dependency_metrics"]
        lines.append(
            f"| `{item['path']}` | {metrics['fan_in']:,} | {metrics['fan_out']:,} | "
            f"{metrics['include_fan_in']:,}/{metrics['include_fan_out']:,} | "
            f"{metrics['call_fan_in']:,}/{metrics['call_fan_out']:,} |"
        )
    lines.extend([
        "",
        "## Architecture graph findings",
        "",
        "### Include cycles",
        "",
    ])
    if graph["include_cycles"]:
        for cycle in graph["include_cycles"]:
            lines.append(f"- {cycle['edge_count']} edges: " + ", ".join(f"`{path}`" for path in cycle["files"]))
    else:
        lines.append("- None")
    lines.extend(["", "### Layer-direction violations", ""])
    if graph["layer_direction_violations"]:
        lines.extend(["| Source | Layer | Target | Layer | Kinds |", "|---|---|---|---|---|"])
        for item in graph["layer_direction_violations"][:100]:
            lines.append(
                f"| `{item['source']}` | {item['source_layer']} | `{item['target']}` | "
                f"{item['target_layer']} | {', '.join(item['kinds'])} |"
            )
    else:
        lines.append("None.")
    lines.extend(["", "### Dead or unreachable implementations", ""])
    if graph["dead_or_unreachable_implementations"]:
        lines.extend(["| File | Status | Confidence | Reason |", "|---|---|---|---|"])
        for item in graph["dead_or_unreachable_implementations"][:100]:
            lines.append(
                f"| `{item['path']}` | {item['status']} | {item['confidence']} | "
                f"{item['reason']} |"
            )
    else:
        lines.append("None.")
    lines.extend(["", "### Duplicate responsibility clusters", ""])
    if graph["duplicate_responsibility_clusters"]:
        for cluster in graph["duplicate_responsibility_clusters"][:50]:
            lines.append(
                f"- similarity {cluster['max_similarity']:.3f}: "
                + ", ".join(f"`{path}`" for path in cluster["files"])
            )
    else:
        lines.append("- None")
    lines.extend([
        "",
        "## Ownership audit",
        "",
        "| File | Declared owner | Layer | Build targets | Violations |",
        "|---|---|---|---|---|",
    ])
    ownership_rows = [item for item in files if item["ownership_violations"]]
    for item in sorted(ownership_rows, key=lambda entry: entry["path"]):
        lines.append(
            f"| `{item['path']}` | {item['declared_owner'] or '-'} | "
            f"{item['layer'] or '-'} | `{', '.join(item['build_targets'])}` | "
            f"{', '.join(item['ownership_violations'])} |"
        )
    if not ownership_rows:
        lines.append("| _None_ | - | - | - | - |")
    lines.extend([
        "",
        "## Refactor queue",
        "",
        "Ranked by physical size. `Largest definition` identifies monolithic functions/classes "
        "that a file-level count can hide.",
        "",
        "| File | Language | Lines | Symbols | Dependencies | Largest definition |",
        "|---|---|---:|---:|---:|---:|",
    ])
    for item in largest[:50]:
        lines.append(
            f"| `{item['path']}` | {item['language']} | {item['lines']:,} | "
            f"{item['symbol_count']:,} | {len(item['dependencies']):,} | "
            f"{item['largest_symbol_lines']:,} |"
        )
    lines.extend(["", f"## Large-file outlines ({len(oversized)} files)", ""])
    for item in oversized:
        lines.extend([
            f"### `{item['path']}`",
            "",
            f"{item['lines']:,} lines; {item['symbol_count']:,} symbols; "
            f"{len(item['dependencies']):,} dependencies; parser `{item['parser']}`.",
            "",
        ])
        structural = [entry for entry in item["symbols"]
                      if entry["kind"] in STRUCTURAL_KINDS]
        biggest = sorted(structural, key=lambda entry: (-entry["span_lines"], entry["line"]))[:20]
        if biggest:
            lines.extend([
                "| Largest definitions / candidate seams | Kind | Lines | Span |",
                "|---|---|---:|---:|",
            ])
            for entry in biggest:
                name = markdown_escape(entry["qualified_name"] + entry["signature"])
                lines.append(
                    f"| `{name}` | {entry['kind']} | {entry['line']}-{entry['end_line']} | "
                    f"{entry['span_lines']:,} |"
                )
            lines.append("")
        scopes: dict[str, list[dict[str, Any]]] = defaultdict(list)
        for entry in structural:
            scopes[entry["scope"] or "<file scope>"].append(entry)
        scope_rows = []
        for scope, entries in scopes.items():
            total_span = sum(entry["span_lines"] for entry in entries
                             if entry["kind"] in {"function", "method"})
            scope_rows.append((total_span, scope, entries))
        if scope_rows:
            lines.extend([
                "| Scope / responsibility cluster | Definitions | Function span |",
                "|---|---:|---:|",
            ])
            for total_span, scope, entries in sorted(scope_rows, reverse=True)[:15]:
                lines.append(
                    f"| `{markdown_escape(scope)}` | {len(entries):,} | {total_span:,} |"
                )
            lines.append("")
    lines.extend([
        "## Complete file inventory",
        "",
        "Every file below has a corresponding full `symbols` array in `code_structure.json`.",
        "",
        "| File | Language | Lines | Symbols | Parser | Compile | Confidence | Fan-in | Fan-out | Diagnostics | Findings |",
        "|---|---|---:|---:|---|---|---|---:|---:|---:|---:|",
    ])
    for item in sorted(files, key=lambda entry: entry["path"]):
        lines.append(
            f"| `{item['path']}` | {item['language']} | {item['lines']:,} | "
            f"{item['symbol_count']:,} | `{item['parser']}` | `{item['compilation_status']}` | "
            f"`{item['symbol_coverage_confidence']['level']}` | "
            f"{item['dependency_metrics']['fan_in']:,} | {item['dependency_metrics']['fan_out']:,} | "
            f"{len(item['diagnostics']):,} | "
            f"{len(item['unmatched_constructs']) + len(item['suspicious_constructs']):,} |"
        )
    lines.extend([
        "",
        "## Interpretation limits",
        "",
        "- C/C++ definitions, scopes, calls, types, and extents come from Clang's AST using the "
        "compilation database. Files not covered by a successful translation unit are explicitly "
        "labeled `universal-ctags-fallback`.",
        "- Python definitions and nesting come from Python's AST. Shader function extents use a "
        "brace-aware outline parser. CSS is inventoried without selector-level symbols.",
        "- Repository dependency edges resolve static includes/imports and Clang calls. Runtime "
        "reflection, plugin loading, generated registrations, and string-based dispatch remain outside "
        "the graph; unreachable files and duplicate responsibility clusters are review candidates, "
        "not automatic deletion findings.",
        "- Generated, build, dependency, artifact, and vendored directories are excluded by default. "
        "Use `--include` to remove the default exclusions or `--exclude` to add project-specific ones.",
        "",
    ])
    return "\n".join(lines)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output-dir", type=Path, default=Path("build/code-structure"))
    parser.add_argument("--large-file-lines", type=int, default=1500)
    parser.add_argument("--exclude", action="append", default=[], metavar="GLOB",
                        help="add an exclusion glob (repeatable)")
    parser.add_argument("--include", action="store_true",
                        help="disable the default exclusion globs")
    parser.add_argument("--ownership-manifest", type=Path, default=Path("code_ownership.json"))
    parser.add_argument("--ninja-file", type=Path, default=Path("build/build.ninja"))
    parser.add_argument("--compile-commands", type=Path,
                        default=Path("build/compile_commands.json"))
    parser.add_argument("--clang-jobs", type=int, default=4,
                        help="parallel libclang translation units (default: 4)")
    parser.add_argument("--check-ownership", action="store_true",
                        help="fail when ownership violations are present")
    parser.add_argument("--check-current", action="store_true",
                        help="verify the existing JSON artifact without rewriting it")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    root = args.root.resolve()
    output_dir = args.output_dir
    if not output_dir.is_absolute():
        output_dir = root / output_dir
    output_dir = output_dir.resolve()
    try:
        output_relative = output_dir.relative_to(root).as_posix()
        output_excludes = (output_relative, f"{output_relative}/**")
    except ValueError:
        output_excludes = ()
    excludes = (() if args.include else DEFAULT_EXCLUDES) + tuple(args.exclude) + output_excludes
    paths = discover_files(root, excludes)
    if not paths and not args.check_current:
        print("No source files found", file=sys.stderr)
        return 1
    manifest_path = args.ownership_manifest
    if not manifest_path.is_absolute():
        manifest_path = root / manifest_path
    ninja_path = args.ninja_file
    if not ninja_path.is_absolute():
        ninja_path = root / ninja_path
    compile_commands_path = args.compile_commands
    if not compile_commands_path.is_absolute():
        compile_commands_path = root / compile_commands_path
    json_path = output_dir / "code_structure.json"
    markdown_path = output_dir / "code_structure.md"
    if args.check_current:
        if not json_path.is_file():
            print(f"Freshness check failed: artifact not found: {json_path}", file=sys.stderr)
            return 4
        try:
            existing_index = json.loads(json_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"Freshness check failed: cannot read {json_path}: {error}", file=sys.stderr)
            return 4
        freshness_errors = validate_current(
            root,
            existing_index,
            paths,
            manifest_path,
            ninja_path,
            compile_commands_path,
        )
        freshness_errors.extend(viewer_output_errors(existing_index, output_dir))
        if freshness_errors:
            print(f"Freshness check failed: {len(freshness_errors)} difference(s)", file=sys.stderr)
            for error in freshness_errors:
                print(f"- {error}", file=sys.stderr)
            return 4
        print(f"Artifact is current: {json_path}")
        return 0

    ownership_manifest = load_ownership_manifest(manifest_path)
    build_targets = build_targets_from_ninja(root, ninja_path)
    index = analyze(
        root,
        paths,
        excludes,
        ownership_manifest,
        build_targets,
        compile_commands_path,
        args.clang_jobs,
    )
    index["freshness"] = freshness_metadata(
        root,
        index["files"],
        manifest_path,
        ninja_path,
        compile_commands_path,
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    json_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    markdown_path.write_text(render_markdown(index, args.large_file_lines), encoding="utf-8")
    viewer_path = write_viewer(index, output_dir)
    print(f"Wrote {json_path}")
    print(f"Wrote {markdown_path}")
    print(f"Wrote {viewer_path}")
    print(
        f"Indexed {index['summary']['files']} files, {index['summary']['lines']} lines, "
        f"and {index['summary']['symbols']} symbols"
    )
    if index["summary"]["parse_errors"]:
        print(f"Parse errors: {index['summary']['parse_errors']}", file=sys.stderr)
        return 2
    if args.check_ownership and index["summary"]["ownership_violations"]:
        print(
            f"Ownership violations: {index['summary']['ownership_violations']}",
            file=sys.stderr,
        )
        return 3
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
