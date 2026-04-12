#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Any, Dict, List, Sequence, Tuple

import cv2
import numpy as np
from scipy import signal


@dataclass
class DetectionResult:
    video_offset_frames: int
    confidence: float
    lag_frames: int
    backend: str
    local_offsets: List[Tuple[int, int, float]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Detect video/audio sync offset and emit sync markers for the editor."
    )
    parser.add_argument("--video", required=True, help="Path to video/visual media input.")
    parser.add_argument("--audio", required=True, help="Path to audio media input.")
    parser.add_argument("--fps", type=float, default=30.0, help="Timeline FPS.")
    parser.add_argument(
        "--interval-seconds",
        type=float,
        default=5.0,
        help="Analysis step cadence in seconds (default 5s).",
    )
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=10.0,
        help="Analysis window duration in seconds (default 10s for 50% overlap at 5s cadence).",
    )
    parser.add_argument(
        "--backend",
        default="auto",
        choices=["auto", "avcorr", "syncnet", "stub"],
        help="Detection backend.",
    )
    parser.add_argument(
        "--syncnet-model",
        default="",
        help="Path to SyncNet model/checkpoint (required for --backend syncnet).",
    )
    parser.add_argument(
        "--syncnet-device",
        default="auto",
        choices=["auto", "cpu", "cuda"],
        help="Device for syncnet backend.",
    )
    parser.add_argument(
        "--max-shift-seconds",
        type=float,
        default=1.0,
        help="Maximum absolute sync search window.",
    )
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.06,
        help="Minimum normalized confidence before applying non-zero offset.",
    )
    parser.add_argument(
        "--dry-run-offset",
        type=int,
        default=0,
        help="Manual frame offset for integration testing.",
    )
    parser.add_argument(
        "--progress",
        action="store_true",
        help="Emit periodic progress logs to stderr.",
    )
    parser.add_argument(
        "--progress-interval-seconds",
        type=float,
        default=2.0,
        help="Progress log cadence while long-running stages execute.",
    )
    return parser.parse_args()


def fail(message: str, code: int = 2) -> int:
    print(json.dumps({"error": message}, separators=(",", ":")))
    return code


def log_progress(args: argparse.Namespace, message: str) -> None:
    if not args.progress:
        return
    print(f"[sync] {message}", file=sys.stderr, flush=True)


def zscore(x: np.ndarray) -> np.ndarray:
    if x.size == 0:
        return x
    x = x.astype(np.float32, copy=False)
    mean = float(np.mean(x))
    std = float(np.std(x))
    if std < 1e-8:
        return x - mean
    return (x - mean) / std


def smooth(x: np.ndarray, width: int = 5) -> np.ndarray:
    if x.size == 0 or width <= 1:
        return x
    k = np.ones(width, dtype=np.float32) / float(width)
    return np.convolve(x, k, mode="same")


def extract_visual_activity(
    video_path: str,
    target_fps: float,
    args: argparse.Namespace,
) -> Tuple[np.ndarray, float]:
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        raise RuntimeError(f"unable to open video: {video_path}")

    native_fps = cap.get(cv2.CAP_PROP_FPS)
    if not native_fps or math.isnan(native_fps) or native_fps <= 0.0:
        native_fps = target_fps if target_fps > 0 else 30.0
    sample_every = max(1, int(round(native_fps / max(1.0, target_fps))))
    total_frames = int(cap.get(cv2.CAP_PROP_FRAME_COUNT) or 0)
    log_progress(
        args,
        f"visual extract start (native_fps={native_fps:.3f}, sample_every={sample_every}, total_frames={total_frames})",
    )

    activity: List[float] = []
    prev_gray: np.ndarray | None = None
    frame_index = 0
    last_log = time.monotonic()
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        if frame_index % sample_every != 0:
            frame_index += 1
            continue
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        if prev_gray is not None:
            diff = cv2.absdiff(gray, prev_gray)
            activity.append(float(np.mean(diff)))
        prev_gray = gray
        now = time.monotonic()
        if now - last_log >= max(0.2, args.progress_interval_seconds):
            if total_frames > 0:
                pct = min(100.0, (100.0 * frame_index) / float(total_frames))
                log_progress(args, f"visual extract {pct:.1f}% ({frame_index}/{total_frames})")
            else:
                log_progress(args, f"visual extract frame={frame_index}")
            last_log = now
        frame_index += 1
    cap.release()

    if not activity:
        raise RuntimeError("no visual activity frames extracted")
    fps_out = native_fps / sample_every
    log_progress(args, f"visual extract done (samples={len(activity)}, fps_out={fps_out:.3f})")
    return np.asarray(activity, dtype=np.float32), float(fps_out)


