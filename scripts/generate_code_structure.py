#!/usr/bin/env python3
"""Generate a procedural, repository-wide source structure index.

The JSON artifact is the canonical output.  It contains every discovered
first-party source file, its dependencies, and its structural symbols.  The
Markdown artifact is a refactor-oriented view of the same data.
"""

from __future__ import annotations

import argparse
import ast
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


SCHEMA = "jcut_code_structure_v1"
OWNERSHIP_SCHEMA = "jcut_code_ownership_v1"
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
                    dependencies.append(dots + (node.module or ""))

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
CPP_TYPE_RE = re.compile(
    r"(?m)^[ \t]*(class|struct|union|enum(?:[ \t]+class)?)[ \t]+"
    r"([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)[^\n;{}]*\{"
)
CPP_FUNCTION_RE = re.compile(
    r"(?m)^[ \t]*(?:(?:template[ \t]*<[^;{}]+>[ \t]*)?)"
    r"(?:[A-Za-z_~][\w:<>,*& \t]*[ \t]+)?([~A-Za-z_]\w*)[ \t]*"
    r"\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?"
    r"(?:(?:override|final)\s*)?(?:->\s*[^;{]+\s*)?\{"
)
CPP_CONTROL_WORDS = {"if", "for", "while", "switch", "catch", "requires"}


def closing_brace_line(text: str, opening_offset: int) -> int:
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
    return line_count(text.encode("utf-8"))


def shader_structure(text: str) -> list[dict[str, Any]]:
    result = []
    for match in SHADER_FUNCTION_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        opening = text.find("{", match.start(), match.end())
        result.append(symbol(
            name=match.group(1),
            kind="function",
            line=line,
            end_line=closing_brace_line(text, opening),
            signature=f"({match.group(2).strip()})",
            parser="shader-outline",
        ))
    return result


def cpp_outline_structure(text: str) -> list[dict[str, Any]]:
    """Fill structural gaps left by Ctags, notably Qt slots and huge inline classes."""
    result: list[dict[str, Any]] = []
    types: list[tuple[int, int, str, str]] = []
    for match in CPP_TYPE_RE.finditer(text):
        opening = text.find("{", match.start(), match.end())
        line = text.count("\n", 0, match.start()) + 1
        end_line = closing_brace_line(text, opening)
        kind = match.group(1).replace(" class", "")
        name = match.group(2)
        types.append((line, end_line, name, kind))
        result.append(symbol(
            name=name,
            kind=kind,
            line=line,
            end_line=end_line,
            parser="cpp-outline",
        ))
    for match in CPP_FUNCTION_RE.finditer(text):
        name = match.group(1)
        if name in CPP_CONTROL_WORDS:
            continue
        opening = text.find("{", match.start(), match.end())
        line = text.count("\n", 0, match.start()) + 1
        end_line = closing_brace_line(text, opening)
        containing = [item for item in types if item[0] < line <= item[1]]
        owner = min(containing, key=lambda item: item[1] - item[0]) if containing else None
        result.append(symbol(
            name=name,
            kind="method" if owner else "function",
            line=line,
            end_line=end_line,
            scope=owner[2] if owner else "",
            signature=f"({match.group(2).strip()})",
            parser="cpp-outline",
        ))
    return result


def merge_symbols(*groups: list[dict[str, Any]]) -> list[dict[str, Any]]:
    merged: dict[tuple[str, int, str], dict[str, Any]] = {}
    for group in groups:
        for entry in group:
            key = (entry["name"], entry["line"], entry["kind"])
            previous = merged.get(key)
            if previous is None or entry["span_lines"] > previous["span_lines"]:
                merged[key] = entry
    return list(merged.values())


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


