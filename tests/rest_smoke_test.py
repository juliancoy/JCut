#!/usr/bin/env python3

from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parent.parent
BASE_URL = ""
TESTBENCH_STATE = REPO_ROOT / "testbench_state.json"


class TestFailure(RuntimeError):
    pass


@dataclass
class RequestSample:
    timestamp: float
    status: str
    detail: str
    payload: dict[str, Any] | None = None


@dataclass
class Diagnostics:
    phase: str = "init"
    last_good_playhead: dict[str, Any] | None = None
    playhead_samples: list[RequestSample] = field(default_factory=list)
    last_profile: dict[str, Any] | None = None
    last_health: dict[str, Any] | None = None
    last_pid: int | None = None
    max_playhead_latency_ms: float = 0.0
    max_profile_latency_ms: float = 0.0

    def add_playhead_sample(self, status: str, detail: str, payload: dict[str, Any] | None = None) -> None:
        self.playhead_samples.append(
            RequestSample(timestamp=time.time(), status=status, detail=detail, payload=payload)
        )
        if len(self.playhead_samples) > 40:
            self.playhead_samples = self.playhead_samples[-40:]


def request(path: str, method: str = "GET", payload: dict[str, Any] | None = None, timeout: float = 3.0) -> Any:
    data = None
    headers: dict[str, str] = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(f"{BASE_URL}{path}", data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read()
            content_type = response.headers.get("Content-Type", "")
    except urllib.error.HTTPError as exc:
        raise TestFailure(f"{method} {path} returned HTTP {exc.code}") from exc
    except TimeoutError as exc:
        raise TestFailure(f"{method} {path} timed out after {timeout:.1f}s") from exc
    except urllib.error.URLError as exc:
        raise TestFailure(f"{method} {path} failed: {exc.reason}") from exc
    except OSError as exc:
        raise TestFailure(f"{method} {path} failed: {exc}") from exc
    if not body:
        return {}
    if "application/json" in content_type:
        return json.loads(body.decode("utf-8"))
    return body


def find_widget(node: dict[str, Any], widget_id: str) -> dict[str, Any] | None:
    if node.get("id") == widget_id:
        return node
    for child in node.get("children", []):
        found = find_widget(child, widget_id)
        if found is not None:
            return found
    return None


def wait_until(predicate, timeout: float, interval: float = 0.1, description: str = "condition") -> Any:
    deadline = time.time() + timeout
    last_value = None
    last_error: Exception | None = None
    while time.time() < deadline:
        try:
            last_value = predicate()
            if last_value:
                return last_value
        except TestFailure as exc:
            last_error = exc
        time.sleep(interval)
    if last_error is not None:
        raise TestFailure(f"timed out waiting for {description}: {last_error}") from last_error
    raise TestFailure(f"timed out waiting for {description}")


def wait_for_ui(timeout: float) -> dict[str, Any]:
    return wait_until(
        lambda: request("/ui?refresh=1", timeout=3.0),
        timeout=timeout,
        description="/ui",
    )


def wait_for_request_json(path: str,
                          timeout: float,
                          method: str = "GET",
                          payload: dict[str, Any] | None = None,
                          description: str | None = None,
                          request_timeout: float = 3.0) -> dict[str, Any]:
    return wait_until(
        lambda: request(path, method=method, payload=payload, timeout=request_timeout),
        timeout=timeout,
        description=description or path,
    )


def try_request(path: str, method: str = "GET", payload: dict[str, Any] | None = None, timeout: float = 1.0) -> tuple[bool, Any]:
    try:
        return True, request(path, method=method, payload=payload, timeout=timeout)
    except TestFailure as exc:
        return False, str(exc)


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


def reserve_control_port() -> tuple[socket.socket, int]:
    reservation = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    reservation.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    reservation.bind(("127.0.0.1", 0))
    return reservation, int(reservation.getsockname()[1])


def create_isolated_playback_project(parent: Path) -> Path:
    if not TESTBENCH_STATE.is_file():
        raise TestFailure(f"testbench state fixture not found: {TESTBENCH_STATE}")
    state = json.loads(TESTBENCH_STATE.read_text(encoding="utf-8"))
    image_clip = next(
        (clip for clip in state.get("timeline", []) if clip.get("mediaType") == "image"),
        None,
    )
    if image_clip is None:
        raise TestFailure("testbench state fixture has no image clip")
    image_clip = dict(image_clip)
    image_clip.update({
        "id": "rest_smoke_image",
        "label": "REST smoke image",
        "filePath": str((REPO_ROOT / "testbench_assets" / "images" / "life_c.png").resolve()),
        "audioEnabled": False,
        "hasAudio": False,
        "startFrame": 0,
        "durationFrames": 600,
        "trackIndex": 0,
        "sourceDurationFrames": 1,
        "sourceInFrame": 0,
        "useProxy": False,
        "proxyPath": "",
    })
    state.update({
        "timeline": [image_clip],
        "tracks": [state.get("tracks", [{}])[0]],
        "selectedClip": image_clip,
        "selectedClipId": image_clip["id"],
        "selectedClipIds": [image_clip["id"]],
        "currentFrame": 0,
        "playing": False,
        "playbackSpeed": 1.0,
        "playbackClockSource": "timeline",
        "playbackClockSourceExplicit": True,
        "renderSyncMarkers": [],
    })

    root = parent / "workspace"
    project = root / "projects" / "rest-smoke"
    project.mkdir(parents=True)
    (project / "state.json").write_text(
        json.dumps(state, indent=2) + "\n", encoding="utf-8"
    )
    (root / "projects" / ".current_project").write_text(
        "rest-smoke\n", encoding="utf-8"
    )
    return root


def launch_editor(offscreen: bool,
                  asan: bool,
                  valgrind: bool,
                  software_rendering: bool,
                  control_port: int,
                  project_root: Path) -> subprocess.Popen[str]:
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
    env["EDITOR_CONTROL_PORT"] = str(control_port)
    env["JCUT_UI_AUTOMATION"] = "1"
    env["JCUT_PROJECT_ROOT"] = str(project_root)
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
            stripped = line.rstrip()
            self.lines.append(stripped)

    def wait_for_pid(self, timeout: float) -> int:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.process.poll() is not None:
                break
            ok, result = try_request("/health", timeout=1.0)
            if ok and result.get("ok") and result.get("pid"):
                return int(result["pid"])
            time.sleep(0.1)
        raise TestFailure("timed out waiting for editor health")


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=5.0)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait(timeout=5.0)