def extract_audio_activity(audio_path: str, fps: float, args: argparse.Namespace) -> np.ndarray:
    if fps <= 0.0:
        raise RuntimeError("invalid fps")
    sr = 16000
    cmd = [
        "ffmpeg",
        "-v",
        "error",
        "-i",
        audio_path,
        "-f",
        "s16le",
        "-ac",
        "1",
        "-ar",
        str(sr),
        "pipe:1",
    ]
    proc = subprocess.run(cmd, capture_output=True, check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg decode failed: {proc.stderr.decode('utf-8', errors='ignore').strip()}")
    y = np.frombuffer(proc.stdout, dtype=np.int16).astype(np.float32) / 32768.0
    if y.size == 0:
        raise RuntimeError("empty audio")
    hop_length = max(32, int(round(sr / fps)))
    frame_length = max(2 * hop_length, 256)
    if y.size < frame_length:
        y = np.pad(y, (0, frame_length - y.size))
    window = np.hanning(frame_length).astype(np.float32)
    values: List[float] = []
    for i in range(0, y.size - frame_length + 1, hop_length):
        frame = y[i : i + frame_length]
        values.append(float(np.sqrt(np.mean((frame * window) ** 2) + 1e-12)))
    env = np.asarray(values, dtype=np.float32)
    env = np.diff(env, prepend=env[0])
    env = np.maximum(env, 0.0)
    log_progress(args, f"audio extract done (samples={y.size}, env_frames={env.size})")
    return env


def detect_lag(
    audio: np.ndarray,
    visual: np.ndarray,
    max_lag: int,
    min_confidence: float,
) -> Tuple[int, float, int]:
    corr = signal.correlate(audio, visual, mode="full", method="fft")
    lags = signal.correlation_lags(audio.size, visual.size, mode="full")
    valid = np.abs(lags) <= max_lag
    if not np.any(valid):
        raise RuntimeError("no valid correlation window")
    corr_v = corr[valid]
    lags_v = lags[valid]

    best_idx = int(np.argmax(corr_v))
    best_lag = int(lags_v[best_idx])

    # lag definition in our scoring:
    # corr(audio[t], visual[t + lag]) peak => visual lags audio by +lag.
    # To keep audio fixed, move video earlier by lag => negative offset.
    offset = -best_lag

    energy = float(np.linalg.norm(audio) * np.linalg.norm(visual))
    peak = float(corr_v[best_idx])
    confidence = 0.0 if energy <= 1e-8 else max(0.0, min(1.0, peak / energy))
    if abs(offset) <= 1 or confidence < min_confidence:
        offset = 0
    return offset, confidence, best_lag


def detect_avcorr(
    video_path: str,
    audio_path: str,
    fps: float,
    interval_seconds: float,
    window_seconds: float,
    max_shift_seconds: float,
    min_confidence: float,
    args: argparse.Namespace,
) -> DetectionResult:
    visual, visual_fps = extract_visual_activity(video_path, fps, args)
    audio = extract_audio_activity(audio_path, visual_fps, args)

    n = min(len(visual), len(audio))
    if n < max(20, int(visual_fps * 1.5)):
        raise RuntimeError("not enough overlapping A/V samples for sync detection")

    visual = smooth(zscore(visual[:n]), width=5)
    audio = smooth(zscore(audio[:n]), width=5)

    max_lag = max(1, int(round(max_shift_seconds * visual_fps)))
    offset, confidence, best_lag = detect_lag(audio, visual, max_lag, min_confidence)

    step = max(1, int(round(interval_seconds * visual_fps)))
    window = max(2, int(round(window_seconds * visual_fps)))
    # Ensure overlap and enough context for stable local lag estimates.
    if window <= step:
        window = step * 2
    window = max(window, int(round(visual_fps * 2.0)))
    if window % 2 != 0:
        window += 1
    half = window // 2

    local_offsets: List[Tuple[int, int, float]] = []
    if n > window:
        total_centers = max(0, ((n - half) - half + step - 1) // step)
        processed_centers = 0
        last_log = time.monotonic()
        for center in range(half, n - half, step):
            seg_audio = audio[center - half : center + half]
            seg_visual = visual[center - half : center + half]
            try:
                local_offset, local_confidence, _ = detect_lag(seg_audio, seg_visual, max_lag, min_confidence)
            except Exception:
                processed_centers += 1
                continue
            timeline_frame = int(round(center * (fps / visual_fps)))
            local_offsets.append((timeline_frame, int(local_offset), float(local_confidence)))
            processed_centers += 1
            now = time.monotonic()
            if now - last_log >= max(0.2, args.progress_interval_seconds):
                if total_centers > 0:
                    pct = min(100.0, (100.0 * processed_centers) / float(total_centers))
                    log_progress(args, f"window analysis {pct:.1f}% ({processed_centers}/{total_centers})")
                else:
                    log_progress(args, f"window analysis processed={processed_centers}")
                last_log = now

    log_progress(
        args,
        f"global lag={best_lag} offset={offset} confidence={confidence:.3f} local_samples={len(local_offsets)}",
    )

    return DetectionResult(
        video_offset_frames=int(offset),
        confidence=confidence,
        lag_frames=best_lag,
        backend="avcorr",
        local_offsets=local_offsets,
    )


def build_sync_markers(
    video_offset_frames: int,
    local_offsets: Sequence[Tuple[int, int, float]],
) -> List[Dict[str, Any]]:
    if not local_offsets:
        return []

    frames = [int(item[0]) for item in local_offsets]
    offsets = np.asarray([int(item[1]) for item in local_offsets], dtype=np.float32)
    # Smooth noisy local estimates before converting to discrete events.
    smooth_offsets = signal.medfilt(offsets, kernel_size=5)
    smooth_offsets = np.rint(smooth_offsets).astype(np.int32)

    baseline = int(video_offset_frames)
    residual = smooth_offsets - baseline
    residual[np.abs(residual) <= 1] = 0

    markers: List[Dict[str, Any]] = []
    previous = 0
    for idx, frame in enumerate(frames):
        target = int(residual[idx])
        delta = target - previous
        if delta == 0:
            continue
        markers.append(
            {
                "frame": int(frame),
                "action": "duplicate" if delta > 0 else "skip",
                "count": int(max(1, min(abs(delta), 12))),
            }
        )
        previous = target
    return markers


def run_detection(args: argparse.Namespace) -> DetectionResult:
    if args.backend == "stub":
        return DetectionResult(
            video_offset_frames=0,
            confidence=0.0,
            lag_frames=0,
            backend="stub",
            local_offsets=[],
        )
    if args.backend == "syncnet":
        try:
            import torch  # type: ignore
        except Exception as exc:
            raise RuntimeError(f"syncnet backend requires torch ({exc})")
        if not args.syncnet_model:
            raise RuntimeError("syncnet backend requires --syncnet-model")
        if not os.path.exists(args.syncnet_model):
            raise RuntimeError(f"syncnet model not found: {args.syncnet_model}")
        device = args.syncnet_device
        if device == "auto":
            device = "cuda" if torch.cuda.is_available() else "cpu"
        # Placeholder until model inference is integrated. Keep explicit and safe.
        raise RuntimeError(
            f"syncnet backend selected (device={device}) but model inference is not yet integrated; "
            "use --backend avcorr for now"
        )
    return detect_avcorr(
        video_path=args.video,
        audio_path=args.audio,
        fps=args.fps,
        interval_seconds=args.interval_seconds,
        window_seconds=args.window_seconds,
        max_shift_seconds=args.max_shift_seconds,
        min_confidence=args.min_confidence,
        args=args,
    )


def main() -> int:
    args = parse_args()
    if not os.path.exists(args.video):
        return fail(f"video file not found: {args.video}")
    if not os.path.exists(args.audio):
        return fail(f"audio file not found: {args.audio}")
    if args.fps <= 0.0:
        return fail("fps must be > 0")
    if args.interval_seconds <= 0.0:
        return fail("interval-seconds must be > 0")
    if args.window_seconds <= 0.0:
        return fail("window-seconds must be > 0")
    if args.max_shift_seconds <= 0.0:
        return fail("max-shift-seconds must be > 0")

    try:
        log_progress(args, "sync detection started")
        result = run_detection(args)
    except Exception as exc:
        return fail(f"detector failed: {exc}")

    video_offset_frames = int(result.video_offset_frames) + int(args.dry_run_offset)
    markers = build_sync_markers(
        video_offset_frames=video_offset_frames,
        local_offsets=result.local_offsets,
    )

    payload: Dict[str, Any] = {
        "videoOffsetFrames": int(video_offset_frames),
        "markers": markers,
        "meta": {
            "backend": result.backend,
            "confidence": float(result.confidence),
            "lagFrames": int(result.lag_frames),
            "localSamples": int(len(result.local_offsets)),
        },
    }
    log_progress(args, "sync detection finished")
    print(json.dumps(payload, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
