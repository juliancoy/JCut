#!/usr/bin/env python3
"""Resolve code-structure records into repository dependency graphs."""

from __future__ import annotations

import re
from collections import Counter, defaultdict, deque
from pathlib import PurePosixPath
from typing import Any, Iterable


IMPLEMENTATION_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".cu"}
GENERIC_RESPONSIBILITY_WORDS = {
    "add", "apply", "build", "clear", "create", "ensure", "find", "get", "handle",
    "has", "is", "load", "make", "operator", "process", "read", "remove", "render",
    "reset", "run", "save", "set", "to", "update", "write",
}


def dependency_kind(language: str) -> str:
    if language in {"C", "C++", "CUDA", "GLSL"}:
        return "include"
    if language == "Python":
        return "import"
    if language == "Shell":
        return "source"
    if language == "CMake":
        return "build-include"
    return "dependency"


def normalized_candidates(source: str, raw: str, language: str) -> list[str]:
    source_parent = PurePosixPath(source).parent
    cleaned = raw.replace("\\", "/").strip().strip('"\'')
    candidates: list[str] = []

    def add(value: PurePosixPath | str) -> None:
        normalized = str(value).removeprefix("./")
        if normalized and normalized not in candidates:
            candidates.append(normalized)

    if language == "Python":
        leading = len(cleaned) - len(cleaned.lstrip("."))
        module = cleaned.lstrip(".").replace(".", "/")
        parent = source_parent
        for _ in range(max(0, leading - 1)):
            parent = parent.parent
        base = parent / module if leading else PurePosixPath(module)
        add(str(base) + ".py")
        add(base / "__init__.py")
    else:
        add(source_parent / cleaned)
        add(cleaned)
        if language == "CMake":
            add(PurePosixPath(cleaned) / "CMakeLists.txt")
            add(source_parent / cleaned / "CMakeLists.txt")
    return candidates


def resolve_dependency(
    source: str,
    raw: str,
    language: str,
    path_set: set[str],
    suffix_index: dict[str, list[str]],
) -> dict[str, Any]:
    candidates = normalized_candidates(source, raw, language)
    for candidate in candidates:
        if candidate in path_set:
            return {
                "raw": raw,
                "kind": dependency_kind(language),
                "status": "resolved",
                "target": candidate,
            }
    cleaned = raw.replace("\\", "/").lstrip("./")
    suffix_matches = suffix_index.get(cleaned, [])
    if len(suffix_matches) == 1:
        return {
            "raw": raw,
            "kind": dependency_kind(language),
            "status": "resolved",
            "target": suffix_matches[0],
        }
    if len(suffix_matches) > 1:
        return {
            "raw": raw,
            "kind": dependency_kind(language),
            "status": "ambiguous",
            "target": None,
            "candidates": sorted(suffix_matches),
        }
    return {"raw": raw, "kind": dependency_kind(language), "status": "external", "target": None}


def symbol_resolution_index(files: list[dict[str, Any]]) -> tuple[dict[str, dict[str, Any]], dict[str, dict[str, Any]]]:
    by_usr_candidates: dict[str, list[dict[str, Any]]] = defaultdict(list)
    by_name_candidates: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for file_record in files:
        for symbol in file_record.get("symbols", []):
            candidate = {"file": file_record["path"], "symbol": symbol}
            usr = symbol.get("usr")
            if usr:
                by_usr_candidates[usr].append(candidate)
            qualified = symbol.get("qualified_name")
            if qualified:
                by_name_candidates[qualified].append(candidate)

    def select(candidates: Iterable[dict[str, Any]]) -> dict[str, Any] | None:
        ordered = sorted(
            candidates,
            key=lambda item: (
                not bool(item["symbol"].get("is_definition")),
                item["file"].endswith((".h", ".hh", ".hpp", ".hxx")),
                item["file"],
                item["symbol"].get("line", 0),
            ),
        )
        return ordered[0] if ordered else None

    return (
        {key: selected for key, values in by_usr_candidates.items() if (selected := select(values))},
        {key: selected for key, values in by_name_candidates.items() if (selected := select(values))},
    )


