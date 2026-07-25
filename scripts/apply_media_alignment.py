#!/usr/bin/env python3
"""Apply a sample-accurate, piecewise media-alignment plan to a JCut project.

The tool replaces one rate-1 clip with non-overlapping timeline/source segments.
Linked generated children and render-sync markers keep their original source
identity and are reassigned to the segment that now owns that source sample.

Dry-run is the default.  --apply refuses to run while the interactive editor is
alive, takes the project save lock, creates a byte-for-byte backup, and replaces
state.json atomically.
"""

from __future__ import annotations

import argparse
import copy
import datetime as dt
import fcntl
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile
from typing import Any
import uuid


AUDIO_SAMPLE_RATE = 48_000
TIMELINE_FPS = 30
SAMPLES_PER_FRAME = AUDIO_SAMPLE_RATE // TIMELINE_FPS


class AlignmentError(RuntimeError):
    pass


def qround_positive(value: float) -> int:
    return math.floor(value + 0.5)


def timeline_sample(clip: dict[str, Any]) -> int:
    return (
        int(clip.get("startFrame", 0)) * SAMPLES_PER_FRAME
        + int(clip.get("startSubframeSamples", 0))
    )


def duration_samples(clip: dict[str, Any]) -> int:
    return (
        int(clip.get("durationFrames", 0)) * SAMPLES_PER_FRAME
        + int(clip.get("durationSubframeSamples", 0))
    )


def source_frame_boundary(frame: int, source_fps: float) -> int:
    if source_fps <= 0:
        raise AlignmentError("sourceFps must be positive")
    return qround_positive((frame / source_fps) * AUDIO_SAMPLE_RATE)


def source_sample(clip: dict[str, Any]) -> int:
    source_fps = float(clip.get("sourceFps", TIMELINE_FPS))
    return source_frame_boundary(int(clip.get("sourceInFrame", 0)), source_fps) + int(
        clip.get("sourceInSubframeSamples", 0)
    )


def set_timeline_sample(clip: dict[str, Any], value: int) -> None:
    if value < 0:
        raise AlignmentError("timeline samples cannot be negative")
    clip["startFrame"], clip["startSubframeSamples"] = divmod(
        value, SAMPLES_PER_FRAME
    )


def set_duration_samples(clip: dict[str, Any], value: int) -> None:
    if value <= 0:
        raise AlignmentError("segment duration must be positive")
    clip["durationFrames"], clip["durationSubframeSamples"] = divmod(
        value, SAMPLES_PER_FRAME
    )


def set_source_sample(clip: dict[str, Any], value: int) -> None:
    if value < 0:
        raise AlignmentError("source samples cannot be negative")
    source_fps = float(clip.get("sourceFps", TIMELINE_FPS))
    frame = max(0, math.floor((value * source_fps) / AUDIO_SAMPLE_RATE))
    while frame > 0 and source_frame_boundary(frame, source_fps) > value:
        frame -= 1
    while source_frame_boundary(frame + 1, source_fps) <= value:
        frame += 1
    clip["sourceInFrame"] = frame
    clip["sourceInSubframeSamples"] = value - source_frame_boundary(
        frame, source_fps
    )


def _require_exact_original(
    clip: dict[str, Any], expected: dict[str, Any]
) -> None:
    mismatches = []
    for key, expected_value in expected.items():
        if clip.get(key) != expected_value:
            mismatches.append(
                f"{key}: expected {expected_value!r}, found {clip.get(key)!r}"
            )
    if mismatches:
        raise AlignmentError(
            "state no longer matches the reviewed alignment input: "
            + "; ".join(mismatches)
        )


