#!/usr/bin/env python3
"""Serve the generated code-structure viewer on the loopback interface."""

from __future__ import annotations

import argparse
import functools
import http.server
import json
import sys
import threading
import webbrowser
from pathlib import Path


REQUIRED_FILES = (
    "index.html",
    "app.css",
    "app.js",
    "code_structure_graph.json",
)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--output-dir", type=Path, default=Path("build/code-structure"))
    parser.add_argument("--host", default="127.0.0.1",
                        help="listen address (default: loopback only)")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--check", action="store_true",
                        help="validate viewer artifacts and exit")
    return parser.parse_args(argv)


def validate_viewer(output_dir: Path) -> list[str]:
    errors = [f"missing {name}" for name in REQUIRED_FILES if not (output_dir / name).is_file()]
    graph_path = output_dir / "code_structure_graph.json"
    if graph_path.is_file():
        try:
            payload = json.loads(graph_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            errors.append(f"invalid code_structure_graph.json: {error}")
        else:
            if payload.get("schema") != "jcut_code_structure_viewer_v1":
                errors.append(f"unsupported viewer schema: {payload.get('schema')!r}")
            if not isinstance(payload.get("nodes"), list) or not isinstance(payload.get("edges"), list):
                errors.append("viewer graph must contain node and edge arrays")
    return errors


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv or sys.argv[1:])
    root = args.root.resolve()
    output_dir = args.output_dir
    if not output_dir.is_absolute():
        output_dir = root / output_dir
    errors = validate_viewer(output_dir)
    if errors:
        print("Code-structure viewer is unavailable:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        print("Regenerate it with: python3 scripts/generate_code_structure.py", file=sys.stderr)
        return 2
    if args.check:
        print(f"Viewer artifacts are valid: {output_dir}")
        return 0

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(output_dir))
    server = http.server.ThreadingHTTPServer((args.host, args.port), handler)
    host, port = server.server_address[:2]
    display_host = "127.0.0.1" if host in {"0.0.0.0", "::"} else host
    url = f"http://{display_host}:{port}/"
    print(f"Serving JCut code structure at {url}")
    print("Press Ctrl+C to stop.")
    if not args.no_browser:
        threading.Timer(0.25, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
