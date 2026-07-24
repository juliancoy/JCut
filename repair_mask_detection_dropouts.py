#!/usr/bin/env python3
"""Repair isolated large detection dropouts in a JCut binary-mask sidecar.

The analysis is completed before any files are changed. A weak mask is repaired
from the nearest healthy original mask within the configured time window. The
repair is a pixelwise maximum, so detections already present in the weak frame
are preserved rather than replaced by the donor mask.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import shutil
import tempfile
from typing import Sequence

from PIL import Image, ImageChops


DEFAULT_WINDOW_SECONDS = 1.0
DEFAULT_WEAK_RATIO = 0.5
DEFAULT_HEALTHY_RATIO = 0.8
DEFAULT_MIN_REFERENCE_PIXELS = 64


@dataclass(frozen=True)
class Repair:
    target_ordinal: int
    donor_ordinal: int
    target_pixels: int
    donor_pixels: int
    reference_pixels: int


def read_json_object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"Cannot read valid JSON from {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"Expected a JSON object in {path}")
    return value


def sidecar_fps(sidecar: Path, completion: dict) -> float:
    metadata_name = completion.get("frame_index_metadata")
    metadata_path = sidecar / (
        str(metadata_name) if metadata_name else "jcut_frame_map.json"
    )
    metadata = read_json_object(metadata_path)
    for value in (
        completion.get("output_fps"),
        metadata.get("output_fps"),
        metadata.get("source_frame_rate"),
    ):
        try:
            fps = float(value)
        except (TypeError, ValueError):
            continue
        if math.isfinite(fps) and fps > 0:
            return fps
    raise ValueError(f"No positive frame rate found in {metadata_path}")


def validate_sidecar(sidecar: Path) -> tuple[dict, int, float]:
    completion_path = sidecar / "jcut_mask.json"
    completion = read_json_object(completion_path)
    if completion.get("schema") != "jcut_mask_sidecar_v1":
        raise ValueError(f"{completion_path} is not a JCut binary-mask sidecar")
    if completion.get("complete") is not True:
        raise ValueError(f"{completion_path} does not mark the sidecar complete")
    try:
        frame_count = int(completion["expected_frame_count"])
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError(
            f"{completion_path} has no valid expected_frame_count"
        ) from error
    if frame_count <= 0:
        raise ValueError(f"{completion_path} has an empty frame range")
    return completion, frame_count, sidecar_fps(sidecar, completion)


def frame_path(sidecar: Path, ordinal: int) -> Path:
    return sidecar / f"frame_{ordinal + 1:06d}.png"


def foreground_pixels(path: Path) -> int | None:
    if not path.is_file():
        return None
    try:
        with Image.open(path) as image:
            histogram = image.convert("L").histogram()
            return sum(histogram[1:])
    except (OSError, ValueError):
        return None


def scan_foreground_counts(
    sidecar: Path,
    frame_count: int,
    workers: int | None = None,
) -> list[int | None]:
    paths = (frame_path(sidecar, ordinal) for ordinal in range(frame_count))
    worker_count = workers or min(8, os.cpu_count() or 1)
    if worker_count == 1:
        return [foreground_pixels(path) for path in paths]
    with ProcessPoolExecutor(max_workers=worker_count) as executor:
        return list(executor.map(foreground_pixels, paths, chunksize=64))


def plan_repairs(
    counts: Sequence[int | None],
    window_frames: int,
    weak_ratio: float = DEFAULT_WEAK_RATIO,
    healthy_ratio: float = DEFAULT_HEALTHY_RATIO,
    min_reference_pixels: int = DEFAULT_MIN_REFERENCE_PIXELS,
) -> list[Repair]:
    if window_frames < 1:
        raise ValueError("window_frames must be positive")
    if not 0 <= weak_ratio < healthy_ratio <= 1:
        raise ValueError("ratios must satisfy 0 <= weak_ratio < healthy_ratio <= 1")

    repairs: list[Repair] = []
    for target, target_value in enumerate(counts):
        start = max(0, target - window_frames)
        stop = min(len(counts), target + window_frames + 1)
        reference = max(
            (value or 0 for value in counts[start:stop]),
            default=0,
        )
        target_pixels = target_value or 0
        if (
            reference < min_reference_pixels
            or target_pixels >= reference * weak_ratio
        ):
            continue

        healthy_minimum = reference * healthy_ratio
        candidates = [
            ordinal
            for ordinal in range(start, stop)
            if ordinal != target
            and counts[ordinal] is not None
            and counts[ordinal] >= healthy_minimum
        ]
        if not candidates:
            continue
        donor = min(
            candidates,
            key=lambda ordinal: (
                abs(ordinal - target),
                ordinal > target,
                ordinal,
            ),
        )
        repairs.append(
            Repair(
                target_ordinal=target,
                donor_ordinal=donor,
                target_pixels=target_pixels,
                donor_pixels=int(counts[donor] or 0),
                reference_pixels=reference,
            )
        )
    return repairs


def atomic_save_png(image: Image.Image, destination: Path) -> None:
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{destination.name}.",
            suffix=".tmp",
            dir=destination.parent,
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
        image.save(temporary_path, format="PNG")
        with temporary_path.open("rb") as saved:
            os.fsync(saved.fileno())
        os.replace(temporary_path, destination)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def merge_repair(target_path: Path, donor_path: Path) -> Image.Image:
    with Image.open(donor_path) as donor_source:
        donor = donor_source.convert("L")
        if target_path.is_file():
            with Image.open(target_path) as target_source:
                target = target_source.convert("L")
                if target.size != donor.size:
                    raise ValueError(
                        f"Mask size mismatch: {target_path} is {target.size}, "
                        f"{donor_path} is {donor.size}"
                    )
                return ImageChops.lighter(target, donor)
        return donor.copy()


def apply_repairs(
    sidecar: Path,
    repairs: Sequence[Repair],
    *,
    fps: float,
    window_seconds: float,
    weak_ratio: float,
    healthy_ratio: float,
    min_reference_pixels: int,
) -> Path | None:
    if not repairs:
        return None

    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    backup = sidecar / f".jcut_mask_dropout_backup_{timestamp}"
    backup.mkdir()
    manifest_repairs: list[dict] = []

    for repair in repairs:
        target = frame_path(sidecar, repair.target_ordinal)
        donor = frame_path(sidecar, repair.donor_ordinal)
        if target.exists():
            shutil.copy2(target, backup / target.name)
        merged = merge_repair(target, donor)
        atomic_save_png(merged, target)
        manifest_repairs.append(
            {
                "target_frame": repair.target_ordinal,
                "target_file": target.name,
                "donor_frame": repair.donor_ordinal,
                "donor_file": donor.name,
                "original_target_pixels": repair.target_pixels,
                "donor_pixels": repair.donor_pixels,
                "local_reference_pixels": repair.reference_pixels,
            }
        )

    manifest = {
        "schema": "jcut_mask_dropout_repair_v1",
        "completed_at_utc": datetime.now(timezone.utc).isoformat(),
        "preservation": (
            "Each repaired mask is the pixelwise maximum of the original target "
            "and donor. Existing target files are also copied byte-for-byte here."
        ),
        "sidecar": str(sidecar.resolve()),
        "fps": fps,
        "window_seconds": window_seconds,
        "weak_ratio": weak_ratio,
        "healthy_ratio": healthy_ratio,
        "min_reference_pixels": min_reference_pixels,
        "repair_count": len(manifest_repairs),
        "repairs": manifest_repairs,
    }
    manifest_path = backup / "repair_manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest_path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sidecar", type=Path, help="JCut binary-mask sidecar directory")
    parser.add_argument(
        "--window-seconds",
        type=float,
        default=DEFAULT_WINDOW_SECONDS,
        help="Maximum donor distance in either direction (default: 1.0)",
    )
    parser.add_argument(
        "--weak-ratio",
        type=float,
        default=DEFAULT_WEAK_RATIO,
        help="Repair masks below this fraction of the local maximum (default: 0.5)",
    )
    parser.add_argument(
        "--healthy-ratio",
        type=float,
        default=DEFAULT_HEALTHY_RATIO,
        help="Donors must reach this fraction of the local maximum (default: 0.8)",
    )
    parser.add_argument(
        "--min-reference-pixels",
        type=int,
        default=DEFAULT_MIN_REFERENCE_PIXELS,
        help="Ignore windows whose strongest mask is smaller than this (default: 64)",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Write repairs; without this option the script only reports its plan",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=min(8, os.cpu_count() or 1),
        help="Parallel mask scanners (default: up to 8)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    sidecar = args.sidecar.expanduser().resolve()
    if args.window_seconds <= 0 or not math.isfinite(args.window_seconds):
        raise ValueError("--window-seconds must be a positive finite number")
    if args.workers < 1:
        raise ValueError("--workers must be positive")

    completion, frame_count, fps = validate_sidecar(sidecar)
    window_frames = max(1, math.ceil(fps * args.window_seconds))
    print(
        f"Scanning {frame_count} masks for prompt "
        f"{completion.get('prompt', '<unknown>')!r} at {fps:.6f} fps..."
    )
    counts = scan_foreground_counts(sidecar, frame_count, args.workers)
    unreadable = sum(value is None for value in counts)
    repairs = plan_repairs(
        counts,
        window_frames,
        args.weak_ratio,
        args.healthy_ratio,
        args.min_reference_pixels,
    )
    print(
        f"Found {len(repairs)} repair candidate(s); "
        f"{unreadable} frame file(s) are missing or unreadable."
    )
    if not args.apply:
        print("Dry run only. Re-run with --apply to write repairs and backups.")
        return 0

    manifest = apply_repairs(
        sidecar,
        repairs,
        fps=fps,
        window_seconds=args.window_seconds,
        weak_ratio=args.weak_ratio,
        healthy_ratio=args.healthy_ratio,
        min_reference_pixels=args.min_reference_pixels,
    )
    if manifest is None:
        print("No files changed.")
    else:
        print(f"Applied {len(repairs)} repair(s). Manifest and backups: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