def _normalized_segments(plan: dict[str, Any]) -> list[dict[str, Any]]:
    raw_segments = plan.get("segments")
    if not isinstance(raw_segments, list) or not raw_segments:
        raise AlignmentError("plan.segments must be a non-empty array")
    segments: list[dict[str, Any]] = []
    ids: set[str] = set()
    previous_timeline_end = -1
    previous_source_end = -1
    for index, raw in enumerate(raw_segments):
        if not isinstance(raw, dict):
            raise AlignmentError(f"segment {index + 1} must be an object")
        segment = {
            "id": str(raw.get("id", "")).strip(),
            "label": str(raw.get("label", "")).strip(),
            "timelineStartSample": int(raw.get("timelineStartSample", -1)),
            "sourceStartSample": int(raw.get("sourceStartSample", -1)),
            "durationSamples": int(raw.get("durationSamples", 0)),
        }
        if not segment["id"]:
            raise AlignmentError(f"segment {index + 1} has no id")
        if segment["id"] in ids:
            raise AlignmentError(f"duplicate segment id: {segment['id']}")
        ids.add(segment["id"])
        if (
            segment["timelineStartSample"] < 0
            or segment["sourceStartSample"] < 0
            or segment["durationSamples"] <= 0
        ):
            raise AlignmentError(f"segment {index + 1} has invalid sample bounds")
        timeline_end = (
            segment["timelineStartSample"] + segment["durationSamples"]
        )
        source_end = segment["sourceStartSample"] + segment["durationSamples"]
        if segment["timelineStartSample"] < previous_timeline_end:
            raise AlignmentError("alignment segments overlap on the timeline")
        if segment["sourceStartSample"] < previous_source_end:
            raise AlignmentError("alignment segments overlap in source media")
        segment["timelineEndSample"] = timeline_end
        segment["sourceEndSample"] = source_end
        segments.append(segment)
        previous_timeline_end = timeline_end
        previous_source_end = source_end
    return segments


def _segment_for_source_sample(
    segments: list[dict[str, Any]], value: int
) -> dict[str, Any] | None:
    for segment in segments:
        if segment["sourceStartSample"] <= value < segment["sourceEndSample"]:
            return segment
    if segments and value == segments[-1]["sourceEndSample"]:
        return segments[-1]
    return None


def _rebase_keyframes(
    keyframes: Any, local_start_sample: int, local_end_sample: int
) -> Any:
    if not isinstance(keyframes, list) or not keyframes:
        return keyframes
    ordered = sorted(
        (copy.deepcopy(value) for value in keyframes if isinstance(value, dict)),
        key=lambda value: int(value.get("frame", 0)),
    )
    if not ordered:
        return []
    rebased: list[dict[str, Any]] = []
    prior = None
    for keyframe in ordered:
        keyframe_sample = int(keyframe.get("frame", 0)) * SAMPLES_PER_FRAME
        if keyframe_sample <= local_start_sample:
            prior = keyframe
        if local_start_sample <= keyframe_sample < local_end_sample:
            keyframe["frame"] = max(
                0, (keyframe_sample - local_start_sample) // SAMPLES_PER_FRAME
            )
            rebased.append(keyframe)
    if prior is not None and (
        not rebased or int(rebased[0].get("frame", 0)) != 0
    ):
        prior["frame"] = 0
        rebased.insert(0, prior)
    deduplicated: dict[int, dict[str, Any]] = {}
    for keyframe in rebased:
        deduplicated[int(keyframe.get("frame", 0))] = keyframe
    return [deduplicated[frame] for frame in sorted(deduplicated)]


