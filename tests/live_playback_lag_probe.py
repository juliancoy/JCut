#!/usr/bin/env python3
"""Attach to a running JCut instance and classify live playback lag.

This is intentionally not a synthetic unit test. It samples the real
`/playback/diagnostics` endpoint while the editor is playing and reports the
first observed failure mode from the actual preview/decode/presenter pipeline.

Run with:

    JCUT_CONTROL_PORT=40130 python3 tests/live_playback_lag_probe.py --seconds 8

Start playback in the UI before running, or use --allow-not-playing to collect a
static baseline.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any


@dataclass
class ClassifiedLag:
    reason: str
    details: dict[str, Any]


def get_json(url: str, timeout: float = 2.0) -> dict[str, Any]:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def number(value: Any, default: float = 0.0) -> float:
    if isinstance(value, bool):
        return float(value)
    if isinstance(value, (int, float)):
        return float(value)
    return default


def classify(sample: dict[str, Any]) -> ClassifiedLag:
    diagnostics = sample.get("diagnostics", {})
    smoothness = diagnostics.get("playback_smoothness", {})
    playback_decode = diagnostics.get("playback_decode", {})
    visible_decode = diagnostics.get("visible_decode_diagnostics", {})

    missing_rate = number(smoothness.get("missing_frame_rate"))
    failure_rate = number(smoothness.get("current_frame_failure_rate"))
    late_rate = number(smoothness.get("late_sample_rate"))
    presented_fps = number(smoothness.get("presented_fps_estimate"))
    exact_rate = number(smoothness.get("exact_hit_rate"))
    visible_block_fraction = number(smoothness.get("visible_request_blocked_fraction"))
    visible_dispatch_rate = number(smoothness.get("visible_request_dispatch_rate"))
    max_frame_lag = number(smoothness.get("max_frame_lag"))
    p95_upload_ms = number(smoothness.get("p95_handoff_upload_ms"))

    playback_pending = number(diagnostics.get("playback_pending_visible_requests"))
    cache_pending = number(diagnostics.get("cache_pending_visible_requests"))
    dropped_presentation = number(diagnostics.get("playback_dropped_presentation_frames"))
    frame_status_last_ms = number(diagnostics.get("frame_status_last_refresh_ms"))
    frame_status_max_ms = number(diagnostics.get("frame_status_max_refresh_ms"))

    visible_wait_ms = number(playback_decode.get("last_visible_wait_ms"))
    max_visible_wait_ms = number(playback_decode.get("max_visible_wait_ms"))
    visible_outcome = playback_decode.get("last_visible_outcome", "")
    qt_delivery_ms = number(playback_decode.get("last_visible_qt_delivery_delay_ms"))
    visible_nulls = number(playback_decode.get("visible_null_completed"))
    visible_obsolete = number(playback_decode.get("visible_obsolete_completed"))

    last_block_reason = diagnostics.get("last_visible_request_block_reason", "")
    active_selection = diagnostics.get("active_frame_selection", "")
    active_failure = diagnostics.get("active_frame_not_up_to_date_failure", "")
    active_exact = bool(diagnostics.get("active_frame_exact", False))
    active_up_to_date = bool(diagnostics.get("active_frame_up_to_date", False))
    playing = bool(diagnostics.get("playing", False))
    has_playback_active = "playback_active" in diagnostics or "editor_playback_active" in diagnostics
    playback_active = bool(diagnostics.get(
        "playback_active",
        diagnostics.get("editor_playback_active", playing),
    ))
    playhead_advance_age_ms = number(diagnostics.get("last_playhead_advance_age_ms"), -1.0)
    heartbeat_age_ms = number(diagnostics.get("main_thread_heartbeat_age_ms"), -1.0)

    details = {
        "playing": playing,
        "playback_active": playback_active,
        "has_live_playback_active": has_playback_active,
        "current_frame": diagnostics.get("current_frame"),
        "fast_current_frame": diagnostics.get("fast_current_frame"),
        "main_thread_heartbeat_age_ms": heartbeat_age_ms,
        "last_playhead_advance_age_ms": playhead_advance_age_ms,
        "presented_fps_estimate": presented_fps,
        "exact_hit_rate": exact_rate,
        "missing_frame_rate": missing_rate,
        "current_frame_failure_rate": failure_rate,
        "late_sample_rate": late_rate,
        "max_frame_lag": max_frame_lag,
        "visible_request_blocked_fraction": visible_block_fraction,
        "visible_request_dispatch_rate": visible_dispatch_rate,
        "playback_pending_visible_requests": playback_pending,
        "cache_pending_visible_requests": cache_pending,
        "playback_dropped_presentation_frames": dropped_presentation,
        "last_visible_wait_ms": visible_wait_ms,
        "max_visible_wait_ms": max_visible_wait_ms,
        "last_visible_outcome": visible_outcome,
        "last_visible_qt_delivery_delay_ms": qt_delivery_ms,
        "visible_null_completed": visible_nulls,
        "visible_obsolete_completed": visible_obsolete,
        "frame_status_last_refresh_ms": frame_status_last_ms,
        "frame_status_max_refresh_ms": frame_status_max_ms,
        "p95_handoff_upload_ms": p95_upload_ms,
        "last_visible_request_block_reason": last_block_reason,
        "active_frame_selection": active_selection,
        "active_frame_exact": active_exact,
        "active_frame_up_to_date": active_up_to_date,
        "active_frame_not_up_to_date_failure": active_failure,
        "visible_decode_diagnostics": visible_decode,
    }

    if playing != playback_active and has_playback_active:
        return ClassifiedLag("diagnostics_disagree_playback_state", details)
    if playing and heartbeat_age_ms > 500:
        return ClassifiedLag("ui_thread_heartbeat_stale_during_playback", details)
    if playback_active and playhead_advance_age_ms > 500:
        return ClassifiedLag("playback_clock_not_advancing", details)
    if missing_rate > 0.02:
        return ClassifiedLag("missing_visible_frames", details)
    if failure_rate > 0.10 or late_rate > 0.10 or max_frame_lag > 2:
        if max_visible_wait_ms > 33 or visible_wait_ms > 33:
            return ClassifiedLag("decoder_visible_wait_over_frame_budget", details)
        if visible_block_fraction > 0.20 or last_block_reason:
            return ClassifiedLag("visible_requests_blocked_or_backlogged", details)
        if dropped_presentation > 0:
            return ClassifiedLag("playback_buffer_presenting_old_frames", details)
        if p95_upload_ms > 8:
            return ClassifiedLag("presenter_handoff_upload_slow", details)
        if frame_status_max_ms > 8:
            return ClassifiedLag("frame_status_refresh_slow_on_ui_thread", details)
        if visible_nulls > 0:
            return ClassifiedLag("decoder_returned_null_visible_frames", details)
        if visible_obsolete > 0:
            return ClassifiedLag("visible_completions_obsolete_before_presentation", details)
        return ClassifiedLag("late_or_inexact_frames_unclassified", details)
    if not active_exact or not active_up_to_date:
        return ClassifiedLag("current_frame_not_exact_or_not_up_to_date", details)
    return ClassifiedLag("no_live_lag_detected", details)


def classify_display(samples: list[dict[str, Any]], screenshot_samples: list[dict[str, Any]]) -> ClassifiedLag | None:
    if len(screenshot_samples) < 2:
        return None

    changed_hashes = {item.get("hash") for item in screenshot_samples if item.get("hash")}
    elapsed_values = [number(item.get("elapsed_ms")) for item in screenshot_samples]
    current_frames = [
        item.get("diagnostics", {}).get("current_frame")
        for item in samples
        if item.get("diagnostics", {}).get("playing", False)
    ]
    current_frame_changed = len({frame for frame in current_frames if frame is not None}) > 1
    details = {
        "screenshot_sample_count": len(screenshot_samples),
        "screenshot_unique_hashes": len(changed_hashes),
        "screenshot_changed_rate": (
            (len(changed_hashes) - 1) / max(1, len(screenshot_samples) - 1)
        ),
        "screenshot_max_elapsed_ms": max(elapsed_values) if elapsed_values else 0.0,
        "screenshot_p95_elapsed_ms": sorted(elapsed_values)[int(0.95 * (len(elapsed_values) - 1))]
        if elapsed_values else 0.0,
        "current_frame_changed": current_frame_changed,
    }
    if current_frame_changed and len(changed_hashes) <= 1:
        return ClassifiedLag("display_framebuffer_not_changing", details)
    if details["screenshot_max_elapsed_ms"] > 100:
        return ClassifiedLag("display_screenshot_capture_slow", details)
    return ClassifiedLag("display_framebuffer_changing", details)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default=os.environ.get("JCUT_CONTROL_HOST", "127.0.0.1"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("JCUT_CONTROL_PORT", "0") or "0"))
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument(
        "--screenshot-interval",
        type=float,
        default=0.0,
        help="Optionally sample /screenshot?include_steps=1 at this interval. Off by default because it perturbs UI paint.",
    )
    parser.add_argument("--allow-not-playing", action="store_true")
    args = parser.parse_args()

    if args.port <= 0:
        print("SKIP: set JCUT_CONTROL_PORT to a running JCut control-server port", file=sys.stderr)
        return 77

    diagnostics_url = f"http://{args.host}:{args.port}/playback/diagnostics?verbose=1"
    screenshot_url = f"http://{args.host}:{args.port}/screenshot?include_steps=1"
    samples: list[dict[str, Any]] = []
    screenshot_samples: list[dict[str, Any]] = []
    classifications: list[ClassifiedLag] = []
    deadline = time.monotonic() + max(args.seconds, args.interval)
    next_screenshot_at = time.monotonic()
    try:
        while time.monotonic() < deadline:
            payload = get_json(diagnostics_url)
            samples.append(payload)
            classifications.append(classify(payload))
            if args.screenshot_interval > 0 and time.monotonic() >= next_screenshot_at:
                try:
                    screenshot = get_json(screenshot_url, timeout=5.0)
                    png_base64 = screenshot.get("png_base64", "")
                    screenshot_samples.append({
                        "hash": hashlib.sha256(png_base64.encode("ascii")).hexdigest()
                        if png_base64 else "",
                        "elapsed_ms": screenshot.get("elapsed_ms", 0),
                        "source_effective": screenshot.get("source_effective", ""),
                    })
                except urllib.error.URLError as exc:
                    screenshot_samples.append({"error": str(exc)})
                next_screenshot_at = time.monotonic() + max(0.25, args.screenshot_interval)
            time.sleep(max(0.05, args.interval))
    except urllib.error.URLError as exc:
        print(f"FAIL: could not reach {diagnostics_url}: {exc}", file=sys.stderr)
        return 2

    if not samples:
        print("FAIL: no diagnostics samples collected", file=sys.stderr)
        return 2

    playing_samples = [
        item for item in samples
        if item.get("diagnostics", {}).get("playing", False)
    ]
    if not playing_samples and not args.allow_not_playing:
        print("SKIP: JCut is not playing; start playback and rerun, or pass --allow-not-playing", file=sys.stderr)
        return 77

    priority = [
        "display_framebuffer_not_changing",
        "diagnostics_disagree_playback_state",
        "ui_thread_heartbeat_stale_during_playback",
        "playback_clock_not_advancing",
        "missing_visible_frames",
        "decoder_visible_wait_over_frame_budget",
        "visible_requests_blocked_or_backlogged",
        "playback_buffer_presenting_old_frames",
        "presenter_handoff_upload_slow",
        "frame_status_refresh_slow_on_ui_thread",
        "decoder_returned_null_visible_frames",
        "visible_completions_obsolete_before_presentation",
        "late_or_inexact_frames_unclassified",
        "current_frame_not_exact_or_not_up_to_date",
        "no_live_lag_detected",
        "display_screenshot_capture_slow",
        "display_framebuffer_changing",
    ]
    display_classification = classify_display(samples, screenshot_samples)
    if display_classification:
        classifications.append(display_classification)
    selected = min(
        classifications,
        key=lambda item: priority.index(item.reason)
        if item.reason in priority
        else priority.index("late_or_inexact_frames_unclassified"),
    )
    report = {
        "ok": True,
        "sample_count": len(samples),
        "playing_sample_count": len(playing_samples),
        "classification": selected.reason,
        "details": selected.details,
        "all_reasons": sorted({item.reason for item in classifications}),
        "display_probe": display_classification.details if display_classification else None,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
