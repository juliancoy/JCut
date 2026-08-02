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

class Worker:
    def run(self, value):
        def nested():
            return value
        return nested()
"""
        symbols, dependencies, error = generator.python_structure(source, "worker.py")
        self.assertIsNone(error)
        self.assertEqual(dependencies, ["json", "pathlib"])
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
        symbols = generator.shader_structure(source)
        self.assertEqual([item["name"] for item in symbols], ["grade", "main"])
        self.assertEqual(symbols[0]["span_lines"], 6)

    def test_cpp_outline_handles_qt_slots_and_large_inline_class(self) -> None:
        source = """class Worker : public QObject {
    Q_OBJECT
private slots:
    void run()
    {
        if (ready()) {
            work();
        }
    }
};
"""
        symbols = generator.cpp_outline_structure(source)
        by_name = {item["name"]: item for item in symbols}
        self.assertEqual(by_name["Worker"]["span_lines"], 10)
        self.assertEqual(by_name["run"]["scope"], "Worker")
        self.assertEqual(by_name["run"]["span_lines"], 6)
        self.assertNotIn("if", by_name)

    def test_cpp_outline_ignores_braces_in_comments_and_raw_strings(self) -> None:
        source = '''class ShaderOwner {
    int timeout = 5'000;
    const char *shader = R"glsl(
        void main() { if (true) { } }
    )glsl";
    // A misleading closing brace: }
    /* Another one: } */
    void release() { }
};
'''
        symbols = generator.cpp_outline_structure(source)
        owner = next(item for item in symbols if item["name"] == "ShaderOwner")
        self.assertEqual(owner["end_line"], 9)

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


if __name__ == "__main__":
    unittest.main()