def wait_for_playhead_progress(
    diagnostics: Diagnostics,
    monitor_seconds: float,
    stall_timeout: float,
    max_consecutive_failures: int,
    poll_interval: float,
    max_playhead_latency_ms: float,
    max_profile_latency_ms: float,
    max_heartbeat_age_ms: int,
    profile_interval: float,
) -> int:
    deadline = time.time() + monitor_seconds
    max_frame = -1
    last_frame: int | None = None
    last_advance_at = time.time()
    last_profile_at = 0.0
    consecutive_failures = 0

    while time.time() < deadline:
        request_started = time.perf_counter()
        ok, result = try_request("/playhead", timeout=1.0)
        playhead_latency_ms = (time.perf_counter() - request_started) * 1000.0
        diagnostics.max_playhead_latency_ms = max(
            diagnostics.max_playhead_latency_ms,
            playhead_latency_ms,
        )
        if max_playhead_latency_ms > 0 and playhead_latency_ms > max_playhead_latency_ms:
            raise TestFailure(
                f"/playhead latency {playhead_latency_ms:.1f}ms exceeded budget "
                f"{max_playhead_latency_ms:.1f}ms"
            )
        if ok:
            playhead = result
            diagnostics.last_good_playhead = playhead
            diagnostics.add_playhead_sample("ok", "playhead", playhead)
            consecutive_failures = 0
            frame = int(playhead["current_frame"])
            if frame > max_frame:
                max_frame = frame
            if last_frame is None or frame != last_frame:
                last_advance_at = time.time()
            last_frame = frame
            if not playhead.get("playback_active"):
                raise TestFailure("playback became inactive during playback monitor window")
        else:
            diagnostics.add_playhead_sample("error", str(result))
            consecutive_failures += 1
            if consecutive_failures >= max_consecutive_failures:
                raise TestFailure(
                    f"/playhead failed {consecutive_failures} times in a row during playback; last error: {result}"
                )

        if time.time() - last_advance_at > stall_timeout:
            raise TestFailure(
                f"playhead stalled for {stall_timeout:.1f}s during playback monitor window"
            )

        now = time.time()
        if profile_interval > 0 and now - last_profile_at >= profile_interval:
            last_profile_at = now
            profile_started = time.perf_counter()
            ok, profile_result = try_request("/profile", timeout=2.0)
            profile_latency_ms = (time.perf_counter() - profile_started) * 1000.0
            diagnostics.max_profile_latency_ms = max(
                diagnostics.max_profile_latency_ms,
                profile_latency_ms,
            )
            if max_profile_latency_ms > 0 and profile_latency_ms > max_profile_latency_ms:
                raise TestFailure(
                    f"/profile latency {profile_latency_ms:.1f}ms exceeded budget "
                    f"{max_profile_latency_ms:.1f}ms"
                )
            if not ok:
                raise TestFailure(f"/profile failed during responsiveness monitor: {profile_result}")
            diagnostics.last_profile = profile_result
            profile = profile_result.get("profile", profile_result)
            heartbeat_age_ms = int(profile.get("main_thread_heartbeat_age_ms", -1))
            if max_heartbeat_age_ms > 0 and heartbeat_age_ms > max_heartbeat_age_ms:
                raise TestFailure(
                    f"main-thread heartbeat age {heartbeat_age_ms}ms exceeded budget "
                    f"{max_heartbeat_age_ms}ms"
                )
            if profile.get("stall_active"):
                raise TestFailure("profile reported stall_active=true during responsiveness monitor")
            if profile.get("ui_thread_responsive") is False:
                raise TestFailure("profile reported ui_thread_responsive=false during responsiveness monitor")

        time.sleep(poll_interval)

    return max_frame