def layer_numbers(layer: str | None) -> set[int]:
    return {int(value) for value in re.findall(r"L(\d+)", layer or "")}


def strongly_connected_components(nodes: Iterable[str], adjacency: dict[str, set[str]]) -> list[list[str]]:
    index = 0
    indices: dict[str, int] = {}
    lowlinks: dict[str, int] = {}
    stack: list[str] = []
    on_stack: set[str] = set()
    components: list[list[str]] = []

    def visit(node: str) -> None:
        nonlocal index
        indices[node] = index
        lowlinks[node] = index
        index += 1
        stack.append(node)
        on_stack.add(node)
        for target in adjacency.get(node, set()):
            if target not in indices:
                visit(target)
                lowlinks[node] = min(lowlinks[node], lowlinks[target])
            elif target in on_stack:
                lowlinks[node] = min(lowlinks[node], indices[target])
        if lowlinks[node] == indices[node]:
            component = []
            while stack:
                member = stack.pop()
                on_stack.remove(member)
                component.append(member)
                if member == node:
                    break
            if len(component) > 1:
                components.append(sorted(component))

    for node in sorted(nodes):
        if node not in indices:
            visit(node)
    return sorted(components, key=lambda component: (-len(component), component))


def production_targets(file_record: dict[str, Any]) -> set[str]:
    targets = {
        target for target in file_record.get("build_targets", [])
        if not target.startswith("test_")
    }
    declared = file_record.get("declared_target")
    if declared:
        targets.add(declared)
    return targets


def responsibility_tokens(name: str) -> set[str]:
    expanded = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", name)
    return {
        token.lower() for token in re.split(r"[^A-Za-z0-9]+", expanded)
        if len(token) >= 3 and token.lower() not in GENERIC_RESPONSIBILITY_WORDS
    }


def duplicate_responsibility_clusters(files: list[dict[str, Any]]) -> list[dict[str, Any]]:
    features: dict[str, set[str]] = {}
    for file_record in files:
        path = file_record["path"]
        if PurePosixPath(path).suffix not in IMPLEMENTATION_SUFFIXES:
            continue
        if path.startswith(("tests/", "legacy/")) or file_record.get("declared_owner") in {"tests", "legacy-and-experiments"}:
            continue
        tokens: set[str] = set()
        definition_names = set()
        for symbol in file_record.get("symbols", []):
            if symbol.get("kind") not in {"function", "method"}:
                continue
            if symbol.get("is_definition") is False:
                continue
            definition_names.add(symbol.get("name", ""))
            tokens.update(responsibility_tokens(symbol.get("name", "")))
        tokens.update(f"fn:{name}" for name in definition_names if name)
        if len(tokens) >= 6:
            features[path] = tokens

    parent = {path: path for path in features}

    def find(path: str) -> str:
        while parent[path] != path:
            parent[path] = parent[parent[path]]
            path = parent[path]
        return path

    def union(left: str, right: str) -> None:
        left_root = find(left)
        right_root = find(right)
        if left_root != right_root:
            parent[right_root] = left_root

    similarities = []
    paths = sorted(features)
    for index, left in enumerate(paths):
        for right in paths[index + 1:]:
            shared = features[left] & features[right]
            if len(shared) < 4:
                continue
            score = len(shared) / len(features[left] | features[right])
            if score < 0.60:
                continue
            union(left, right)
            similarities.append({
                "left": left,
                "right": right,
                "similarity": round(score, 3),
                "shared_features": sorted(shared)[:20],
            })

    groups: dict[str, list[str]] = defaultdict(list)
    for path in paths:
        groups[find(path)].append(path)
    clusters = []
    for members in groups.values():
        if len(members) < 2:
            continue
        member_set = set(members)
        evidence = [
            item for item in similarities
            if item["left"] in member_set and item["right"] in member_set
        ]
        clusters.append({
            "files": sorted(members),
            "max_similarity": max(item["similarity"] for item in evidence),
            "evidence": sorted(evidence, key=lambda item: (-item["similarity"], item["left"], item["right"])),
            "confidence": "candidate",
        })
    return sorted(clusters, key=lambda item: (-item["max_similarity"], item["files"]))


