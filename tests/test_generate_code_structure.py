#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "generate_code_structure.py"
SPEC = importlib.util.spec_from_file_location("generate_code_structure", SCRIPT)
assert SPEC and SPEC.loader
generator = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(generator)


class GenerateCodeStructureTest(unittest.TestCase):
    def create_generated_fixture(self, root: Path) -> Path:
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        (root / "sample.py").write_text("def answer():\n    return 42\n", encoding="utf-8")
        subprocess.run(["git", "add", "sample.py"], cwd=root, check=True)
        subprocess.run(
            [
                "git",
                "-c", "user.name=JCut Test",
                "-c", "user.email=jcut-test@example.invalid",
                "commit", "-q", "-m", "fixture",
            ],
            cwd=root,
            check=True,
        )
        output = root / "report"
        result = subprocess.run(
            [sys.executable, str(SCRIPT), "--root", str(root), "--output-dir", str(output)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return output

    def check_current(self, root: Path, output: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--root", str(root),
                "--output-dir", str(output),
                "--check-current",
            ],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_ownership_resolution_prefers_highest_priority(self) -> None:
        manifest = {
            "schema": generator.OWNERSHIP_SCHEMA,
            "rules": [
                {"owner": "fallback", "layer": "L5", "priority": 1, "patterns": ["*"]},
                {"owner": "audio", "layer": "L3", "priority": 10, "patterns": ["audio_*"]},
            ],
        }
        ownership, violations = generator.resolve_declared_ownership(
            "audio_engine.cpp", manifest
        )
        self.assertEqual(ownership["declared_owner"], "audio")
        self.assertEqual(ownership["layer"], "L3")
        self.assertEqual(violations, [])

    def test_dependency_graph_resolves_architecture_and_reachability(self) -> None:
        def record(
            path: str,
            *,
            layer: str,
            owner: str,
            dependencies: list[str] | None = None,
            symbols: list[dict[str, object]] | None = None,
            calls: list[dict[str, object]] | None = None,
            targets: list[str] | None = None,
            declared_target: str | None = None,
        ) -> dict[str, object]:
            return {
                "path": path,
                "language": "C++",
                "layer": layer,
                "declared_owner": owner,
                "dependencies": dependencies or [],
                "symbols": symbols or [],
                "calls": calls or [],
                "build_targets": targets or [],
                "declared_target": declared_target,
            }

        responsibility_symbols = [
            {
                "name": name,
                "qualified_name": name,
                "kind": "function",
                "is_definition": True,
            }
            for name in (
                "decodeAudioFrame",
                "renderAudioFrame",
                "prepareAudioFrame",
                "finalizeAudioFrame",
            )
        ]
        files = [
            record(
                "entry.cpp",
                layer="L0",
                owner="entry",
                dependencies=["runtime.h"],
                calls=[{
                    "callee": "work",
                    "callee_usr": "c:@F@work#",
                    "line": 10,
                    "column": 5,
                }],
                targets=["app"],
            ),
            record(
                "runtime.h",
                layer="L3",
                owner="runtime",
                dependencies=["domain.h"],
                declared_target="runtime_lib",
            ),
            record(
                "domain.h",
                layer="L2",
                owner="domain",
                dependencies=["runtime.h"],
            ),
            record(
                "worker.cpp",
                layer="L3",
                owner="runtime",
                symbols=[{
                    "name": "work",
                    "qualified_name": "work",
                    "kind": "function",
                    "usr": "c:@F@work#",
                    "is_definition": True,
                    "line": 1,
                }],
                targets=["runtime_lib"],
            ),
            record("dead.cpp", layer="L2", owner="domain"),
            record(
                "duplicate_a.cpp",
                layer="L2",
                owner="domain",
                symbols=responsibility_symbols,
            ),
            record(
                "duplicate_b.cpp",
                layer="L2",
                owner="domain",
                symbols=responsibility_symbols,
            ),
        ]

        graph = generator.build_dependency_graph(files)
        by_path = {item["path"]: item for item in files}
        self.assertEqual(
            by_path["entry.cpp"]["resolved_dependencies"][0]["target"],
            "runtime.h",
        )
        self.assertEqual(
            by_path["entry.cpp"]["calls"][0]["resolved_target"]["file"],
            "worker.cpp",
        )
        self.assertEqual(by_path["entry.cpp"]["dependency_metrics"]["fan_out"], 2)
        self.assertEqual(by_path["worker.cpp"]["dependency_metrics"]["fan_in"], 1)
        self.assertEqual(graph["include_cycles"][0]["files"], ["domain.h", "runtime.h"])
        self.assertTrue(any(
            item["source"] == "runtime.h" and item["target"] == "domain.h"
            for item in graph["layer_direction_violations"]
        ))
        self.assertTrue(any(
            item["source_target"] == "app" and item["target_target"] == "runtime_lib"
            for item in graph["target_to_target_edges"]
        ))
        self.assertTrue(any(
            item["source_owner"] == "entry" and item["target_owner"] == "runtime"
            for item in graph["cross_owner_dependencies"]
        ))
        self.assertTrue(any(
            item["path"] == "dead.cpp" and item["status"] == "dead"
            for item in graph["dead_or_unreachable_implementations"]
        ))
        self.assertTrue(any(
            set(item["files"]) == {"duplicate_a.cpp", "duplicate_b.cpp"}
            for item in graph["duplicate_responsibility_clusters"]
        ))

    def test_dependency_resolution_prefers_local_include_and_maps_python_module(self) -> None:
        files = [
            {
                "path": "legacy/tool.cpp",
                "language": "C++",
                "dependencies": ["shared.h"],
                "symbols": [],
                "calls": [],
                "build_targets": [],
            },
            {
                "path": "legacy/shared.h",
                "language": "C++",
                "dependencies": [],
                "symbols": [],
                "calls": [],
                "build_targets": [],
            },
            {
                "path": "shared.h",
                "language": "C++",
                "dependencies": [],
                "symbols": [],
                "calls": [],
                "build_targets": [],
            },
            {
                "path": "pkg/worker.py",
                "language": "Python",
                "dependencies": ["pkg.helpers"],
                "symbols": [],
                "calls": [],
                "build_targets": [],
            },
            {
                "path": "pkg/helpers.py",
                "language": "Python",
                "dependencies": [],
                "symbols": [],
                "calls": [],
                "build_targets": [],
            },
        ]

        graph = generator.build_dependency_graph(files)
        by_path = {item["path"]: item for item in files}
        self.assertEqual(
            by_path["legacy/tool.cpp"]["resolved_dependencies"][0]["target"],
            "legacy/shared.h",
        )
        self.assertEqual(
            by_path["pkg/worker.py"]["resolved_dependencies"][0]["target"],
            "pkg/helpers.py",
        )
        self.assertEqual(graph["summary"]["ambiguous_dependencies"], 0)

    def test_ownership_resolution_reports_ambiguous_top_priority(self) -> None:
        manifest = {
            "schema": generator.OWNERSHIP_SCHEMA,
            "rules": [
                {"owner": "one", "layer": "L2", "priority": 10, "patterns": ["*.cpp"]},
                {"owner": "two", "layer": "L3", "priority": 10, "patterns": ["audio_*"]},
            ],
        }
        _, violations = generator.resolve_declared_ownership("audio_engine.cpp", manifest)
        self.assertEqual(violations, ["ambiguous_declared_owner"])

    def test_production_build_targets_exclude_test_executables(self) -> None:
        self.assertEqual(
            generator.production_build_targets(["editor_core", "test_audio_engine"]),
            ["editor_core"],
        )

    def test_python_ast_records_nesting_spans_and_imports(self) -> None:
        source = """import json
from pathlib import Path
from . import helper

class Worker:
    def run(self, value):
        def nested():
            return value
        return nested()
"""
        symbols, dependencies, error = generator.python_structure(source, "worker.py")
        self.assertIsNone(error)
        self.assertEqual(dependencies, [".helper", "json", "pathlib"])
        by_name = {item["qualified_name"]: item for item in symbols}
        self.assertEqual(by_name["Worker"]["span_lines"], 5)
        self.assertEqual(by_name["Worker::run"]["kind"], "method")
        self.assertEqual(by_name["Worker.run::nested"]["span_lines"], 2)

    def test_shader_outline_finds_function_extent(self) -> None:
        source = """#version 450
vec4 grade(vec4 color) {
    if (color.r > 0.5) {
        color.g = 1.0;
    }
    return color;
}
void main() { grade(vec4(1.0)); }
"""
        symbols, findings = generator.shader_structure(source)
        self.assertEqual([item["name"] for item in symbols], ["grade", "main"])
        self.assertEqual(symbols[0]["span_lines"], 6)
        self.assertEqual(findings, [])

    def test_shader_outline_reports_unmatched_braces(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "broken.frag"
            source.write_text("void main() {\n    if (true) {\n}\n", encoding="utf-8")
            index = generator.analyze(root, [Path("broken.frag")], ())
            file_record = index["files"][0]
            self.assertEqual(
                [item["kind"] for item in file_record["unmatched_constructs"]],
                ["unmatched-opening-brace"],
            )
            self.assertFalse(file_record["parse_succeeded"])
            self.assertEqual(file_record["symbol_coverage_confidence"]["level"], "low")
            self.assertEqual(index["summary"]["parse_errors"], 1)

    def test_ctags_fallback_rejects_malformed_braces_and_attributes_warnings(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "broken.cpp"
            source.write_text("int broken() {\n    return 1;\n", encoding="utf-8")

            original_run_ctags = generator.run_ctags

            def run_ctags_with_warning(root_path, paths):
                symbols, _ = original_run_ctags(root_path, paths)
                return symbols, ["broken.cpp: warning: incomplete input"]

            generator.run_ctags = run_ctags_with_warning
            try:
                index = generator.analyze(root, [Path("broken.cpp")], ())
            finally:
                generator.run_ctags = original_run_ctags

            file_record = index["files"][0]
            self.assertEqual(file_record["parser"], "universal-ctags-fallback")
            self.assertFalse(file_record["parse_succeeded"])
            self.assertIsNone(file_record["compilation_succeeded"])
            self.assertEqual(file_record["compilation_status"], "not-attempted")
            self.assertIn("broken.cpp: warning: incomplete input", file_record["diagnostics"])
            self.assertIn("structural validation failed", file_record["parse_error"])
            self.assertEqual(
                [item["kind"] for item in file_record["unmatched_constructs"]],
                ["unmatched-opening-brace"],
            )
            self.assertEqual(file_record["symbol_coverage_confidence"]["level"], "low")
            suspicious_kinds = {
                item["kind"] for item in file_record["suspicious_constructs"]
            }
            self.assertIn("semantic-parser-fallback", suspicious_kinds)
            self.assertIn("parser-diagnostic", suspicious_kinds)
            self.assertEqual(index["summary"]["parse_errors"], 1)

    def test_clang_ast_records_authoritative_cpp_structure_and_calls(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "sample.cpp"
            source.write_text(
                """int connect(int);
int QStringLiteral(const char*);
template <typename Callback> void runPass(double, Callback) {}
struct Worker {
    operator bool() const { return true; }
    void run() {
        connect(1);
        auto callback = []() { return 1; };
        runPass(0.2, callback);
        QStringLiteral("value");
    }
};
""",
                encoding="utf-8",
            )
            database = root / "compile_commands.json"
            database.write_text(
                json.dumps([{
                    "directory": str(root),
                    "command": f"clang++ -std=c++20 -c {source} -o sample.o",
                    "file": str(source),
                }]),
                encoding="utf-8",
            )

            index = generator.analyze(
                root,
                [Path("sample.cpp")],
                (),
                compile_commands_path=database,
                clang_jobs=1,
            )
            self.assertEqual(index["parser_warnings"], [])
            self.assertEqual(index["clang_ast"]["translation_units_succeeded"], 1)
            file_record = index["files"][0]
            self.assertEqual(file_record["parser"], "clang-ast")
            self.assertTrue(file_record["parse_succeeded"])
            self.assertTrue(file_record["compilation_succeeded"])
            self.assertEqual(file_record["compilation_status"], "succeeded")
            self.assertEqual(file_record["diagnostics"], [])
            self.assertEqual(file_record["symbol_coverage_confidence"]["level"], "high")
            self.assertEqual(file_record["unmatched_constructs"], [])
            by_name = {item["qualified_name"]: item for item in file_record["symbols"]}
            self.assertEqual(by_name["Worker::operator bool"]["name"], "operator bool")
            self.assertEqual(by_name["Worker::operator bool"]["type"], "bool () const")
            self.assertEqual(by_name["Worker::run"]["span_lines"], 6)
            self.assertEqual(by_name["Worker::run"]["parser"], "clang-ast")
            self.assertNotIn("callback", by_name)
            call_names = {item["callee"] for item in file_record["calls"]}
            self.assertTrue(any(name.endswith("connect") for name in call_names))
            self.assertTrue(any(name.endswith("runPass") for name in call_names))
            self.assertTrue(any(name.endswith("QStringLiteral") for name in call_names))

    def test_outline_references_nested_symbol_indices(self) -> None:
        symbols, _, _ = generator.python_structure(
            "class Worker:\n    def run(self):\n        return 1\n", "worker.py"
        )
        symbols.sort(key=lambda item: (item["line"], -item["span_lines"], item["name"]))
        outline = generator.build_outline(symbols)
        self.assertEqual(len(outline), 1)
        self.assertEqual(symbols[outline[0]["symbol_index"]]["name"], "Worker")
        child = outline[0]["children"][0]
        self.assertEqual(symbols[child["symbol_index"]]["name"], "run")

    def test_cli_generates_json_and_markdown_for_git_corpus(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            (root / "sample.py").write_text("def answer():\n    return 42\n", encoding="utf-8")
            (root / "sample.cpp").write_text(
                '#include "sample.h"\nint answer() { return 42; }\n', encoding="utf-8"
            )
            output = root / "report"
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--root", str(root), "--output-dir", str(output),
                 "--large-file-lines", "1"],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            index = json.loads((output / "code_structure.json").read_text(encoding="utf-8"))
            self.assertEqual(index["schema"], generator.SCHEMA)
            self.assertEqual(index["summary"]["files"], 2)
            cpp = next(item for item in index["files"] if item["path"] == "sample.cpp")
            self.assertEqual(cpp["dependencies"], ["sample.h"])
            self.assertTrue(any(item["name"] == "answer" for item in cpp["symbols"]))
            self.assertEqual(cpp["parser"], "universal-ctags-fallback")
            self.assertTrue(cpp["parse_succeeded"])
            self.assertIsNone(cpp["compilation_succeeded"])
            self.assertEqual(cpp["diagnostics"], [])
            self.assertIsNotNone(cpp["parser_fallback_reason"])
            self.assertEqual(cpp["symbol_coverage_confidence"]["level"], "low")
            self.assertEqual(cpp["unmatched_constructs"], [])
            self.assertEqual(
                cpp["suspicious_constructs"][0]["kind"],
                "semantic-parser-fallback",
            )
            markdown = (output / "code_structure.md").read_text(encoding="utf-8")
            self.assertIn("## Large-file outlines (2 files)", markdown)

    def test_discovery_ignores_tracked_files_deleted_in_worktree(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            subprocess.run(["git", "init", "-q"], cwd=root, check=True)
            deleted = root / "deleted.cpp"
            deleted.write_text("int deleted();\n", encoding="utf-8")
            subprocess.run(["git", "add", "deleted.cpp"], cwd=root, check=True)
            deleted.unlink()
            (root / "present.cpp").write_text("int present();\n", encoding="utf-8")

            self.assertEqual(
                generator.discover_files(root, ()),
                [Path("present.cpp")],
            )

    def test_check_current_accepts_unchanged_artifact_and_records_inputs(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            index = json.loads((output / "code_structure.json").read_text(encoding="utf-8"))
            freshness = index["freshness"]
            self.assertEqual(freshness["generator_version"], generator.GENERATOR_VERSION)
            self.assertEqual(len(freshness["generator_sha256"]), 64)
            self.assertEqual(len(freshness["git_commit"]), 40)
            self.assertEqual(len(freshness["dirty_state_fingerprint"]), 64)
            self.assertEqual(len(freshness["corpus_fingerprint"]), 64)
            self.assertIsNone(freshness["ownership_manifest"]["sha256"])
            self.assertIsNone(freshness["build_file"]["sha256"])
            self.assertIsNone(freshness["compile_commands"]["sha256"])

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("Artifact is current", result.stdout)
            markdown = (output / "code_structure.md").read_text(encoding="utf-8")
            self.assertIn("## Artifact provenance", markdown)
            self.assertIn(freshness["corpus_fingerprint"], markdown)

    def test_check_current_rejects_changed_indexed_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            (root / "sample.py").write_text("def answer():\n    return 43\n", encoding="utf-8")

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("indexed file changed: sample.py", result.stderr)

    def test_check_current_rejects_added_indexed_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            (root / "added.py").write_text("value = 1\n", encoding="utf-8")

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("indexed file added: added.py", result.stderr)

    def test_check_current_rejects_removed_indexed_file(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            (root / "sample.py").unlink()

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("indexed file removed: sample.py", result.stderr)

    def test_check_current_rejects_recorded_metadata_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            artifact_path = output / "code_structure.json"
            index = json.loads(artifact_path.read_text(encoding="utf-8"))
            index["freshness"]["generator_version"] = "stale-version"
            artifact_path.write_text(json.dumps(index), encoding="utf-8")

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("generator_version changed", result.stderr)

    def test_check_current_rejects_ownership_manifest_change(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            (root / "code_ownership.json").write_text(
                json.dumps({"schema": generator.OWNERSHIP_SCHEMA, "rules": []}),
                encoding="utf-8",
            )

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("ownership_manifest changed", result.stderr)

    def test_check_current_rejects_build_file_change(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            build = root / "build"
            build.mkdir()
            (build / "build.ninja").write_text("# newly configured\n", encoding="utf-8")

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("build_file changed", result.stderr)

    def test_check_current_rejects_compilation_database_change(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = self.create_generated_fixture(root)
            build = root / "build"
            build.mkdir()
            (build / "compile_commands.json").write_text("[]\n", encoding="utf-8")

            result = self.check_current(root, output)
            self.assertEqual(result.returncode, 4)
            self.assertIn("compile_commands changed", result.stderr)


if __name__ == "__main__":
    unittest.main()
