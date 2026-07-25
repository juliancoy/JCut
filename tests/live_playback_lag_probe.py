#!/usr/bin/env python3
"""Attach to a running JCut instance and classify live playback lag.

This is intentionally not a synthetic unit test. During the measured interval
it samples only JCut's lock-free `/playback/telemetry` endpoint, so the probe
does not steal time from the UI/presentation thread. One compact pipeline
diagnostic is collected after the interval to classify any observed failure.

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


def nonnegative_counter_delta(current: float, previous: float) -> float:
    """Return an interval delta without carrying a counter reset forward."""
    return current - previous if current >= previous else 0.0


def measurement_urls(host: str, port: int) -> tuple[str, str, str]:
    base = f"http://{host}:{port}"
    return (
        f"{base}/playback/telemetry",
        f"{base}/playback/diagnostics",
        f"{base}/screenshot?include_steps=1",
    )


def classify_fast_telemetry(sample: dict[str, Any]) -> ClassifiedLag | None:
    playback_active = bool(sample.get("playback_active", False))
    heartbeat_age_ms = number(sample.get("main_thread_heartbeat_age_ms"), -1.0)
    playhead_advance_age_ms = number(
        sample.get("last_playhead_advance_age_ms"), -1.0
    )
    details = {
        "playback_active": playback_active,
        "current_frame": sample.get("current_frame"),
        "active_presented_source_frame":
            sample.get("active_presented_source_frame"),
        "main_thread_heartbeat_age_ms": heartbeat_age_ms,
        "last_playhead_advance_age_ms": playhead_advance_age_ms,
    }
    if playback_active and heartbeat_age_ms > 500:
        return ClassifiedLag(
            "ui_thread_heartbeat_stale_during_playback", details
        )
    if playback_active and playhead_advance_age_ms > 500:
        return ClassifiedLag("playback_clock_not_advancing", details)
    return None


def classify(
    sample: dict[str, Any],
    *,
    presentation_miss_delta: float = 0.0,
    presentation_misses_since_baseline: float = 0.0,
    observed_presented_fps: float | None = None,
    expected_presented_fps: float = 30.0,
    minimum_presented_fps_ratio: float = 0.90,
    allow_smoothness_cadence_fallback: bool = True,
) -> ClassifiedLag:
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
    unique_presentation_misses = number(
        diagnostics.get("unique_presentation_misses")
    )
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
    if (
        observed_presented_fps is None
        and allow_smoothness_cadence_fallback
        and "presented_fps_estimate" in smoothness
    ):
        observed_presented_fps = presented_fps
        presented_fps_source = "playback_smoothness_window"
    elif observed_presented_fps is not None:
        presented_fps_source = "probe_counter_wall_delta"
    else:
        presented_fps_source = "unavailable"
    minimum_presented_fps_ratio = max(
        0.0, min(1.0, minimum_presented_fps_ratio)
    )
    minimum_presented_fps = (
        expected_presented_fps * minimum_presented_fps_ratio
    )
    presented_fps_ratio = (
        observed_presented_fps / expected_presented_fps
        if observed_presented_fps is not None and expected_presented_fps > 0
        else None
    )

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
        "unique_presentation_misses": unique_presentation_misses,
        "unique_presentation_misses_interval_delta": presentation_miss_delta,
        "unique_presentation_misses_since_baseline":
            presentation_misses_since_baseline,
        "observed_presented_fps": observed_presented_fps,
        "observed_presented_fps_source": presented_fps_source,
        "expected_presented_fps": expected_presented_fps,
        "minimum_presented_fps": minimum_presented_fps,
        "minimum_presented_fps_ratio": minimum_presented_fps_ratio,
        "observed_presented_fps_ratio": presented_fps_ratio,
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
    if (
        playback_active
        and observed_presented_fps is not None
        and expected_presented_fps > 0
        and observed_presented_fps < minimum_presented_fps
    ):
        return ClassifiedLag(
            "preview_presentation_cadence_below_target", details
        )
    if missing_rate > 0.02:
        return ClassifiedLag("missing_visible_frames", details)
    if presentation_miss_delta > 0:
        return ClassifiedLag("presenter_recorded_unique_misses", details)
    if failure_rate > 0.10 or late_rate > 0.10 or max_frame_lag > 2:
        if max_visible_wait_ms > 33 or visible_wait_ms > 33:
            return ClassifiedLag("decoder_visible_wait_over_frame_budget", details)
        if visible_block_fraction > 0.20 or last_block_reason:
            return ClassifiedLag("visible_requests_blocked_or_backlogged", details)
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
    parser.add_argument("--warmup-seconds", type=float, default=1.0)
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--expected-fps", type=float, default=30.0)
    parser.add_argument(
        "--minimum-presented-fps-ratio",
        type=float,
        default=0.90,
        help="Classify preview cadence below expected-fps times this ratio.",
    )
    parser.add_argument(
        "--screenshot-interval",
        type=float,
        default=0.0,
        help="Optionally sample /screenshot?include_steps=1 at this interval. Off by default because it perturbs UI paint.",
    )
    parser.add_argument("--allow-not-playing", action="store_true")
    args = parser.parse_args()

    if args.expected_fps <= 0:
        parser.error("--expected-fps must be greater than zero")
    if not 0 < args.minimum_presented_fps_ratio <= 1:
        parser.error(
            "--minimum-presented-fps-ratio must be greater than zero and "
            "no greater than one"
        )

    if args.port <= 0:
        print("SKIP: set JCUT_CONTROL_PORT to a running JCut control-server port", file=sys.stderr)
        return 77

    telemetry_url, diagnostics_url, screenshot_url = measurement_urls(
        args.host, args.port
    )
    telemetry_samples: list[dict[str, Any]] = []
    diagnostic_samples: list[dict[str, Any]] = []
    screenshot_samples: list[dict[str, Any]] = []
    classifications: list[ClassifiedLag] = []
    presentation_miss_baseline = 0.0
    presented_frames_baseline: float | None = None
    presented_frames_latest: float | None = None
    cadence_baseline_at = 0.0
    cadence_latest_at = 0.0
    observed_presented_fps: float | None = None
    presentation_misses_since_baseline = 0.0
    try:
        if args.warmup_seconds > 0:
            time.sleep(args.warmup_seconds)

        baseline_telemetry = get_json(telemetry_url)
        required_counters = (
            "presented_frames",
            "unique_presentation_misses",
        )
        missing_counters = [
            field for field in required_counters
            if field not in baseline_telemetry
        ]
        if missing_counters:
            raise ValueError(
                "playback telemetry is missing required counters: "
                + ", ".join(missing_counters)
            )
        telemetry_samples.append(baseline_telemetry)
        baseline_classification = classify_fast_telemetry(
            baseline_telemetry
        )
        if baseline_classification:
            classifications.append(baseline_classification)
        presentation_miss_baseline = number(
            baseline_telemetry.get("unique_presentation_misses")
        )
        presented_frames_baseline = number(
            baseline_telemetry.get("presented_frames")
        )
        presented_frames_latest = presented_frames_baseline
        cadence_baseline_at = time.monotonic()
        cadence_latest_at = cadence_baseline_at
        deadline = cadence_baseline_at + max(args.seconds, args.interval)
        sampling_interval = max(0.05, args.interval)
        next_sample_at = cadence_baseline_at + sampling_interval
        next_screenshot_at = cadence_baseline_at

        while next_sample_at < deadline:
            time.sleep(max(0.0, next_sample_at - time.monotonic()))
            telemetry = get_json(telemetry_url)
            telemetry_samples.append(telemetry)
            fast_classification = classify_fast_telemetry(telemetry)
            if fast_classification:
                classifications.append(fast_classification)
            if (
                args.screenshot_interval > 0
                and time.monotonic() >= next_screenshot_at
            ):
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
            next_sample_at += sampling_interval

        time.sleep(max(0.0, deadline - time.monotonic()))
        final_telemetry = get_json(telemetry_url)
        cadence_latest_at = time.monotonic()
        telemetry_samples.append(final_telemetry)
        final_fast_classification = classify_fast_telemetry(final_telemetry)
        if final_fast_classification:
            classifications.append(final_fast_classification)

        presented_frames_latest = number(
            final_telemetry.get("presented_frames")
        )
        final_presentation_misses = number(
            final_telemetry.get("unique_presentation_misses")
        )
        if (
            presented_frames_baseline is not None
            and presented_frames_latest < presented_frames_baseline
        ):
            raise ValueError(
                "presented_frames reset during the measurement interval"
            )
        if final_presentation_misses < presentation_miss_baseline:
            raise ValueError(
                "unique_presentation_misses reset during the measurement "
                "interval"
            )
        presentation_misses_since_baseline = nonnegative_counter_delta(
            final_presentation_misses,
            presentation_miss_baseline,
        )
        cadence_elapsed_seconds = cadence_latest_at - cadence_baseline_at
        if (
            presented_frames_baseline is not None
            and cadence_elapsed_seconds > 0
        ):
            observed_presented_fps = (
                nonnegative_counter_delta(
                    presented_frames_latest,
                    presented_frames_baseline,
                )
                / cadence_elapsed_seconds
            )

        final_diagnostics = get_json(diagnostics_url)
        diagnostic_samples.append(final_diagnostics)
        classifications.append(
            classify(
                final_diagnostics,
                presentation_miss_delta=
                    presentation_misses_since_baseline,
                presentation_misses_since_baseline=
                    presentation_misses_since_baseline,
                observed_presented_fps=observed_presented_fps,
                expected_presented_fps=args.expected_fps,
                minimum_presented_fps_ratio=
                    args.minimum_presented_fps_ratio,
                allow_smoothness_cadence_fallback=False,
            )
        )
    except (urllib.error.URLError, ValueError) as exc:
        print(
            f"FAIL: live playback measurement failed via {telemetry_url}: "
            f"{exc}",
            file=sys.stderr,
        )
        return 2

    if not telemetry_samples or not diagnostic_samples:
        print("FAIL: no live playback samples collected", file=sys.stderr)
        return 2

    playing_samples = [
        item for item in telemetry_samples
        if item.get("playback_active", False)
    ]
    if not playing_samples and not args.allow_not_playing:
        print("SKIP: JCut is not playing; start playback and rerun, or pass --allow-not-playing", file=sys.stderr)
        return 77

    priority = [
        "display_framebuffer_not_changing",
        "diagnostics_disagree_playback_state",
        "ui_thread_heartbeat_stale_during_playback",
        "playback_clock_not_advancing",
        "preview_presentation_cadence_below_target",
        "missing_visible_frames",
        "decoder_visible_wait_over_frame_budget",
        "visible_requests_blocked_or_backlogged",
        "presenter_recorded_unique_misses",
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
    display_samples = [
        {
            "diagnostics": {
                "current_frame": item.get("current_frame"),
                "playing": item.get("playback_active", False),
            }
        }
        for item in telemetry_samples
    ]
    display_classification = classify_display(
        display_samples, screenshot_samples
    )
    if display_classification:
        classifications.append(display_classification)
    selected = min(
        classifications,
        key=lambda item: priority.index(item.reason)
        if item.reason in priority
        else priority.index("late_or_inexact_frames_unclassified"),
    )
    cadence_elapsed_seconds = max(
        0.0, cadence_latest_at - cadence_baseline_at
    )
    presented_frames_delta = (
        nonnegative_counter_delta(
            presented_frames_latest,
            presented_frames_baseline,
        )
        if presented_frames_latest is not None
        and presented_frames_baseline is not None
        else None
    )
    cadence_report = {
        "warmup_seconds": max(0.0, args.warmup_seconds),
        "counter_available": presented_frames_delta is not None,
        "presented_frames_delta": presented_frames_delta,
        "elapsed_seconds": cadence_elapsed_seconds,
        "observed_presented_fps": (
            presented_frames_delta / cadence_elapsed_seconds
            if presented_frames_delta is not None
            and cadence_elapsed_seconds > 0
            else selected.details.get("observed_presented_fps")
        ),
        "expected_presented_fps": args.expected_fps,
        "minimum_presented_fps_ratio":
            args.minimum_presented_fps_ratio,
        "minimum_presented_fps":
            args.expected_fps * args.minimum_presented_fps_ratio,
    }
    report = {
        "ok": True,
        "sample_count": len(telemetry_samples),
        "telemetry_sample_count": len(telemetry_samples),
        "diagnostic_sample_count": len(diagnostic_samples),
        "playing_sample_count": len(playing_samples),
        "measurement_path": "lock_free_playback_telemetry",
        "classification": selected.reason,
        "details": selected.details,
        "presentation_cadence": cadence_report,
        "all_reasons": sorted({item.reason for item in classifications}),
        "display_probe": display_classification.details if display_classification else None,
    }
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