def write_failure_artifacts(
    artifact_dir: Path,
    diagnostics: Diagnostics,
    monitor: HarnessMonitor,
    error: str,
) -> dict[str, Any]:
    artifact_dir.mkdir(parents=True, exist_ok=True)
    timestamp = time.strftime("%Y%m%d-%H%M%S")

    recovery: dict[str, Any] = {}
    for name, path, timeout in (
        ("playhead", "/playhead", 2.0),
        ("health", "/health", 2.0),
        ("profile", "/profile", 2.0),
    ):
        ok, result = try_request(path, timeout=timeout)
        recovery[name] = {"ok": ok, "result": result}

    screenshot_path = artifact_dir / f"{timestamp}-stall.png"
    ok, screenshot_result = try_request("/screenshot", timeout=3.0)
    if ok and isinstance(screenshot_result, (bytes, bytearray)) and screenshot_result:
        screenshot_path.write_bytes(screenshot_result)
        recovery["screenshot"] = {"ok": True, "path": str(screenshot_path)}
    else:
        recovery["screenshot"] = {"ok": False, "result": screenshot_result}

    harness_log_path = artifact_dir / f"{timestamp}-harness.log"
    harness_log_path.write_text("\n".join(monitor.lines[-400:]) + "\n", encoding="utf-8")

    gdb_backtrace_path = artifact_dir / f"{timestamp}-gdb.txt"
    gdb_result: dict[str, Any]
    if diagnostics.last_pid:
        try:
            gdb = subprocess.run(
                [
                    "gdb",
                    "-p",
                    str(diagnostics.last_pid),
                    "-batch",
                    "-ex",
                    "set pagination off",
                    "-ex",
                    "info threads",
                    "-ex",
                    "thread apply all bt",
                ],
                cwd=str(REPO_ROOT),
                text=True,
                capture_output=True,
                timeout=15.0,
            )
            gdb_output = (gdb.stdout or "") + ("\n" + gdb.stderr if gdb.stderr else "")
            gdb_backtrace_path.write_text(gdb_output, encoding="utf-8")
            gdb_result = {
                "ok": gdb.returncode == 0,
                "path": str(gdb_backtrace_path),
                "returncode": gdb.returncode,
            }
        except Exception as exc:
            gdb_result = {"ok": False, "error": str(exc)}
    else:
        gdb_result = {"ok": False, "error": "no pid available"}

    failure_payload = {
        "ok": False,
        "error": error,
        "phase": diagnostics.phase,
        "pid": diagnostics.last_pid,
                "last_good_playhead": diagnostics.last_good_playhead,
                "last_profile": diagnostics.last_profile,
                "last_health": diagnostics.last_health,
                "max_playhead_latency_ms": round(diagnostics.max_playhead_latency_ms, 1),
                "max_profile_latency_ms": round(diagnostics.max_profile_latency_ms, 1),
                "recent_playhead_samples": [
            {
                "t": round(sample.timestamp, 3),
                "status": sample.status,
                "detail": sample.detail,
                "frame": None if not sample.payload else sample.payload.get("current_frame"),
                "active": None if not sample.payload else sample.payload.get("playback_active"),
            }
            for sample in diagnostics.playhead_samples[-20:]
        ],
        "recovery_probe": recovery,
        "process_log": str(harness_log_path),
        "gdb_backtrace": gdb_result,
    }

    result_path = artifact_dir / f"{timestamp}-result.json"
    result_path.write_text(json.dumps(failure_payload, indent=2) + "\n", encoding="utf-8")
    failure_payload["artifact_result"] = str(result_path)
    return failure_payload


