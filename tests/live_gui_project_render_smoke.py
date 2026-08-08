#!/usr/bin/env python3
"""Short smoke test for the current project's live Vulkan preview render."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def request_json(base: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    data = json.dumps(payload).encode() if payload is not None else None
    request = Request(
        f"{base}{path}",
        data=data,
        headers={"Content-Type": "application/json"} if data is not None else {},
        method="POST" if data is not None else "GET",
    )
    try:
        with urlopen(request, timeout=15) as response:
            return json.load(response)
    except (HTTPError, URLError, TimeoutError, json.JSONDecodeError) as error:
        raise RuntimeError(f"{request.method} {path} failed: {error}") from error


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def set_playing(base: str, playing: bool) -> None:
    health = request_json(base, "/health")
    if bool(health.get("playback_active")) == playing:
        return
    control = "transport.play" if playing else "transport.pause"
    result = request_json(base, "/click-item", {"id": control})
    require(bool(result.get("ok")), f"{control} failed: {result}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", default="http://127.0.0.1:40130")
    parser.add_argument("--seconds", type=float, default=6.0)
    parser.add_argument("--frame", type=int, help="optional project timeline frame to test")
    args = parser.parse_args()
    require(args.seconds >= 1.0, "--seconds must be at least 1")

    initial = request_json(args.base, "/health")
    require(bool(initial.get("ok")), "JCut health endpoint is not ready")
    pid = int(initial.get("pid", 0))
    require(pid > 0, "health response did not include a valid PID")

    repo_root = Path(__file__).resolve().parents[1]
    running_exe = Path(os.readlink(f"/proc/{pid}/exe")).resolve()
    built_exe = (repo_root / "build" / "jcut").resolve()
    require(running_exe == built_exe, f"running executable is stale: {running_exe} != {built_exe}")

    original_frame = int(initial.get("current_frame", 0))
    original_playing = bool(initial.get("playback_active"))
    set_playing(args.base, False)
    test_frame = args.frame if args.frame is not None else original_frame
    try:
        request_json(args.base, "/playhead", {"frame": test_frame})

        health = request_json(args.base, "/health")
        if not bool(health.get("preview_window_exposed")):
            request_json(args.base, "/window", {"op": "raise"})
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                health = request_json(args.base, "/health")
                if bool(health.get("preview_window_exposed")):
                    break
                time.sleep(0.1)
        require(bool(health.get("preview_window_exposed")), "live preview window is not exposed")

        baseline = request_json(args.base, "/profile?live=1&force=1")["profile"]["preview"]
        baseline_presented = int(baseline.get("presented_frames", 0))
        baseline_handoff_failures = int(baseline.get("handoff_failures", 0))

        try:
            set_playing(args.base, True)
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline:
                time.sleep(min(0.25, deadline - time.monotonic()))
        finally:
            set_playing(args.base, False)

        settle_deadline = time.monotonic() + 2.0
        while True:
            final = request_json(args.base, "/profile?live=1&force=1")["profile"]
            preview = final["preview"]
            decoded_ready = int(preview.get("ready_decode_status_clips", 0)) == int(
                preview.get("active_decode_status_clips", 0)
            )
            generated_ready = int(preview.get("ready_generated_status_clips", 0)) == int(
                preview.get("active_generated_status_clips", 0)
            )
            if decoded_ready and generated_ready:
                break
            if time.monotonic() >= settle_deadline:
                break
            time.sleep(0.1)
        presented_delta = int(preview.get("presented_frames", 0)) - baseline_presented
        handoff_failure_delta = int(preview.get("handoff_failures", 0)) - baseline_handoff_failures

        require(preview.get("backend") == "vulkan", "preview backend is not Vulkan")
        require(bool(preview.get("native_active")), "native Vulkan preview is inactive")
        require(not bool(preview.get("vulkan_path_uses_qimage")), "preview unexpectedly used QImage")
        require(presented_delta > 0, "no live preview frames were presented")
        require(handoff_failure_delta == 0, f"GPU handoff failures increased by {handoff_failure_delta}")
        require(
            int(preview.get("ready_decode_status_clips", 0))
            == int(preview.get("active_decode_status_clips", 0)),
            "not all active decoded layers were ready",
        )
        require(
            int(preview.get("ready_generated_status_clips", 0))
            == int(preview.get("active_generated_status_clips", 0)),
            "not all active generated layers were ready",
        )
        for kind in ("title", "transcript"):
            candidates = int(preview.get(f"{kind}_candidate_count", 0))
            require(
                int(preview.get(f"{kind}_drawn_count", 0)) == candidates,
                f"{kind} draw count did not match its candidate count",
            )
    finally:
        request_json(args.base, "/playhead", {"frame": original_frame})
        set_playing(args.base, original_playing)

    print(
        "PASS live GUI project render: "
        f"frame={test_frame} seconds={args.seconds:g} presented={presented_delta} "
        f"ready_layers={int(preview.get('ready_decode_status_clips', 0))}/"
        f"{int(preview.get('active_decode_status_clips', 0))} "
        f"titles={int(preview.get('title_drawn_count', 0))} "
        f"transcripts={int(preview.get('transcript_drawn_count', 0))}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as error:
        print(f"FAIL live GUI project render: {error}")
        raise SystemExit(1)
