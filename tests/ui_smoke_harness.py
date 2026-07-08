#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import os
import signal
import shutil
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
ONSCREEN_BASE_URL = "http://127.0.0.1:40130"
OFFSCREEN_BASE_URL = "http://127.0.0.1:40131"
EDITOR_PROCESS_PATTERN = f"{REPO_ROOT}/(build|build-asan)/(editor|jcut)"
SPEAKER_FLOW_CLIP = REPO_ROOT / "testbench_assets" / "video" / "pseudorandom_source.mp4"
SPEAKER_FLOW_SOURCE_MARKER = REPO_ROOT / "testbench_assets" / "video" / "pseudorandom_source.source"
SPEAKER_FLOW_SOURCE_ID = "sam3_assets_videos_0001"


class HarnessFailure(RuntimeError):
    pass


def ensure_speaker_flow_clip() -> None:
    source_marker = (
        SPEAKER_FLOW_SOURCE_MARKER.read_text(encoding="utf-8").strip()
        if SPEAKER_FLOW_SOURCE_MARKER.exists()
        else ""
    )
    if not SPEAKER_FLOW_CLIP.exists() or source_marker != SPEAKER_FLOW_SOURCE_ID:
        SPEAKER_FLOW_CLIP.parent.mkdir(parents=True, exist_ok=True)
        frame_pattern = REPO_ROOT / "sam3" / "assets" / "videos" / "0001" / "%d.jpg"
        cmd = [
            "ffmpeg",
            "-y",
            "-framerate", "30",
            "-start_number", "0",
            "-i", str(frame_pattern),
            "-f", "lavfi",
            "-i", "sine=frequency=440:sample_rate=48000:duration=6",
            "-frames:v", "180",
            "-c:v", "libx264",
            "-pix_fmt", "yuv420p",
            "-g", "30",
            "-c:a", "aac",
            "-shortest",
            str(SPEAKER_FLOW_CLIP),
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        if result.returncode != 0 or not SPEAKER_FLOW_CLIP.exists():
            detail = result.stderr.strip() or result.stdout.strip()
            raise HarnessFailure(f"failed to generate speaker-flow clip with ffmpeg: {detail}")
        SPEAKER_FLOW_SOURCE_MARKER.write_text(SPEAKER_FLOW_SOURCE_ID + "\n", encoding="utf-8")

    transcript_path = SPEAKER_FLOW_CLIP.with_suffix(".json")
    if transcript_path.exists():
        return
    words = []
    for index in range(18):
        start = index / 3.0
        words.append({
            "start": start,
            "end": start + 0.28,
            "word": f"word{index}",
            "text": f"word{index}",
            "speaker": "SPEAKER_00",
        })
    transcript_path.write_text(json.dumps({
        "segments": [{"speaker": "SPEAKER_00", "words": words}],
        "speaker_profiles": {
            "SPEAKER_00": {
                "name": "Speaker 1",
                "organization": "Test",
            }
        },
    }, indent=2), encoding="utf-8")


def request(base_url: str,
            path: str,
            method: str = "GET",
            payload: dict[str, Any] | None = None,
            timeout: float = 3.0) -> Any:
    data = None
    headers: dict[str, str] = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{base_url}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read()
            content_type = response.headers.get("Content-Type", "")
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        raise HarnessFailure(f"{method} {path} returned HTTP {exc.code}: {detail}") from exc
    except TimeoutError as exc:
        raise HarnessFailure(f"{method} {path} timed out after {timeout:.1f}s") from exc
    except urllib.error.URLError as exc:
        raise HarnessFailure(f"{method} {path} failed: {exc.reason}") from exc
    except OSError as exc:
        raise HarnessFailure(f"{method} {path} failed: {exc}") from exc
    if not body:
        return {}
    if "application/json" in content_type:
        return json.loads(body.decode("utf-8"))
    return body


def wait_until(predicate, timeout: float, description: str, interval: float = 0.1) -> Any:
    deadline = time.time() + timeout
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except HarnessFailure as exc:
            last_error = exc
        time.sleep(interval)
    if last_error is not None:
        raise HarnessFailure(f"timed out waiting for {description}: {last_error}") from last_error
    raise HarnessFailure(f"timed out waiting for {description}")


def wait_for_json(process: subprocess.Popen[str],
                  base_url: str,
                  path: str,
                  timeout: float,
                  description: str) -> Any:
    def poll() -> Any:
        if process.poll() is not None:
            raise HarnessFailure(f"editor exited before {description} (returncode={process.returncode})")
        return request(base_url, path, timeout=1.5)
    return wait_until(poll, timeout=timeout, description=description)


def wait_for_ui_payload(process: subprocess.Popen[str],
                        base_url: str,
                        timeout: float,
                        require_speaker_table_rows: bool,
                        min_speaker_table_rows: int) -> Any:
    def poll() -> Any:
        if process.poll() is not None:
            raise HarnessFailure(f"editor exited before /ui became ready (returncode={process.returncode})")
        payload = request(base_url, "/ui?refresh=1", timeout=8.0)
        if not payload.get("ok"):
            return None
        if not require_speaker_table_rows:
            return payload
        ui_root = payload["window"]
        facedetections_table = continuity_track_table(ui_root)
        if facedetections_table is None:
            return None
        row_count = int(facedetections_table.get("rows", 0))
        if row_count < min_speaker_table_rows:
            return None
        return payload
    return wait_until(poll, timeout=timeout, description="/ui speaker-track rows")


def ui_request(base_url: str, payload: dict[str, Any], timeout: float = 8.0) -> Any:
    return request(base_url, "/ui", method="POST", payload=payload, timeout=timeout)


def kill_existing_editors() -> None:
    probe = subprocess.run(
        ["pgrep", "-f", EDITOR_PROCESS_PATTERN],
        text=True,
        capture_output=True,
        cwd=str(REPO_ROOT),
    )
    if probe.returncode not in (0, 1):
        raise HarnessFailure(f"failed to probe existing editor processes: {probe.stderr.strip()}")
    if probe.returncode == 1:
        return

    for line in probe.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            os.kill(int(line), signal.SIGTERM)
        except ProcessLookupError:
            continue

    deadline = time.time() + 5.0
    while time.time() < deadline:
        probe = subprocess.run(
            ["pgrep", "-f", EDITOR_PROCESS_PATTERN],
            text=True,
            capture_output=True,
            cwd=str(REPO_ROOT),
        )
        if probe.returncode == 1:
            return
        time.sleep(0.1)

    probe = subprocess.run(
        ["pgrep", "-f", EDITOR_PROCESS_PATTERN],
        text=True,
        capture_output=True,
        cwd=str(REPO_ROOT),
    )
    for line in probe.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            os.kill(int(line), signal.SIGKILL)
        except ProcessLookupError:
            continue


def resolve_editor_path(asan: bool) -> Path:
    build_dir = REPO_ROOT / ("build-asan" if asan else "build")
    for candidate in (
        build_dir / "jcut",
        # macOS bundle layout (MACOSX_BUNDLE TRUE in CMakeLists).
        build_dir / "jcut.app" / "Contents" / "MacOS" / "jcut",
        build_dir / "editor",
        build_dir / "bin" / "editor",
        build_dir / "bin" / "jcut",
    ):
        if candidate.exists():
            return candidate
    return build_dir / "editor"


def launch_editor(offscreen: bool,
                  asan: bool,
                  valgrind: bool,
                  software_rendering: bool) -> subprocess.Popen[str]:
    cmd = [str(resolve_editor_path(asan))]
    if valgrind:
        cmd = [
            "valgrind",
            "--tool=memcheck",
            "--leak-check=full",
            "--track-origins=yes",
            "--error-exitcode=101",
            "--num-callers=32",
        ] + cmd
    env = os.environ.copy()
    if offscreen:
        env["QT_QPA_PLATFORM"] = "offscreen"
    if software_rendering or valgrind:
        env["QT_QUICK_BACKEND"] = "software"
        env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        env["QT_OPENGL"] = "software"
    if valgrind:
        env["EDITOR_FORCE_NULL_RHI"] = "1"
    env["EDITOR_CONTROL_PORT"] = "40131" if offscreen else "40130"
    env["JCUT_UI_AUTOMATION"] = "1"
    env["JCUT_PROJECT_ROOT"] = str(REPO_ROOT)
    return subprocess.Popen(
        cmd,
        cwd=str(REPO_ROOT),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )


class HarnessMonitor:
    def __init__(self, process: subprocess.Popen[str]) -> None:
        self.process = process
        self.lines: list[str] = []
        self._thread = threading.Thread(target=self._pump_output, daemon=True)
        self._thread.start()

    def _pump_output(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.append(line.rstrip())

    def recent_lines(self, limit: int = 80) -> list[str]:
        return self.lines[-limit:]


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5.0)


def find_widget(node: dict[str, Any], widget_id: str) -> dict[str, Any] | None:
    if node.get("id") == widget_id:
        return node
    for child in node.get("children", []):
        found = find_widget(child, widget_id)
        if found is not None:
            return found
    return None


def find_widgets(node: dict[str, Any], predicate) -> list[dict[str, Any]]:
    matches: list[dict[str, Any]] = []
    if predicate(node):
        matches.append(node)
    for child in node.get("children", []):
        matches.extend(find_widgets(child, predicate))
    return matches


def list_items(widget: dict[str, Any]) -> list[dict[str, Any]]:
    items = widget.get("items", [])
    return items if isinstance(items, list) else []


def require_widget(ui_root: dict[str, Any], widget_id: str) -> dict[str, Any]:
    widget = find_widget(ui_root, widget_id)
    if widget is None:
        raise HarnessFailure(f"required widget not found: {widget_id}")
    return widget


def require_enabled_visible(widget: dict[str, Any], widget_id: str) -> None:
    state = widget.get("state", {})
    if not state.get("visible", False):
        raise HarnessFailure(f"{widget_id} is not visible")
    if not state.get("enabled", False):
        raise HarnessFailure(f"{widget_id} is not enabled")


def require_present_enabled(widget: dict[str, Any], widget_id: str) -> None:
    state = widget.get("state", {})
    if not state.get("enabled", False):
        raise HarnessFailure(f"{widget_id} is not enabled")


def table_by_header(ui_root: dict[str, Any], header_text: str) -> dict[str, Any] | None:
    matches = find_widgets(
        ui_root,
        lambda node: node.get("role") == "table_widget" and any(
            header_text.lower() in str(header).lower()
            for header in node.get("headers", [])
        ),
    )
    return matches[0] if matches else None


def continuity_track_table(ui_root: dict[str, Any]) -> dict[str, Any] | None:
    matches = find_widgets(
        ui_root,
        lambda node: node.get("role") == "table_widget"
        and node.get("headers", []) == ["Stream", "Track", "Frames", "Range", "Source"],
    )
    return matches[0] if matches else None


def save_binary(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(content)


def reset_speaker_flow_artifacts(clip_path: Path) -> None:
    transcript_path = clip_path.with_suffix(".json")
    editable_path = clip_path.with_name(f"{clip_path.stem}_editable.json")
    sidecar_dir = clip_path.with_name(f"{clip_path.stem}.jcut")
    removable = [
        editable_path,
        editable_path.with_name(f"{editable_path.stem}_facedetections.bin"),
        editable_path.with_name(f"{editable_path.stem}_facedetections_processed.bin"),
        editable_path.with_name(f"{editable_path.stem}_identity.bin"),
        transcript_path.with_name(f"{transcript_path.stem}_facedetections.bin"),
        transcript_path.with_name(f"{transcript_path.stem}_facedetections_processed.bin"),
        transcript_path.with_name(f"{transcript_path.stem}_identity.bin"),
    ]
    for path in removable:
        if path.exists():
            path.unlink()
    if sidecar_dir.exists():
        shutil.rmtree(sidecar_dir)
    (sidecar_dir / "facedetections" / "clip_video_01").mkdir(parents=True, exist_ok=True)
    editable_path.write_text(transcript_path.read_text(encoding="utf-8"), encoding="utf-8")


def configure_editor_project_root(editor_path: Path) -> None:
    del editor_path


def build_if_requested(args: argparse.Namespace) -> None:
    if args.skip_build:
        return
    build_cmd = ["./build.sh"]
    if args.asan:
        build_cmd.append("--asan")
    build = subprocess.run(
        build_cmd,
        cwd=str(REPO_ROOT),
        text=True,
        capture_output=True,
        timeout=args.build_timeout,
    )
    if build.returncode != 0:
        raise HarnessFailure(
            f"build failed\nstdout:\n{build.stdout}\nstderr:\n{build.stderr}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="UI smoke harness for the live editor control-server surface."
    )
    parser.add_argument("--offscreen", action="store_true", help="Launch headlessly with QT_QPA_PLATFORM=offscreen")
    parser.add_argument("--asan", action="store_true", help="Use build-asan/editor")
    parser.add_argument("--valgrind", action="store_true", help="Run the editor under valgrind")
    parser.add_argument("--software-rendering", action="store_true", help="Force software rendering")
    parser.add_argument("--skip-build", action="store_true", help="Do not run ./build.sh before launch")
    parser.add_argument("--startup-timeout", type=float, default=20.0)
    parser.add_argument("--build-timeout", type=float, default=120.0)
    parser.add_argument("--artifact-dir", default=str(REPO_ROOT / "tests" / "artifacts" / "ui_harness"))
    parser.add_argument("--screenshot-out", default="", help="Optional screenshot path; defaults under artifact-dir")
    parser.add_argument("--exercise-speaker-table-menu", action="store_true",
                        help="If the continuity-track table has rows, open its context menu and verify actions.")
    parser.add_argument("--exercise-generated-track-assignment", action="store_true",
                        help="Generate continuity tracks on the local sample clip and assign one playhead track.")
    parser.add_argument("--require-speaker-table-rows", action="store_true",
                        help="Fail if the continuity-track table exists but has no rows.")
    parser.add_argument("--min-speaker-table-rows", type=int, default=1,
                        help="Minimum continuity-track rows required when --require-speaker-table-rows is set.")
    args = parser.parse_args()

    if args.valgrind and args.asan:
        print(json.dumps({"ok": False, "error": "--asan and --valgrind cannot be combined"}), file=sys.stderr)
        return 1
    if args.valgrind:
        args.software_rendering = True
        args.startup_timeout = max(args.startup_timeout, 45.0)

    artifact_dir = Path(args.artifact_dir).resolve()
    screenshot_out = Path(args.screenshot_out).resolve() if args.screenshot_out else artifact_dir / "ui.png"
    base_url = OFFSCREEN_BASE_URL if args.offscreen else ONSCREEN_BASE_URL

    process: subprocess.Popen[str] | None = None
    monitor: HarnessMonitor | None = None
    try:
        build_if_requested(args)
        if args.exercise_generated_track_assignment:
            ensure_speaker_flow_clip()
            reset_speaker_flow_artifacts(SPEAKER_FLOW_CLIP)
        kill_existing_editors()

        editor_path = resolve_editor_path(args.asan)
        if not editor_path.exists():
            raise HarnessFailure(f"editor binary not found: {editor_path}")
        configure_editor_project_root(editor_path)

        process = launch_editor(
            offscreen=args.offscreen,
            asan=args.asan,
            valgrind=args.valgrind,
            software_rendering=args.software_rendering,
        )
        monitor = HarnessMonitor(process)

        health = wait_for_json(process, base_url, "/health", args.startup_timeout, "/health")
        ui_payload = wait_for_ui_payload(
            process,
            base_url,
            args.startup_timeout,
            require_speaker_table_rows=args.require_speaker_table_rows,
            min_speaker_table_rows=max(1, args.min_speaker_table_rows),
        )
        if not health.get("ok"):
            raise HarnessFailure("/health returned ok=false")
        if not ui_payload.get("ok"):
            raise HarnessFailure("/ui returned ok=false")

        ui_root = ui_payload["window"]
        require_enabled_visible(require_widget(ui_root, "transport.play"), "transport.play")
        require_enabled_visible(require_widget(ui_root, "preview.window"), "preview.window")
        require_present_enabled(require_widget(ui_root, "speakers.generate_facedetections"), "speakers.generate_facedetections")
        require_present_enabled(require_widget(ui_root, "speakers.facedetections_settings"), "speakers.facedetections_settings")
        require_widget(ui_root, "speakers.assign_facedetections")
        require_widget(ui_root, "speakers_identities_section")
        require_widget(ui_root, "speakers_continuity_section")
        require_widget(ui_root, "speakers_framing_section")
        require_widget(ui_root, "speakers_debug_section")
        require_widget(ui_root, "speakers.show_playhead_tracks")
        require_widget(ui_root, "speakers.playhead_tracks")

        try:
            subtab = request(base_url, "/speakers/subtab", timeout=1.5)
        except HarnessFailure as exc:
            subtab = {"ok": False, "error": str(exc)}

        facedetections_table = continuity_track_table(ui_root)
        if facedetections_table is None and not args.exercise_generated_track_assignment:
            raise HarnessFailure("could not find the speaker continuity-track table in the UI tree")

        row_count = int(facedetections_table.get("rows", 0)) if facedetections_table is not None else 0
        if args.require_speaker_table_rows and row_count <= 0:
            raise HarnessFailure("speaker continuity-track table has no rows")

        speaker_flow_status: dict[str, Any] = {"ok": False, "skipped": True}
        if args.exercise_generated_track_assignment:
            inspector_tabs = find_widgets(
                ui_root,
                lambda node: node.get("role") == "tab_widget"
                and any(tab.get("label") == "Speakers" for tab in node.get("tabs", [])),
            )
            if not inspector_tabs:
                raise HarnessFailure("could not find inspector tab widget")
            speaker_tab_select = ui_request(
                base_url,
                {
                    "op": "tab_select",
                    "path": inspector_tabs[0]["path"],
                    "tabLabel": "Speakers",
                    "timeoutMs": 10000,
                },
                timeout=15.0,
            )
            if not speaker_tab_select.get("ok"):
                raise HarnessFailure(f"failed to select Speakers inspector tab: {speaker_tab_select}")

            def wait_for_speaker_flow_ready() -> tuple[dict[str, Any], dict[str, Any]]:
                profile_payload = request(base_url, "/profile", timeout=8.0)
                profile = profile_payload.get("profile", {})
                preview = profile.get("preview", {})
                if preview.get("selected_clip_id") != "clip_video_01":
                    return None
                payload = request(base_url, "/ui?refresh=1", timeout=8.0)
                if not payload.get("ok"):
                    return None
                roster_widget = require_widget(payload["window"], "speakers.roster")
                if int(roster_widget.get("rows", 0)) <= 0:
                    return None
                return payload, profile_payload

            ui_payload, profile_payload = wait_until(
                wait_for_speaker_flow_ready,
                timeout=30.0,
                description="speaker flow project state",
                interval=0.5,
            )
            ui_root = ui_payload["window"]
            roster = require_widget(ui_root, "speakers.roster")
            facedetections_table = require_widget(ui_root, "speakers.continuity_table")
            add_track_buttons = find_widgets(
                ui_root,
                lambda node: str(node.get("id", "")).startswith("speakers.roster.add_tracks."),
            )
            if not add_track_buttons:
                raise HarnessFailure("could not find speaker add-tracks button")
            selected_speaker_id = str(add_track_buttons[0]["id"]).split(".")[-1]
            roster_select = ui_request(
                base_url,
                {
                    "op": "click",
                    "id": add_track_buttons[0]["id"],
                    "timeoutMs": 10000,
                },
                timeout=15.0,
            )
            if not roster_select.get("ok"):
                raise HarnessFailure(f"failed to activate speaker row: {roster_select}")

            def wait_for_selected_speaker() -> dict[str, Any]:
                payload = request(base_url, "/ui?refresh=1", timeout=8.0)
                if not payload.get("ok"):
                    return None
                selected_speaker_widget = require_widget(payload["window"], "speakers.selected_speaker")
                if selected_speaker_widget.get("text") == "No speaker selected":
                    return None
                return payload

            ui_payload = wait_until(
                wait_for_selected_speaker,
                timeout=10.0,
                description="selected speaker label",
                interval=0.5,
            )
            ui_root = ui_payload["window"]

            generator_control = request(
                base_url,
                "/facedetections/generator-control",
                method="POST",
                payload={
                    "mode": "fixed",
                    "detector_workers": 1,
                    "detector_pipeline_slots": 1,
                    "launch_profile": "throughput",
                    "live_preview": False,
                    "control_window": False,
                    "progress_output": False,
                },
                timeout=8.0,
            )
            if not generator_control.get("ok"):
                raise HarnessFailure(f"failed to configure FaceDetections generator: {generator_control}")

            generate_result = ui_request(
                base_url,
                {
                    "op": "click",
                    "id": "speakers.generate_facedetections",
                    "timeoutMs": 180000,
                },
                timeout=190.0,
            )
            if not generate_result.get("ok"):
                raise HarnessFailure(f"failed to generate continuity tracks: {generate_result}")

            def wait_for_generated_ui() -> dict[str, Any]:
                payload = request(base_url, "/ui?refresh=1", timeout=8.0)
                if not payload.get("ok"):
                    return None
                continuity_widget = require_widget(payload["window"], "speakers.continuity_table")
                continuity_rows = int(continuity_widget.get("rows", 0))
                if continuity_rows <= 0:
                    return None
                return payload

            generated_ui = wait_until(
                wait_for_generated_ui,
                timeout=45.0,
                description="generated continuity rows",
                interval=0.5,
            )
            ui_root = generated_ui["window"]
            facedetections_table = require_widget(ui_root, "speakers.continuity_table")
            row_count = int(facedetections_table.get("rows", 0))

            def wait_for_playhead_track() -> tuple[dict[str, Any], dict[str, Any]]:
                payload = request(base_url, "/ui?refresh=1", timeout=8.0)
                if not payload.get("ok"):
                    return None
                playhead_widget = require_widget(payload["window"], "speakers.playhead_tracks")
                for item in list_items(playhead_widget):
                    text = str(item.get("text", ""))
                    if item.get("selectable", False) and "No Tracks At Playhead" not in text and "No Continuity Tracks" not in text:
                        return payload, item
                return None

            generated_ui, playhead_item = wait_until(
                wait_for_playhead_track,
                timeout=15.0,
                description="playhead track candidate after generation",
                interval=0.25,
            )
            ui_root = generated_ui["window"]

            track_map_query = urllib.parse.urlencode({
                "clipId": "clip_video_01",
                "filePath": str(SPEAKER_FLOW_CLIP),
                "transcriptPath": str(SPEAKER_FLOW_CLIP.with_name(f"{SPEAKER_FLOW_CLIP.stem}_editable.json")),
                "full": "1",
            })
            track_map_payload = request(
                base_url,
                f"/speaker-flow/track-map?{track_map_query}",
                timeout=8.0,
            )
            if not track_map_payload.get("ok"):
                raise HarnessFailure(f"speaker-flow track-map REST endpoint failed: {track_map_payload}")
            for required_key in ("section_track_map", "track_identity_map", "contiguous_sections", "speaker_flow_clip"):
                if required_key not in track_map_payload:
                    raise HarnessFailure(f"speaker-flow track-map REST endpoint missing {required_key}")

            if args.offscreen:
                selected_speaker = require_widget(generated_ui["window"], "speakers.selected_speaker")
                speaker_flow_status = {
                    "ok": True,
                    "skipped": False,
                    "clip": str(SPEAKER_FLOW_CLIP),
                    "selected_clip_id": profile_payload.get("profile", {}).get("preview", {}).get("selected_clip_id", ""),
                    "generated_track_rows": row_count,
                    "selected_speaker": selected_speaker.get("text", ""),
                    "playhead_track_text": playhead_item.get("text", ""),
                    "track_map_rest": {
                        "section_track_map_count": len(track_map_payload.get("section_track_map", [])),
                        "track_identity_map_count": len(track_map_payload.get("track_identity_map", [])),
                        "contiguous_sections_count": len(track_map_payload.get("contiguous_sections", [])),
                    },
                }

            if not args.offscreen:
                playhead_select = ui_request(
                    base_url,
                    {
                        "op": "item_select",
                        "id": "speakers.playhead_tracks",
                        "row": int(playhead_item["row"]),
                        "timeoutMs": 10000,
                    },
                    timeout=15.0,
                )
                if not playhead_select.get("ok"):
                    raise HarnessFailure(f"failed to select playhead track: {playhead_select}")

                assign_result = ui_request(
                    base_url,
                    {
                        "op": "click",
                        "id": "speakers.assign_facedetections",
                        "timeoutMs": 20000,
                    },
                    timeout=25.0,
                )
                if not assign_result.get("ok"):
                    raise HarnessFailure(f"failed to assign selected playhead track: {assign_result}")

                editable_transcript_path = SPEAKER_FLOW_CLIP.with_name(
                    f"{SPEAKER_FLOW_CLIP.stem}_editable.json"
                )

                def wait_for_assignment_persisted() -> dict[str, Any]:
                    transcript_root = json.loads(editable_transcript_path.read_text(encoding="utf-8"))
                    track_map = (
                        transcript_root.get("speaker_flow", {})
                        .get("clips", {})
                        .get("clip_video_01", {})
                        .get("resolved_current", {})
                        .get("track_identity_map", [])
                    )
                    matching_rows = [
                        row for row in track_map
                        if row.get("identity_id") == selected_speaker_id
                    ]
                    if not matching_rows:
                        return None
                    return {
                        "track_map": track_map,
                        "matching_rows": matching_rows,
                    }

                assignment_state = wait_until(
                    wait_for_assignment_persisted,
                    timeout=30.0,
                    description="persisted speaker assignment",
                    interval=0.5,
                )
                assigned_items = assignment_state["matching_rows"]
                selected_speaker = require_widget(generated_ui["window"], "speakers.selected_speaker")
                speaker_flow_status = {
                    "ok": True,
                    "skipped": False,
                    "clip": str(SPEAKER_FLOW_CLIP),
                    "selected_clip_id": profile_payload.get("profile", {}).get("preview", {}).get("selected_clip_id", ""),
                    "generated_track_rows": row_count,
                    "assigned_track_count": len(assigned_items),
                    "selected_speaker": selected_speaker.get("text", ""),
                    "playhead_track_text": playhead_item.get("text", ""),
                }

        context_menu: dict[str, Any] | None = None
        context_menu_status: dict[str, Any] = {"ok": False, "skipped": True}
        if args.exercise_speaker_table_menu and row_count > 0:
            try:
                context_menu_response = request(
                    base_url,
                    "/ui/table/context-action",
                    method="POST",
                    payload={
                        "table": {
                            "selector": {
                                "class": "QTableWidget",
                                "headersContains": "Track",
                                "withinPath": "speakers.section.continuity",
                                "visibleOnly": True,
                            }
                        },
                        "row": 0,
                        "column": 0,
                    },
                    timeout=8.0,
                )
                context_menu = context_menu_response.get("menu", {})
                actions = json.dumps(context_menu, sort_keys=True)
                context_menu_status = {"ok": context_menu_response.get("ok", False)}
                if context_menu_response.get("ok") and "Find Matching Tracks" not in actions:
                    context_menu_status = {
                        "ok": False,
                        "error": "speaker table context menu did not expose 'Find Matching Tracks'",
                    }
            except HarnessFailure as exc:
                context_menu_status = {"ok": False, "error": str(exc)}

        screenshot_status: dict[str, Any]
        try:
            screenshot = request(base_url, "/screenshot?source=window", timeout=5.0)
            if isinstance(screenshot, (bytes, bytearray)) and screenshot:
                save_binary(screenshot_out, bytes(screenshot))
                screenshot_status = {"ok": True, "path": str(screenshot_out)}
            else:
                screenshot_status = {"ok": False, "error": "empty screenshot payload"}
        except HarnessFailure as exc:
            screenshot_status = {"ok": False, "error": str(exc)}

        try:
            project = request(base_url, "/project", timeout=8.0)
        except HarnessFailure as exc:
            project = {"ok": False, "error": str(exc)}

        result = {
            "ok": True,
            "base_url": base_url,
            "pid": health.get("pid"),
            "project": project,
            "subtab": subtab,
            "speaker_track_table_rows": row_count,
            "speaker_flow": speaker_flow_status,
            "context_menu_checked": bool(args.exercise_speaker_table_menu and row_count > 0),
            "context_menu_status": context_menu_status,
            "screenshot": screenshot_status,
        }
        if context_menu is not None:
            result["context_menu"] = context_menu
        print(json.dumps(result, indent=2))
        return 0
    except Exception as exc:
        failure = {
            "ok": False,
            "error": str(exc),
        }
        if monitor is not None:
            failure["recent_log"] = monitor.recent_lines()
        print(json.dumps(failure, indent=2), file=sys.stderr)
        return 1
    finally:
        if process is not None:
            stop_process(process)


if __name__ == "__main__":
    raise SystemExit(main())