def main() -> int:
    parser = argparse.ArgumentParser(description="REST smoke test for the editor")
    parser.add_argument("--offscreen", action="store_true", help="Launch the app headlessly")
    parser.add_argument("--asan", action="store_true", help="Pass --asan through to ./build.sh")
    parser.add_argument("--valgrind", action="store_true", help="Run the launched editor under valgrind memcheck")
    parser.add_argument("--software-rendering", action="store_true", help="Force software rendering in the harness")
    parser.add_argument("--build-timeout", type=float, default=120.0)
    parser.add_argument("--startup-timeout", type=float, default=15.0)
    parser.add_argument("--restart-timeout", type=float, default=15.0)
    parser.add_argument("--play-timeout", type=float, default=8.0)
    parser.add_argument("--advance-timeout", type=float, default=8.0)
    parser.add_argument("--monitor-seconds", type=float, default=4.0)
    parser.add_argument("--stall-timeout", type=float, default=2.5)
    parser.add_argument("--poll-interval", type=float, default=0.25)
    parser.add_argument("--max-playhead-failures", type=int, default=4)
    parser.add_argument("--profile-interval", type=float, default=0.0)
    parser.add_argument("--max-playhead-latency-ms", type=float, default=0.0)
    parser.add_argument("--max-profile-latency-ms", type=float, default=0.0)
    parser.add_argument("--max-heartbeat-age-ms", type=int, default=0)
    parser.add_argument("--pause-timeout", type=float, default=5.0)
    parser.add_argument(
        "--preserve-playback-config",
        action="store_true",
        help="Use the saved project playback speed/clock instead of forcing deterministic timeline playback.",
    )
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--skip-restart", action="store_true")
    parser.add_argument("--artifact-dir", default=str(REPO_ROOT / "tests" / "artifacts"))
    parser.add_argument("--screenshot-out", default=str(REPO_ROOT / "tests" / "rest_smoke_screenshot.png"))
    args = parser.parse_args()
    global BASE_URL
    if args.valgrind and args.asan:
        print(json.dumps({"ok": False, "error": "--asan and --valgrind should not be combined"}), file=sys.stderr)
        return 1
    if args.valgrind:
        args.startup_timeout = max(args.startup_timeout, 45.0)
        args.restart_timeout = max(args.restart_timeout, 45.0)
        args.play_timeout = max(args.play_timeout, 20.0)
        args.advance_timeout = max(args.advance_timeout, 20.0)
        args.software_rendering = True

    if not args.skip_build:
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
            print(json.dumps({"ok": False, "error": "build failed", "stdout": build.stdout, "stderr": build.stderr}), file=sys.stderr)
            return 1

    temp_project = tempfile.TemporaryDirectory(prefix="jcut-rest-smoke-")
    project_root = create_isolated_playback_project(Path(temp_project.name))
    port_reservation, control_port = reserve_control_port()
    BASE_URL = f"http://127.0.0.1:{control_port}"
    # Hold the ephemeral port until immediately before launch. UI automation
    # bypasses the application singleton, so unrelated editor instances can
    # remain running without being terminated by this test.
    port_reservation.close()

    process = launch_editor(
        offscreen=args.offscreen,
        asan=args.asan,
        valgrind=args.valgrind,
        software_rendering=args.software_rendering,
        control_port=control_port,
        project_root=project_root,
    )
    monitor = HarnessMonitor(process)
    diagnostics = Diagnostics()
    try:
        diagnostics.phase = "startup"
        initial_pid = monitor.wait_for_pid(timeout=args.startup_timeout)
        diagnostics.last_health = wait_until(
            lambda: request("/health", timeout=1.0),
            timeout=args.startup_timeout,
            description="/health",
        )
        diagnostics.last_pid = initial_pid
        restarted_pid = initial_pid
        if not args.skip_restart:
            diagnostics.phase = "restart"
            os.kill(initial_pid, signal.SIGKILL)
            process.wait(timeout=5.0)
            process = launch_editor(
                offscreen=args.offscreen,
                asan=args.asan,
                valgrind=args.valgrind,
                software_rendering=args.software_rendering,
                control_port=control_port,
                project_root=project_root,
            )
            monitor = HarnessMonitor(process)
            restarted_pid = monitor.wait_for_pid(timeout=args.restart_timeout)
            if restarted_pid == initial_pid:
                raise TestFailure("restart reused the same pid unexpectedly")
            diagnostics.last_health = wait_until(
                lambda: request("/health", timeout=1.0),
                timeout=args.restart_timeout,
                description="/health after restart",
            )
            diagnostics.last_pid = int(diagnostics.last_health.get("pid", 0))

        diagnostics.phase = "ui"
        ui = wait_for_ui(args.startup_timeout)
        if not ui.get("ok"):
            raise TestFailure("/ui did not return ok=true")
        play_button = find_widget(ui["window"], "transport.play")
        if play_button is None:
            raise TestFailure("transport.play not found in UI tree")
        if not play_button.get("clickable") or not play_button.get("enabled") or not play_button.get("visible"):
            raise TestFailure("transport.play is not clickable/enabled/visible")

        if not args.preserve_playback_config:
            diagnostics.phase = "playback_config"
            playback_config = wait_for_request_json(
                "/playback",
                timeout=5.0,
                method="POST",
                payload={
                    "playback_speed": 1.0,
                    "clock_source": "timeline",
                    "audio_warp_mode": "disabled",
                },
                description="deterministic playback config",
                request_timeout=5.0,
            )
            if not playback_config.get("ok"):
                raise TestFailure(f"failed to force deterministic playback config: {playback_config}")
            seek_result = wait_for_request_json(
                "/playhead",
                timeout=5.0,
                method="POST",
                payload={"frame": 0},
                description="deterministic playhead seek",
                request_timeout=5.0,
            )
            if not seek_result.get("ok"):
                raise TestFailure(f"failed to seek deterministic playback start: {seek_result}")

            state = wait_for_request_json(
                "/state",
                timeout=5.0,
                description="state for playhead clamp regression",
                request_timeout=5.0,
            )
            timeline = state.get("timeline", [])
            clip_ends = [
                int(clip.get("startFrame", 0)) + int(clip.get("durationFrames", 0)) - 1
                for clip in timeline
                if int(clip.get("durationFrames", 0)) > 0
            ]
            if clip_ends:
                last_content_frame = max(clip_ends)
                beyond_content_frame = last_content_frame + 30
                clamp_seek = wait_for_request_json(
                    "/playhead",
                    timeout=5.0,
                    method="POST",
                    payload={"frame": beyond_content_frame},
                    description="playhead clamp past content end",
                    request_timeout=5.0,
                )
                if not clamp_seek.get("ok"):
                    raise TestFailure(f"failed to seek beyond content end: {clamp_seek}")
                clamped_playhead = wait_for_request_json(
                    "/playhead",
                    timeout=5.0,
                    description="playhead after clamp past content end",
                    request_timeout=5.0,
                )
                clamped_frame = int(clamped_playhead.get("current_frame", -1))
                if clamped_frame != last_content_frame:
                    raise TestFailure(
                        "playhead did not clamp to the last content frame: "
                        f"requested={beyond_content_frame} expected={last_content_frame} actual={clamped_frame}"
                    )
                seek_result = wait_for_request_json(
                    "/playhead",
                    timeout=5.0,
                    method="POST",
                    payload={"frame": 0},
                    description="deterministic playhead reset after clamp regression",
                    request_timeout=5.0,
                )
                if not seek_result.get("ok"):
                    raise TestFailure(f"failed to reset deterministic playback start: {seek_result}")

        diagnostics.phase = "click_play"
        click_result = wait_for_request_json(
            "/click-item",
            timeout=max(5.0, args.play_timeout),
            method="POST",
            payload={"id": "transport.play"},
            description="transport.play click",
            request_timeout=max(5.0, args.play_timeout),
        )
        if not click_result.get("ok"):
            raise TestFailure(f"click-item transport.play failed: {click_result}")

        def playback_started() -> Any:
            ok, result = try_request("/playhead")
            if not ok:
                diagnostics.add_playhead_sample("error", str(result))
                return None
            playhead = result
            diagnostics.last_good_playhead = playhead
            diagnostics.add_playhead_sample("ok", "playhead", playhead)
            if playhead.get("ok") and playhead.get("playback_active"):
                return playhead
            return None

        diagnostics.phase = "playback_started"
        started_profile = wait_until(
            playback_started,
            timeout=args.play_timeout,
            description="playback to start",
        )

        start_frame = int(started_profile["current_frame"])

        def frame_advanced() -> Any:
            ok, result = try_request("/playhead")
            if not ok:
                diagnostics.add_playhead_sample("error", str(result))
                return None
            playhead = result
            diagnostics.last_good_playhead = playhead
            diagnostics.add_playhead_sample("ok", "playhead", playhead)
            if not playhead.get("ok"):
                return None
            current_frame = int(playhead["current_frame"])
            if current_frame > start_frame:
                return playhead
            return None

        diagnostics.phase = "advance"
        advanced_profile = wait_until(
            frame_advanced,
            timeout=args.advance_timeout,
            description="frame advancement after play",
        )

        diagnostics.phase = "monitor"
        max_frame = max(
            int(advanced_profile["current_frame"]),
            wait_for_playhead_progress(
                diagnostics,
                monitor_seconds=args.monitor_seconds,
                stall_timeout=args.stall_timeout,
                max_consecutive_failures=args.max_playhead_failures,
                poll_interval=args.poll_interval,
                max_playhead_latency_ms=args.max_playhead_latency_ms,
                max_profile_latency_ms=args.max_profile_latency_ms,
                max_heartbeat_age_ms=args.max_heartbeat_age_ms,
                profile_interval=args.profile_interval,
            ),
        )

        if max_frame <= start_frame:
            raise TestFailure("frame did not continue advancing during playback monitor window")

        diagnostics.phase = "pause_click"
        wait_for_request_json(
            "/click-item",
            timeout=max(5.0, args.pause_timeout),
            method="POST",
            payload={"id": "transport.pause"},
            description="transport.pause click",
            request_timeout=max(5.0, args.pause_timeout),
        )

        def playback_stopped() -> Any:
            playhead = request("/playhead", timeout=1.0)
            diagnostics.last_good_playhead = playhead
            if playhead.get("ok") and not playhead.get("playback_active"):
                return playhead
            return None

        diagnostics.phase = "pause_wait"
        paused_playhead = wait_until(
            playback_stopped,
            timeout=args.pause_timeout,
            description="playback to pause",
        )

        diagnostics.phase = "health_after_pause"
        health_after_pause = request("/health", timeout=3.0)
        diagnostics.last_health = health_after_pause
        if not health_after_pause.get("ok"):
            raise TestFailure("/health did not recover after pause")

        screenshot_path: str | None = None
        if not args.offscreen:
            diagnostics.phase = "screenshot"
            screenshot_result = request("/screenshot?include_steps=1", timeout=5.0)
            if not isinstance(screenshot_result, dict) or not screenshot_result.get("ok"):
                raise TestFailure("screenshot diagnostics request failed")
            step_names = {
                step.get("name")
                for step in screenshot_result.get("steps", [])
                if isinstance(step, dict)
            }
            if "encode_png" not in step_names:
                raise TestFailure("screenshot diagnostics did not report off-UI PNG encoding")
            forbidden_steps = {"prepare_paint", "capture_native_winid"} & step_names
            if forbidden_steps:
                raise TestFailure(
                    f"screenshot used unsafe UI capture steps: {sorted(forbidden_steps)}"
                )
            if "ui_capture_elapsed_ms" not in screenshot_result or "encode_elapsed_ms" not in screenshot_result:
                raise TestFailure("screenshot diagnostics omitted capture/encoding timings")
            source_steps = [
                step
                for step in screenshot_result.get("steps", [])
                if isinstance(step, dict) and step.get("name") == "capture_source"
            ]
            if any(
                not step.get("skipped") and step.get("storage") != "heap_qimage"
                for step in source_steps
            ):
                raise TestFailure("widget screenshot capture did not use heap-backed QImage storage")
            try:
                screenshot = base64.b64decode(screenshot_result.get("png_base64", ""), validate=True)
            except (ValueError, TypeError) as exc:
                raise TestFailure(f"screenshot returned invalid PNG base64: {exc}") from exc
            screenshot_file = Path(args.screenshot_out)
            screenshot_file.parent.mkdir(parents=True, exist_ok=True)
            screenshot_file.write_bytes(screenshot)
            if screenshot_file.stat().st_size == 0:
                raise TestFailure("screenshot was empty")
            screenshot_path = str(screenshot_file)

        print(
            json.dumps(
                {
                    "ok": True,
                    "build_ok": True,
                    "asan": args.asan,
                    "initial_pid": initial_pid,
                    "restarted_pid": restarted_pid,
                    "restart_skipped": args.skip_restart,
                    "start_frame": start_frame,
                    "advanced_frame": max_frame,
                    "paused_frame": int(paused_playhead["current_frame"]),
                    "max_playhead_latency_ms": round(diagnostics.max_playhead_latency_ms, 1),
                    "max_profile_latency_ms": round(diagnostics.max_profile_latency_ms, 1),
                    "screenshot": screenshot_path,
                },
                indent=2,
            )
        )
        return 0
    except Exception as exc:
        diagnostics.last_pid = diagnostics.last_pid or (process.pid if process.poll() is None else None)
        failure_payload = write_failure_artifacts(
            Path(args.artifact_dir),
            diagnostics,
            monitor,
            str(exc),
        )
        failure_payload["asan"] = args.asan
        print(json.dumps(failure_payload), file=sys.stderr)
        return 1
    finally:
        stop_process(process)
        port_reservation.close()
        temp_project.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