def _linked_child_pieces(
    child: dict[str, Any],
    original_timeline_start: int,
    original_source_start: int,
    segments: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    old_timeline_start = timeline_sample(child)
    old_duration = duration_samples(child)
    child_source_start = (
        original_source_start + old_timeline_start - original_timeline_start
    )
    child_source_end = child_source_start + old_duration
    pieces: list[dict[str, Any]] = []
    for segment in segments:
        intersection_start = max(
            child_source_start, segment["sourceStartSample"]
        )
        intersection_end = min(child_source_end, segment["sourceEndSample"])
        if intersection_start >= intersection_end:
            continue
        piece = copy.deepcopy(child)
        if pieces:
            piece["id"] = str(
                uuid.uuid5(
                    uuid.NAMESPACE_URL,
                    (
                        f"jcut-media-alignment:{child.get('id', '')}:"
                        f"{segment['id']}:{intersection_start}"
                    ),
                )
            )
        piece["linkedSourceClipId"] = segment["id"]
        set_timeline_sample(
            piece,
            segment["timelineStartSample"]
            + intersection_start
            - segment["sourceStartSample"],
        )
        set_duration_samples(piece, intersection_end - intersection_start)
        local_start = intersection_start - child_source_start
        local_end = intersection_end - child_source_start
        for key in (
            "transformKeyframes",
            "gradingKeyframes",
            "opacityKeyframes",
            "effectEnabledKeyframes",
            "titleKeyframes",
        ):
            piece[key] = _rebase_keyframes(
                piece.get(key), local_start, local_end
            )
        pieces.append(piece)
    if not pieces:
        raise AlignmentError(
            f"linked child {child.get('id', '')} is entirely in removed source intervals"
        )
    return pieces


def transform_state(
    state: dict[str, Any], plan: dict[str, Any]
) -> tuple[dict[str, Any], dict[str, Any]]:
    result = copy.deepcopy(state)
    timeline = result.get("timeline")
    if not isinstance(timeline, list):
        raise AlignmentError("state.timeline must be an array")

    clip_id = str(plan.get("clipId", "")).strip()
    if not clip_id:
        raise AlignmentError("plan.clipId is required")
    matches = [
        (index, clip)
        for index, clip in enumerate(timeline)
        if isinstance(clip, dict) and clip.get("id") == clip_id
    ]
    if len(matches) != 1:
        raise AlignmentError(
            f"expected exactly one clip {clip_id!r}, found {len(matches)}"
        )
    original_index, original = matches[0]
    if abs(float(original.get("playbackRate", 1.0)) - 1.0) > 1e-12:
        raise AlignmentError("piecewise alignment currently requires playbackRate 1")
    if any(
        original.get(key)
        for key in (
            "transformKeyframes",
            "gradingKeyframes",
            "opacityKeyframes",
            "effectEnabledKeyframes",
            "titleKeyframes",
            "correctionPolygons",
        )
    ):
        raise AlignmentError(
            "alignment source clip has creative keyframes; split it in the editor"
        )

    expected = plan.get("expectedOriginal", {})
    if not isinstance(expected, dict):
        raise AlignmentError("plan.expectedOriginal must be an object")
    _require_exact_original(original, expected)
    segments = _normalized_segments(plan)
    if segments[0]["id"] != clip_id:
        raise AlignmentError("the first segment must retain plan.clipId")

    original_timeline_start = timeline_sample(original)
    original_source_start = source_sample(original)
    original_timeline_end = original_timeline_start + duration_samples(original)
    if segments[0]["timelineStartSample"] != original_timeline_start:
        raise AlignmentError("the first segment must retain the original timeline start")
    preserve_timeline_end = bool(plan.get("preserveOriginalTimelineEnd", True))
    if (
        preserve_timeline_end
        and segments[-1]["timelineEndSample"] != original_timeline_end
    ):
        raise AlignmentError("segments must preserve the original timeline end")

    source_duration_limit = source_frame_boundary(
        int(original.get("sourceDurationFrames", 0)),
        float(original.get("sourceFps", TIMELINE_FPS)),
    )
    if segments[-1]["sourceEndSample"] > source_duration_limit:
        raise AlignmentError("alignment extends beyond source media duration")

    parent_clips: list[dict[str, Any]] = []
    for index, segment in enumerate(segments):
        parent = copy.deepcopy(original)
        parent["id"] = segment["id"]
        if segment["label"]:
            parent["label"] = segment["label"]
        set_timeline_sample(parent, segment["timelineStartSample"])
        set_source_sample(parent, segment["sourceStartSample"])
        set_duration_samples(parent, segment["durationSamples"])
        parent_clips.append(parent)

    timeline[original_index : original_index + 1] = parent_clips

    linked_count = 0
    linked_output_count = 0
    split_linked_child_count = 0
    linked_by_segment = {segment["id"]: 0 for segment in segments}
    transformed_timeline: list[Any] = []
    for child in timeline:
        if not isinstance(child, dict) or child.get("linkedSourceClipId") != clip_id:
            transformed_timeline.append(child)
            continue
        linked_count += 1
        pieces = _linked_child_pieces(
            child, original_timeline_start, original_source_start, segments
        )
        linked_output_count += len(pieces)
        if len(pieces) > 1:
            split_linked_child_count += 1
        for piece in pieces:
            linked_by_segment[piece["linkedSourceClipId"]] += 1
            transformed_timeline.append(piece)
    result["timeline"] = transformed_timeline

    markers = result.get("renderSyncMarkers", [])
    marker_count = 0
    if isinstance(markers, list):
        for marker in markers:
            if not isinstance(marker, dict) or marker.get("clipId") != clip_id:
                continue
            marker_count += 1
            marker_timeline = int(marker.get("frame", 0)) * SAMPLES_PER_FRAME
            marker_source = (
                original_source_start
                + marker_timeline
                - original_timeline_start
            )
            segment = _segment_for_source_sample(segments, marker_source)
            if segment is None:
                raise AlignmentError(
                    f"render-sync marker at frame {marker.get('frame')} "
                    "falls in a removed source interval"
                )
            mapped_timeline = (
                segment["timelineStartSample"]
                + marker_source
                - segment["sourceStartSample"]
            )
            marker["clipId"] = segment["id"]
            marker["frame"] = mapped_timeline // SAMPLES_PER_FRAME

    if isinstance(result.get("stateRevision"), int):
        result["stateRevision"] += 1

    summary = {
        "clipId": clip_id,
        "segmentCount": len(segments),
        "linkedChildCount": linked_count,
        "linkedOutputChildCount": linked_output_count,
        "splitLinkedChildCount": split_linked_child_count,
        "linkedChildrenBySegment": linked_by_segment,
        "renderSyncMarkerCount": marker_count,
        "segments": [
            {
                key: segment[key]
                for key in (
                    "id",
                    "timelineStartSample",
                    "timelineEndSample",
                    "sourceStartSample",
                    "sourceEndSample",
                    "durationSamples",
                )
            }
            for segment in segments
        ],
    }
    return result, summary


def running_editor_pid() -> int | None:
    lock_path = Path(tempfile.gettempdir()) / "PanelTalkEditor.lock"
    try:
        first_line = lock_path.read_text(encoding="utf-8").splitlines()[0]
        pid = int(first_line)
        os.kill(pid, 0)
    except (FileNotFoundError, IndexError, ValueError, ProcessLookupError):
        return None
    except PermissionError:
        return pid
    return pid


def _write_atomic(path: Path, payload: bytes) -> None:
    mode = path.stat().st_mode
    with tempfile.NamedTemporaryFile(
        mode="wb", prefix=f".{path.name}.", suffix=".tmp", dir=path.parent, delete=False
    ) as handle:
        temporary_path = Path(handle.name)
        try:
            handle.write(payload)
            handle.flush()
            os.fsync(handle.fileno())
            os.chmod(temporary_path, mode)
        except Exception:
            temporary_path.unlink(missing_ok=True)
            raise
    os.replace(temporary_path, path)
    directory_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(directory_fd)
    finally:
        os.close(directory_fd)


def apply_state_file(
    state_path: Path, plan: dict[str, Any], backup_path: Path | None = None
) -> tuple[dict[str, Any], Path]:
    pid = running_editor_pid()
    if pid is not None:
        raise AlignmentError(
            f"interactive JCut process {pid} is running; stop it before applying alignment"
        )
    lock_path = state_path.parent / ".jcut-project-save.lock"
    lock_path.touch(mode=0o600, exist_ok=True)
    with lock_path.open("r+b") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as exc:
            raise AlignmentError("project save lock is busy") from exc
        raw_state = state_path.read_bytes()
        state = json.loads(raw_state)
        transformed, summary = transform_state(state, plan)
        if backup_path is None:
            stamp = dt.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            backup_path = state_path.with_name(
                f"{state_path.stem}.before-media-alignment-{stamp}{state_path.suffix}"
            )
        if backup_path.exists():
            raise AlignmentError(f"backup already exists: {backup_path}")
        shutil.copy2(state_path, backup_path)
        payload = (
            json.dumps(transformed, ensure_ascii=False, indent=4) + "\n"
        ).encode("utf-8")
        _write_atomic(state_path, payload)
        summary["backupPath"] = str(backup_path)
        summary["statePath"] = str(state_path)
        return summary, backup_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply a sample-accurate JCut media-alignment plan."
    )
    parser.add_argument("state", type=Path)
    parser.add_argument("plan", type=Path)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="write the transformed state atomically; default is dry-run",
    )
    parser.add_argument("--backup", type=Path)
    args = parser.parse_args()

    try:
        plan = json.loads(args.plan.read_text(encoding="utf-8"))
        if args.apply:
            summary, _ = apply_state_file(args.state, plan, args.backup)
        else:
            state = json.loads(args.state.read_text(encoding="utf-8"))
            _, summary = transform_state(state, plan)
            summary["dryRun"] = True
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0
    except (AlignmentError, OSError, json.JSONDecodeError) as exc:
        print(f"media alignment failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