def build_dependency_graph(files: list[dict[str, Any]]) -> dict[str, Any]:
    by_path = {item["path"]: item for item in files}
    path_set = set(by_path)
    suffix_index: dict[str, list[str]] = defaultdict(list)
    for path in path_set:
        parts = PurePosixPath(path).parts
        for start in range(len(parts)):
            suffix_index["/".join(parts[start:])].append(path)

    symbol_by_usr, symbol_by_name = symbol_resolution_index(files)
    edge_data: dict[tuple[str, str], dict[str, Any]] = {}
    dependency_status_counts: Counter[str] = Counter()
    symbol_call_total = 0
    symbol_call_resolved = 0
    symbol_call_resolution: Counter[str] = Counter()

    def add_edge(source: str, target: str, kind: str, count: int = 1, example: str | None = None) -> None:
        if source == target:
            return
        key = (source, target)
        entry = edge_data.setdefault(key, {
            "source": source,
            "target": target,
            "kinds": set(),
            "include_count": 0,
            "dependency_count": 0,
            "call_count": 0,
            "symbol_examples": [],
        })
        entry["kinds"].add(kind)
        entry[f"{kind}_count"] += count
        if example and example not in entry["symbol_examples"] and len(entry["symbol_examples"]) < 5:
            entry["symbol_examples"].append(example)

    ambiguous_dependencies = []
    for file_record in files:
        path = file_record["path"]
        resolved_dependencies = []
        for raw in file_record.get("dependencies", []):
            resolved = resolve_dependency(
                path,
                raw,
                file_record["language"],
                path_set,
                suffix_index,
            )
            resolved_dependencies.append(resolved)
            dependency_status_counts[resolved["status"]] += 1
            if resolved["status"] == "resolved":
                edge_kind = "include" if resolved["kind"] == "include" else "dependency"
                add_edge(path, resolved["target"], edge_kind)
            elif resolved["status"] == "ambiguous":
                ambiguous_dependencies.append({"source": path, **resolved})
        file_record["resolved_dependencies"] = resolved_dependencies

        for call in file_record.get("calls", []):
            symbol_call_total += 1
            candidate = symbol_by_usr.get(call.get("callee_usr", ""))
            if candidate is None and call.get("callee"):
                candidate = symbol_by_name.get(call["callee"])
            if candidate is None:
                call["resolution"] = "external" if call.get("callee_usr") else "unresolved"
                symbol_call_resolution[call["resolution"]] += 1
                call["resolved_target"] = None
                continue
            symbol = candidate["symbol"]
            call["resolution"] = "resolved"
            symbol_call_resolved += 1
            symbol_call_resolution["resolved"] += 1
            call["resolved_target"] = {
                "file": candidate["file"],
                "symbol": symbol.get("qualified_name") or symbol.get("name"),
                "usr": symbol.get("usr", ""),
                "line": symbol.get("line"),
            }
            add_edge(path, candidate["file"], "call", example=call.get("callee"))

    file_edges = []
    incoming: Counter[str] = Counter()
    outgoing: Counter[str] = Counter()
    include_incoming: Counter[str] = Counter()
    include_outgoing: Counter[str] = Counter()
    call_incoming: Counter[str] = Counter()
    call_outgoing: Counter[str] = Counter()
    for entry in edge_data.values():
        rendered = {
            **entry,
            "kinds": sorted(entry["kinds"]),
        }
        file_edges.append(rendered)
        source = entry["source"]
        target = entry["target"]
        outgoing[source] += 1
        incoming[target] += 1
        if entry["include_count"]:
            include_outgoing[source] += 1
            include_incoming[target] += 1
        if entry["call_count"]:
            call_outgoing[source] += 1
            call_incoming[target] += 1
    file_edges.sort(key=lambda item: (item["source"], item["target"]))

    for file_record in files:
        path = file_record["path"]
        file_record["dependency_metrics"] = {
            "fan_in": incoming[path],
            "fan_out": outgoing[path],
            "include_fan_in": include_incoming[path],
            "include_fan_out": include_outgoing[path],
            "call_fan_in": call_incoming[path],
            "call_fan_out": call_outgoing[path],
        }

    include_adjacency: dict[str, set[str]] = defaultdict(set)
    full_adjacency: dict[str, set[str]] = defaultdict(set)
    for edge in file_edges:
        full_adjacency[edge["source"]].add(edge["target"])
        if edge["include_count"]:
            include_adjacency[edge["source"]].add(edge["target"])
    include_cycles = []
    for component in strongly_connected_components(path_set, include_adjacency):
        members = set(component)
        include_cycles.append({
            "files": component,
            "edge_count": sum(
                target in members
                for source in members
                for target in include_adjacency.get(source, set())
            ),
        })

    layer_violations = []
    for edge in file_edges:
        source_record = by_path[edge["source"]]
        target_record = by_path[edge["target"]]
        source_layers = layer_numbers(source_record.get("layer"))
        target_layers = layer_numbers(target_record.get("layer"))
        if not source_layers or not target_layers or 5 in source_layers:
            continue
        if min(source_layers) > max(target_layers):
            layer_violations.append({
                "source": edge["source"],
                "source_layer": source_record.get("layer"),
                "target": edge["target"],
                "target_layer": target_record.get("layer"),
                "kinds": edge["kinds"],
            })

    owner_edges: dict[tuple[str, str], dict[str, Any]] = {}
    for edge in file_edges:
        source_owner = by_path[edge["source"]].get("declared_owner") or "<unowned>"
        target_owner = by_path[edge["target"]].get("declared_owner") or "<unowned>"
        if source_owner == target_owner:
            continue
        key = (source_owner, target_owner)
        item = owner_edges.setdefault(key, {
            "source_owner": source_owner,
            "target_owner": target_owner,
            "file_edges": 0,
            "include_edges": 0,
            "dependency_edges": 0,
            "call_edges": 0,
            "examples": [],
        })
        item["file_edges"] += 1
        item["include_edges"] += bool(edge["include_count"])
        item["dependency_edges"] += bool(edge["dependency_count"])
        item["call_edges"] += bool(edge["call_count"])
        if len(item["examples"]) < 5:
            item["examples"].append({"source": edge["source"], "target": edge["target"]})
    cross_owner_dependencies = sorted(
        owner_edges.values(),
        key=lambda item: (-item["file_edges"], item["source_owner"], item["target_owner"]),
    )

    target_edges: dict[tuple[str, str], dict[str, Any]] = {}
    for edge in file_edges:
        for source_target in production_targets(by_path[edge["source"]]):
            for target_target in production_targets(by_path[edge["target"]]):
                if source_target == target_target:
                    continue
                key = (source_target, target_target)
                item = target_edges.setdefault(key, {
                    "source_target": source_target,
                    "target_target": target_target,
                    "file_edges": 0,
                    "include_edges": 0,
                    "dependency_edges": 0,
                    "call_edges": 0,
                    "examples": [],
                })
                item["file_edges"] += 1
                item["include_edges"] += bool(edge["include_count"])
                item["dependency_edges"] += bool(edge["dependency_count"])
                item["call_edges"] += bool(edge["call_count"])
                if len(item["examples"]) < 5:
                    item["examples"].append({"source": edge["source"], "target": edge["target"]})
    target_to_target_edges = sorted(
        target_edges.values(),
        key=lambda item: (-item["file_edges"], item["source_target"], item["target_target"]),
    )

    roots = set()
    for file_record in files:
        path = file_record["path"]
        basename = PurePosixPath(path).name.lower()
        layers = layer_numbers(file_record.get("layer"))
        if 0 in layers or basename.endswith(("main.cpp", "entry.cpp", "_cli.cpp")) or path.startswith("tests/"):
            roots.add(path)
    reachable = set(roots)
    queue = deque(sorted(roots))
    while queue:
        source = queue.popleft()
        for target in full_adjacency.get(source, set()):
            if target not in reachable:
                reachable.add(target)
                queue.append(target)

    unreachable_implementations = []
    for file_record in files:
        path = file_record["path"]
        if PurePosixPath(path).suffix not in IMPLEMENTATION_SUFFIXES:
            continue
        if path.startswith(("tests/", "legacy/")) or file_record.get("declared_owner") in {"tests", "legacy-and-experiments"}:
            continue
        targets = sorted(production_targets(file_record))
        if not targets and incoming[path] == 0:
            unreachable_implementations.append({
                "path": path,
                "status": "dead",
                "confidence": "high",
                "reason": "no production target and no incoming repository dependency",
                "build_targets": targets,
            })
        elif targets and path not in reachable:
            unreachable_implementations.append({
                "path": path,
                "status": "unreachable",
                "confidence": "candidate",
                "reason": "not reachable from an entry, CLI, or test root through resolved file edges",
                "build_targets": targets,
            })

    duplicate_clusters = duplicate_responsibility_clusters(files)
    return {
        "schema": "jcut_dependency_graph_v1",
        "policy": {
            "layer_direction": "L0 may depend toward higher numbered layers; L5 sources are exempt; a violation requires every source layer to be deeper than every target layer",
            "reachability_roots": "L0 files, main/entry/CLI files, and test files",
            "duplicate_responsibility": "implementation files with at least six normalized definition features, four shared features, and Jaccard similarity >= 0.60",
        },
        "edge_storage": {
            "resolved_static_dependencies": "files[].resolved_dependencies[]",
            "resolved_symbol_calls": "files[].calls[].resolved_target",
            "file_aggregates": "dependency_graph.file_edges[]",
            "per_file_fan_metrics": "files[].dependency_metrics",
        },
        "summary": {
            "resolved_file_edges": len(file_edges),
            "resolved_include_edges": sum(bool(edge["include_count"]) for edge in file_edges),
            "resolved_import_or_build_edges": sum(
                bool(edge["dependency_count"]) for edge in file_edges
            ),
            "resolved_call_edges": sum(bool(edge["call_count"]) for edge in file_edges),
            "raw_dependencies": sum(dependency_status_counts.values()),
            "dependency_resolution": dict(sorted(dependency_status_counts.items())),
            "symbol_calls": symbol_call_total,
            "symbol_calls_resolved": symbol_call_resolved,
            "symbol_call_resolution": dict(sorted(symbol_call_resolution.items())),
            "ambiguous_dependencies": len(ambiguous_dependencies),
            "include_cycles": len(include_cycles),
            "layer_direction_violations": len(layer_violations),
            "cross_owner_owner_pairs": len(cross_owner_dependencies),
            "cross_owner_file_edges": sum(
                item["file_edges"] for item in cross_owner_dependencies
            ),
            "target_edges": len(target_to_target_edges),
            "dead_or_unreachable_implementations": len(unreachable_implementations),
            "duplicate_responsibility_clusters": len(duplicate_clusters),
        },
        "file_edges": file_edges,
        "ambiguous_dependencies": ambiguous_dependencies,
        "include_cycles": include_cycles,
        "layer_direction_violations": sorted(
            layer_violations,
            key=lambda item: (item["source"], item["target"]),
        ),
        "target_to_target_edges": target_to_target_edges,
        "cross_owner_dependencies": cross_owner_dependencies,
        "dead_or_unreachable_implementations": sorted(
            unreachable_implementations,
            key=lambda item: (item["status"], item["path"]),
        ),
        "duplicate_responsibility_clusters": duplicate_clusters,
    }
