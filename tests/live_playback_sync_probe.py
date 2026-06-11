#!/usr/bin/env python3
"""Probe live JCut playback sync across audio, video, and subtitle domains.

This attaches to a running editor control server. It is intended for real media
cases such as the Baltimore County project, not as a synthetic unit test.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any

TIMELINE_FPS = 30.0
TIMELINE_SAMPLE_RATE = 48_000.0


@dataclass
class Sample:
    t: float
    payload: dict[str, Any]


def request_json(
    method: str,
    url: str,
    body: dict[str, Any] | None = None,
    timeout: float = 4.0,
    attempts: int = 4,
) -> dict[str, Any]:
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    last_error: Exception | None = None
    for attempt in range(max(1, attempts)):
        req = urllib.request.Request(url, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as response:
                return json.loads(response.read().decode("utf-8"))
        except urllib.error.HTTPError as exc:
            last_error = exc
            if exc.code != 503 or attempt + 1 >= attempts:
                raise
        except urllib.error.URLError as exc:
            last_error = exc
            if attempt + 1 >= attempts:
                raise
        time.sleep(0.25 * float(attempt + 1))
    raise RuntimeError(f"request failed after retries: {last_error}")


def number(value: Any, default: float = 0.0) -> float:
    if isinstance(value, bool):
        return float(value)
    if isinstance(value, (int, float)):
        return float(value)
    return default


def integer(value: Any, default: int = 0) -> int:
    if isinstance(value, bool):
        return int(value)
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return int(value)
    return default


def selected_clip(state: dict[str, Any], contains: str) -> dict[str, Any]:
    selected = state.get("state", {}).get("selectedClip", {})
    if selected and contains.lower() in selected.get("filePath", "").lower():
        return selected
    for clip in state.get("state", {}).get("timeline", []):
        if contains.lower() in clip.get("filePath", "").lower():
            return clip
    return selected if isinstance(selected, dict) else {}


def expected_source_frame_for_clip(clip: dict[str, Any], timeline_frame: int) -> int | None:
    try:
        start_frame = integer(clip.get("startFrame"))
        duration_frames = integer(clip.get("durationFrames"))
        source_in = integer(clip.get("sourceInFrame"))
        source_fps = number(clip.get("sourceFps"), 0.0)
        playback_rate = number(clip.get("playbackRate"), 1.0)
    except Exception:
        return None
    if source_fps <= 0 or timeline_frame < start_frame or timeline_frame >= start_frame + duration_frames:
        return None
    local_timeline_frame = timeline_frame - start_frame
    return source_in + int((local_timeline_frame * source_fps * playback_rate) // TIMELINE_FPS)


def summarize(values: list[float]) -> dict[str, float]:
    if not values:
        return {"count": 0}
    ordered = sorted(values)
    p95_index = min(len(ordered) - 1, int(round((len(ordered) - 1) * 0.95)))
    return {
        "count": len(values),
        "min": ordered[0],
        "max": ordered[-1],
        "avg": sum(values) / len(values),
        "p95": ordered[p95_index],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=os.environ.get("JCUT_CONTROL_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("JCUT_CONTROL_PORT", "0") or "0"))
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--warmup-seconds", type=float, default=1.0)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--speed", type=float, default=3.0)
    parser.add_argument("--seek-frame", type=int, default=-1)
    parser.add_argument("--clip-contains", default="Hho5MORgIj8")
    parser.add_argument("--max-audio-video-drift-frames", type=float, default=24.0)
    parser.add_argument("--max-audio-video-drift-p95-frames", type=float, default=4.0)
    parser.add_argument("--max-video-source-drift-frames", type=float, default=2.0)
    parser.add_argument("--max-presented-source-lag-frames", type=float, default=4.0)
    parser.add_argument("--start-if-needed", action="store_true")
    parser.add_argument("--leave-playing", action="store_true")
    args = parser.parse_args()

    if args.port <= 0:
        print("SKIP: set JCUT_CONTROL_PORT or pass --port", file=sys.stderr)
        return 77

    base = f"http://{args.host}:{args.port}"
    try:
        initial_playback = request_json("GET", f"{base}/playback")
        state = request_json("GET", f"{base}/state")
    except urllib.error.URLError as exc:
        print(f"FAIL: could not reach JCut at {base}: {exc}", file=sys.stderr)
        return 2

    clip = selected_clip(state, args.clip_contains)
    if not clip or args.clip_contains.lower() not in clip.get("filePath", "").lower():
        print(f"FAIL: selected/timeline clip does not match {args.clip_contains!r}", file=sys.stderr)
        return 2

    original_editor = initial_playback.get("editor", {})
    original_speed = number(original_editor.get("playback_speed"), 1.0)
    original_clock = original_editor.get("clock_source", "auto")
    original_warp = original_editor.get("audio_warp_mode", "time_stretch")
    original_loop = bool(original_editor.get("playback_loop_enabled", False))

    request_json("POST", f"{base}/playback", {
        "playback_speed": args.speed,
        "clock_source": "audio",
        "audio_warp_mode": "time_stretch",
        "playback_loop_enabled": original_loop,
    })
    if args.seek_frame >= 0:
        request_json("POST", f"{base}/playhead", {"frame": args.seek_frame})

    started_by_probe = False
    playhead = request_json("GET", f"{base}/playhead")
    if args.start_if_needed and not playhead.get("playback_active", False):
        click = request_json("POST", f"{base}/click-item", {"id": "transport.play"}, timeout=8.0)
        if not click.get("ok", False):
            print(f"FAIL: could not start playback: {click}", file=sys.stderr)
            return 2
        started_by_probe = True
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            playhead = request_json("GET", f"{base}/playhead")
            if playhead.get("playback_active", False):
                break
            time.sleep(0.1)
        if not playhead.get("playback_active", False):
            print("FAIL: playback did not become active after transport.play", file=sys.stderr)
            return 2

    if args.warmup_seconds > 0:
        time.sleep(args.warmup_seconds)

    samples: list[Sample] = []
    sync_latencies_ms: list[float] = []
    deadline = time.monotonic() + max(args.seconds, args.interval)
    try:
        while time.monotonic() < deadline:
            started = time.monotonic()
            sync_payload = request_json("GET", f"{base}/playback/sync", timeout=4.0)
            sync_latencies_ms.append((time.monotonic() - started) * 1000.0)
            samples.append(Sample(time.monotonic(), {"sync": sync_payload}))
            time.sleep(max(0.05, args.interval))
    finally:
        if started_by_probe and not args.leave_playing:
            try:
                request_json("POST", f"{base}/click-item", {"id": "transport.pause"}, timeout=8.0)
            except Exception:
                pass
        request_json("POST", f"{base}/playback", {
            "playback_speed": original_speed,
            "clock_source": original_clock,
            "audio_warp_mode": original_warp,
            "playback_loop_enabled": original_loop,
        })

    if not samples:
        print("FAIL: no samples collected", file=sys.stderr)
        return 2

    audio_video_drift_frames: list[float] = []
    video_source_drift_frames: list[float] = []
    presented_source_lag_frames: list[float] = []
    frame_advances: list[float] = []
    failures: list[dict[str, Any]] = []
    previous_frame: int | None = None
    playing_count = 0
    audio_blocked_count = 0
    time_stretch_miss_count = 0

    for sample in samples:
        sync = sample.payload["sync"]
        playing = bool(sync.get("playback_active", False))
        if playing:
            playing_count += 1

        editor_frame = integer(sync.get("editor_current_frame"))
        projected_audio_frame = integer(sync.get("projected_audio_clock_absolute_frame"), editor_frame)
        audio_video_drift = editor_frame - projected_audio_frame
        audio_video_drift_frames.append(abs(float(audio_video_drift)))

        if sync.get("audio_playback_blocked", False) or sync.get("pitch_preserving_audio_blocked", False):
            audio_blocked_count += 1
        time_stretch_miss_count = max(time_stretch_miss_count, integer(sync.get("time_stretch_cache_miss_count")))

        requested_source = integer(sync.get("active_requested_source_frame"), -1)
        presented_source = integer(sync.get("active_presented_source_frame"), -1)
        expected_source = expected_source_frame_for_clip(clip, editor_frame)
        if expected_source is not None and requested_source >= 0:
            video_source_drift_frames.append(abs(float(requested_source - expected_source)))
        if requested_source >= 0 and presented_source >= 0:
            presented_source_lag_frames.append(abs(float(requested_source - presented_source)))

        if previous_frame is not None:
            frame_advances.append(float(editor_frame - previous_frame))
        previous_frame = editor_frame

        if abs(audio_video_drift) > args.max_audio_video_drift_frames:
            failures.append({
                "type": "audio_video_drift",
                "editor_frame": editor_frame,
                "projected_audio_frame": projected_audio_frame,
                "drift_frames": audio_video_drift,
            })
        if expected_source is not None and requested_source >= 0:
            source_drift = requested_source - expected_source
            if abs(source_drift) > args.max_video_source_drift_frames:
                failures.append({
                    "type": "video_source_drift",
                    "editor_frame": editor_frame,
                    "requested_source_frame": requested_source,
                    "expected_source_frame": expected_source,
                    "drift_frames": source_drift,
                })
        if requested_source >= 0 and presented_source >= 0:
            source_lag = requested_source - presented_source
            if abs(source_lag) > args.max_presented_source_lag_frames:
                failures.append({
                    "type": "presented_source_lag",
                    "requested_source_frame": requested_source,
                    "presented_source_frame": presented_source,
                    "lag_frames": source_lag,
                })

    audio_video_summary = summarize(audio_video_drift_frames)
    if number(audio_video_summary.get("p95"), 0.0) > args.max_audio_video_drift_p95_frames:
        failures.append({
            "type": "audio_video_drift_p95",
            "p95_frames": audio_video_summary.get("p95"),
            "max_allowed_p95_frames": args.max_audio_video_drift_p95_frames,
        })

    report = {
        "ok": not failures and playing_count > 0 and audio_blocked_count == 0 and time_stretch_miss_count == 0,
        "clip": {
            "id": clip.get("id", ""),
            "filePath": clip.get("filePath", ""),
            "sourceFps": clip.get("sourceFps", 0),
            "playbackRate": clip.get("playbackRate", 1),
        },
        "playback": {
            "speed": args.speed,
            "clock_source": "audio",
            "audio_warp_mode": "time_stretch",
            "sample_count": len(samples),
            "playing_sample_count": playing_count,
        },
        "thresholds": {
            "max_audio_video_drift_frames": args.max_audio_video_drift_frames,
            "max_audio_video_drift_p95_frames": args.max_audio_video_drift_p95_frames,
            "max_video_source_drift_frames": args.max_video_source_drift_frames,
            "max_presented_source_lag_frames": args.max_presented_source_lag_frames,
        },
        "summary": {
            "audio_video_drift_frames": audio_video_summary,
            "video_source_drift_frames": summarize(video_source_drift_frames),
            "presented_source_lag_frames": summarize(presented_source_lag_frames),
            "editor_frame_advances_per_sample": summarize(frame_advances),
            "audio_blocked_sample_count": audio_blocked_count,
            "time_stretch_cache_miss_count": time_stretch_miss_count,
            "sync_latency_ms": summarize(sync_latencies_ms),
        },
        "failures": failures[:20],
        "failure_count": len(failures),
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