def analyze(
    root: Path,
    paths: list[Path],
    excludes: tuple[str, ...],
    ownership_manifest: dict[str, Any] | None = None,
    build_targets: dict[str, list[str]] | None = None,
) -> dict[str, Any]:
    ctags_paths = [path for path in paths
                   if language_for(path) not in {"Python", "GLSL", "CSS"}]
    ctags_symbols, warnings = run_ctags(root, ctags_paths) if ctags_paths else ({}, [])
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
        if language == "Python":
            symbols, dependencies, parse_error = python_structure(text, relative.as_posix())
        elif language == "GLSL":
            symbols = shader_structure(text)
        elif language in {"C", "C++", "CUDA"}:
            symbols = merge_symbols(
                ctags_symbols.get(relative.as_posix(), []),
                cpp_outline_structure(text),
            )
        elif language == "CSS":
            symbols = []
        else:
            symbols = ctags_symbols.get(relative.as_posix(), [])
        symbols.sort(key=lambda item: (item["line"], -item["span_lines"], item["name"]))
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
        file_record = {
            "path": relative.as_posix(),
            "language": language,
            "parser": ("+".join(sorted({entry["parser"] for entry in symbols})) if symbols else
                       "python-ast" if language == "Python" else
                       "shader-outline" if language == "GLSL" else
                       "universal-ctags" if language != "CSS" else "inventory-only"),
            "lines": line_count(data),
            "bytes": len(data),
            "sha256": hashlib.sha256(data).hexdigest(),
            "dependencies": dependencies,
            "symbols": symbols,
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
            "languages": language_summary,
        },
        "parser_warnings": warnings,
        "parse_errors": parse_errors,
        "ownership_manifest_schema": ownership_manifest.get("schema") if ownership_manifest else None,
        "files": files,
    }


def markdown_escape(text: str) -> str:
    return text.replace("|", "\\|").replace("\n", " ")


def render_markdown(index: dict[str, Any], large_file_lines: int) -> str:
    summary = index["summary"]
    files = index["files"]
    largest = sorted(files, key=lambda item: (-item["lines"], item["path"]))
    oversized = [item for item in largest if item["lines"] >= large_file_lines]
    lines = [
        "# Generated Code Structure",
        "",
        "> Generated by `scripts/generate_code_structure.py`; do not edit by hand.",
        "> `code_structure.json` is the canonical full symbol index.",
        "",
        f"Generated: `{index['generated_at']}`  ",
        f"Corpus: **{summary['files']:,} files**, **{summary['lines']:,} lines**, "
        f"**{summary['symbols']:,} symbols**  ",
        f"Large-file threshold: **{large_file_lines:,} lines**",
        f"Ownership violations: **{summary['ownership_violations']:,}**",
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
        "| File | Language | Lines | Symbols | Parser |",
        "|---|---|---:|---:|---|",
    ])
    for item in sorted(files, key=lambda entry: entry["path"]):
        lines.append(
            f"| `{item['path']}` | {item['language']} | {item['lines']:,} | "
            f"{item['symbol_count']:,} | `{item['parser']}` |"
        )
    lines.extend([
        "",
        "## Interpretation limits",
        "",
        "- This is a structural syntax index, not a type-resolved call graph. C/C++ symbols come "
        "from Universal Ctags and therefore do not require a successful compile or compile database.",
        "- Python definitions and nesting come from Python's AST. Shader function extents use a "
        "brace-aware outline parser. CSS is inventoried without selector-level symbols.",
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
    parser.add_argument("--check-ownership", action="store_true",
                        help="fail when ownership violations are present")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    root = args.root.resolve()
    excludes = (() if args.include else DEFAULT_EXCLUDES) + tuple(args.exclude)
    paths = discover_files(root, excludes)
    if not paths:
        print("No source files found", file=sys.stderr)
        return 1
    manifest_path = args.ownership_manifest
    if not manifest_path.is_absolute():
        manifest_path = root / manifest_path
    ninja_path = args.ninja_file
    if not ninja_path.is_absolute():
        ninja_path = root / ninja_path
    ownership_manifest = load_ownership_manifest(manifest_path)
    build_targets = build_targets_from_ninja(root, ninja_path)
    index = analyze(root, paths, excludes, ownership_manifest, build_targets)
    output_dir = args.output_dir
    if not output_dir.is_absolute():
        output_dir = root / output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    json_path = output_dir / "code_structure.json"
    markdown_path = output_dir / "code_structure.md"
    json_path.write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
    markdown_path.write_text(render_markdown(index, args.large_file_lines), encoding="utf-8")
    print(f"Wrote {json_path}")
    print(f"Wrote {markdown_path}")
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
