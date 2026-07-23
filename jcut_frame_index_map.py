#!/usr/bin/env python3
"""Build and validate JCut source-frame to generated-mask-ordinal maps."""

from __future__ import annotations

import argparse
from collections import OrderedDict
import hashlib
from itertools import zip_longest
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from typing import Any


MAP_SCHEMA = "jcut_frame_index_map_v2"
SOURCE_IDENTITY_SCHEMA = "jcut_source_content_identity_v1"
SOURCE_VERSION_TOKEN_SCHEMA = "jcut_source_version_token_v1"
IDENTITY_HASH_BYTES = 1024 * 1024
SOURCE_HASH_CHUNK_BYTES = 4 * 1024 * 1024
SOURCE_IDENTITY_CACHE_ENTRIES = 8


# Whole-file hashing is needed only when creating an identity or when the
# source's local version token no longer matches the token recorded with the
# artifact. Keep the result process-local so several sidecars for one source do
# not each stream a large video after a move or metadata migration.
_SOURCE_CONTENT_HASH_CACHE: OrderedDict[tuple[str, ...], str] = OrderedDict()


def _positive_float(value: Any) -> float | None:
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    return parsed if math.isfinite(parsed) and parsed > 0.0 else None


def _positive_int(value: Any) -> int | None:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    return parsed if parsed > 0 else None


def _nonnegative_int(value: Any) -> int | None:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        return None
    return parsed if parsed >= 0 else None


def _rational_float(value: Any) -> float | None:
    text = str(value or "").strip()
    if "/" not in text:
        return _positive_float(text)
    numerator, denominator = text.split("/", 1)
    try:
        denominator_value = float(denominator)
        result = float(numerator) / denominator_value
    except (TypeError, ValueError, ZeroDivisionError):
        return None
    return result if math.isfinite(result) and result > 0.0 else None


def probe_source_stream(input_path: Path) -> dict[str, Any]:
    """Probe the facts used by DecoderContext to derive its frame-number key space."""
    probe = subprocess.run(
        [
            "ffprobe",
            "-hide_banner",
            "-loglevel",
            "error",
            "-threads",
            "0",
            "-select_streams",
            "v:0",
            "-show_entries",
            (
                "stream=index,codec_name,nb_frames,duration,start_time,time_base,"
                "r_frame_rate,avg_frame_rate:format=duration"
            ),
            "-of",
            "json",
            str(input_path),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if probe.returncode != 0:
        raise RuntimeError(f"ffprobe stream probe failed: {probe.stderr.strip()}")
    try:
        payload = json.loads(probe.stdout or "{}")
        streams = payload.get("streams") or []
    except json.JSONDecodeError as error:
        raise RuntimeError(f"ffprobe returned invalid stream JSON: {error}") from error
    if not streams:
        raise RuntimeError(f"No video stream found in: {input_path}")
    stream = dict(streams[0])
    stream["format_duration"] = (payload.get("format") or {}).get("duration")
    return stream


def source_frame_rate_for_index_map(input_path: Path) -> float | None:
    """Return DecoderContext's effective timestamp-to-frame conversion rate.

    DecoderContext prefers declared frame-count / duration, then FFmpeg's guessed
    frame rate. ffprobe exposes that guessed/base rate as ``r_frame_rate``; the
    average rate is only the final fallback for malformed containers.
    """
    stream = probe_source_stream(input_path)
    frames = _positive_int(stream.get("nb_frames"))
    duration = _positive_float(stream.get("duration")) or _positive_float(
        stream.get("format_duration")
    )
    if frames is not None and duration is not None:
        return frames / duration
    return _rational_float(stream.get("r_frame_rate")) or _rational_float(
        stream.get("avg_frame_rate")
    )


def _sha256_text(value: Any) -> str | None:
    text = str(value or "").strip().lower()
    if len(text) != 64:
        return None
    try:
        int(text, 16)
    except ValueError:
        return None
    return text


def _portable_content_hash(identity: dict[str, Any]) -> str | None:
    if identity.get("identity_schema") != SOURCE_IDENTITY_SCHEMA:
        return None
    return _sha256_text(identity.get("content_sha256"))


def _source_stat_fields(input_path: Path) -> dict[str, Any]:
    stat = input_path.stat()
    if not input_path.is_file() or stat.st_size < 0:
        raise RuntimeError(f"Source media is not a regular file: {input_path}")
    return {
        "size": stat.st_size,
        "mtime_ns": str(stat.st_mtime_ns),
        "ctime_ns": str(stat.st_ctime_ns),
        "device": str(stat.st_dev),
        "inode": str(stat.st_ino),
    }


def _stat_fields_match(left: dict[str, Any], right: dict[str, Any]) -> bool:
    left_size = _nonnegative_int(left.get("size"))
    right_size = _nonnegative_int(right.get("size"))
    return (
        left_size is not None
        and left_size == right_size
        and all(
            bool(str(left.get(key) or ""))
            and str(left.get(key)) == str(right.get(key) or "")
            for key in ("mtime_ns", "ctime_ns", "device", "inode")
        )
    )


def _source_version_token(input_path: Path) -> dict[str, Any]:
    """Return a cheap, local cache token; this is never the durable identity."""
    before = _source_stat_fields(input_path)
    digest = hashlib.sha256()
    middle_digest = hashlib.sha256()
    digest.update(str(before["size"]).encode("ascii"))
    digest.update(b"\0")
    middle_digest.update(str(before["size"]).encode("ascii"))
    middle_digest.update(b"\0")
    with input_path.open("rb") as source:
        digest.update(source.read(IDENTITY_HASH_BYTES))
        if before["size"] > IDENTITY_HASH_BYTES:
            source.seek(max(0, before["size"] - IDENTITY_HASH_BYTES))
            digest.update(source.read(IDENTITY_HASH_BYTES))
        middle_offset = max(0, (before["size"] - IDENTITY_HASH_BYTES) // 2)
        source.seek(middle_offset)
        middle_digest.update(source.read(IDENTITY_HASH_BYTES))
    after = _source_stat_fields(input_path)
    if not _stat_fields_match(before, after):
        raise RuntimeError(f"Source media changed while it was identified: {input_path}")
    return {
        "cache_token_schema": SOURCE_VERSION_TOKEN_SCHEMA,
        "path": str(input_path.resolve()),
        **before,
        "head_tail_sha256": digest.hexdigest(),
        "middle_sha256": middle_digest.hexdigest(),
        "hash_bytes_per_edge": IDENTITY_HASH_BYTES,
        "middle_hash_bytes": IDENTITY_HASH_BYTES,
    }


def _version_tokens_match(left: dict[str, Any], right: dict[str, Any]) -> bool:
    return (
        _stat_fields_match(left, right)
        and _sha256_text(left.get("head_tail_sha256"))
        == _sha256_text(right.get("head_tail_sha256"))
        and _sha256_text(left.get("head_tail_sha256")) is not None
        and _sha256_text(left.get("middle_sha256"))
        == _sha256_text(right.get("middle_sha256"))
        and _sha256_text(left.get("middle_sha256")) is not None
    )


def _source_cache_key(input_path: Path, token: dict[str, Any]) -> tuple[str, ...]:
    return (
        str(input_path.resolve()),
        *(str(token.get(key) or "") for key in (
            "size", "mtime_ns", "ctime_ns", "device", "inode",
            "head_tail_sha256", "middle_sha256",
        )),
    )


def _source_content_sha256(input_path: Path, token: dict[str, Any]) -> str:
    """Hash all source bytes and reject a concurrent source replacement."""
    before = _source_stat_fields(input_path)
    if not _stat_fields_match(token, before):
        raise RuntimeError(f"Source media changed while it was identified: {input_path}")
    digest = hashlib.sha256()
    with input_path.open("rb") as source:
        for chunk in iter(lambda: source.read(SOURCE_HASH_CHUNK_BYTES), b""):
            digest.update(chunk)
    after = _source_stat_fields(input_path)
    if not _stat_fields_match(before, after):
        raise RuntimeError(f"Source media changed while it was hashed: {input_path}")
    return digest.hexdigest()


def source_identity(
    input_path: Path,
    cached_identity: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Return a portable content identity with local verification-cache hints.

    ``content_sha256`` and ``size`` are the durable identity. The POSIX fields
    and sampled hashes are only a version token allowing an unchanged local
    file to reuse a previously verified whole-file digest without streaming it
    again. If that token changes (including after a copy or host move), the
    complete file is hashed before it can match.
    """
    token = _source_version_token(input_path)
    cached_hash = _portable_content_hash(cached_identity or {})
    if cached_hash is not None and _version_tokens_match(cached_identity or {}, token):
        content_hash = cached_hash
    else:
        cache_key = _source_cache_key(input_path, token)
        content_hash = _SOURCE_CONTENT_HASH_CACHE.get(cache_key)
        if content_hash is None:
            content_hash = _source_content_sha256(input_path, token)
            _SOURCE_CONTENT_HASH_CACHE[cache_key] = content_hash
            _SOURCE_CONTENT_HASH_CACHE.move_to_end(cache_key)
            while len(_SOURCE_CONTENT_HASH_CACHE) > SOURCE_IDENTITY_CACHE_ENTRIES:
                _SOURCE_CONTENT_HASH_CACHE.popitem(last=False)
        else:
            _SOURCE_CONTENT_HASH_CACHE.move_to_end(cache_key)
    return {
        **token,
        "identity_schema": SOURCE_IDENTITY_SCHEMA,
        "content_sha256": content_hash,
    }


def frame_index_map_metadata_path(map_path: Path) -> Path:
    return map_path.with_name("jcut_frame_map.json")


def _read_json_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{path.name}.",
            suffix=".tmp",
            dir=path.parent,
            mode="w",
            encoding="utf-8",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            json.dump(payload, output, indent=2, sort_keys=True)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, path)
        temporary_path = None
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def inspect_frame_index_map(map_path: Path) -> dict[str, int] | None:
    mapped_frames = 0
    min_source_frame: int | None = None
    max_source_frame: int | None = None
    max_mask_frame = -1
    previous_source_frame = -1
    previous_mask_frame = -1
    try:
        with map_path.open("r", encoding="utf-8") as source:
            for line in source:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                fields = stripped.split()
                if len(fields) < 2:
                    return None
                try:
                    source_frame = int(fields[0])
                    mask_frame = int(fields[1])
                except ValueError:
                    return None
                if source_frame < 0 or mask_frame != mapped_frames:
                    return None
                if mapped_frames > 0 and (
                    source_frame < previous_source_frame or mask_frame < previous_mask_frame
                ):
                    return None
                min_source_frame = (
                    source_frame if min_source_frame is None else min(min_source_frame, source_frame)
                )
                max_source_frame = (
                    source_frame if max_source_frame is None else max(max_source_frame, source_frame)
                )
                max_mask_frame = max(max_mask_frame, mask_frame)
                previous_source_frame = source_frame
                previous_mask_frame = mask_frame
                mapped_frames += 1
    except OSError:
        return None
    if mapped_frames == 0 or min_source_frame is None or max_source_frame is None:
        return None
    return {
        "mapped_frame_count": mapped_frames,
        "min_source_frame": min_source_frame,
        "max_source_frame": max_source_frame,
        "max_mask_frame": max_mask_frame,
        "expected_output_frame_count": max_mask_frame + 1,
    }


def _frame_index_pairs(map_path: Path):
    with map_path.open("r", encoding="utf-8") as source:
        for line in source:
            stripped = line.strip()
            if not stripped or stripped.startswith("#"):
                continue
            fields = stripped.split()
            if len(fields) < 2:
                raise RuntimeError(f"Invalid frame map row in: {map_path}")
            yield int(fields[0]), int(fields[1])


def _frame_index_map_matches_source(input_path: Path, map_path: Path) -> bool:
    """Regenerate DecoderContext's map and compare every source/ordinal pair."""
    with tempfile.TemporaryDirectory(
        prefix=".jcut-frame-map-verify-", dir=map_path.parent
    ) as temporary_directory:
        expected_path = Path(temporary_directory) / "jcut_frame_map.tsv"
        write_jcut_frame_index_map(input_path, expected_path)
        sentinel = object()
        return all(
            existing == expected
            for existing, expected in zip_longest(
                _frame_index_pairs(map_path),
                _frame_index_pairs(expected_path),
                fillvalue=sentinel,
            )
        )


def _same_output_fps(metadata_value: Any, output_fps: float | None) -> bool:
    if output_fps is None:
        return metadata_value is None
    parsed = _positive_float(metadata_value)
    return parsed is not None and abs(parsed - output_fps) <= max(1e-9, output_fps * 1e-9)


def _identity_matches(left: dict[str, Any], right: dict[str, Any]) -> bool:
    left_size = _nonnegative_int(left.get("size"))
    right_size = _nonnegative_int(right.get("size"))
    left_content_text = str(left.get("content_sha256") or "").strip()
    right_content_text = str(right.get("content_sha256") or "").strip()
    if any(
        str(identity.get("identity_schema") or "").strip()
        not in ("", SOURCE_IDENTITY_SCHEMA)
        for identity in (left, right)
    ):
        return False
    left_content_hash = _portable_content_hash(left)
    right_content_hash = _portable_content_hash(right)
    if (left_content_text and left_content_hash is None) or (
        right_content_text and right_content_hash is None
    ):
        return False
    if left_content_hash is not None and right_content_hash is not None:
        return left_size is not None and left_size == right_size and (
            left_content_hash == right_content_hash
        )
    # Compatibility for v2 metadata created before complete content hashes
    # were introduced, including a legacy completion manifest paired with an
    # upgraded map. It is intentionally local-token-only.
    return _version_tokens_match(left, right)


def source_identities_match(left: dict[str, Any], right: dict[str, Any]) -> bool:
    """Compare durable identities, with strict local legacy-v2 compatibility."""
    return _identity_matches(left, right)


def validated_frame_index_map_metadata(
    input_path: Path,
    map_path: Path,
    output_fps: float | None = None,
) -> dict[str, Any] | None:
    metadata = _read_json_object(frame_index_map_metadata_path(map_path))
    if metadata.get("schema") != MAP_SCHEMA or metadata.get("status") != "ready":
        return None
    if (
        metadata.get("frame_domain") != "source_timestamp_to_generated_ordinal"
        or metadata.get("map_file") != "jcut_frame_map.tsv"
    ):
        return None
    if not _same_output_fps(metadata.get("output_fps"), output_fps):
        return None
    expected_identity = metadata.get("source_identity") or {}
    if _portable_content_hash(expected_identity) is None:
        # A legacy identity is valid only on the original local file version;
        # reject a moved/mismatched token before doing an otherwise pointless
        # full-file hash, then hash once to upgrade an accepted artifact.
        current_token = _source_version_token(input_path)
        if not _version_tokens_match(expected_identity, current_token):
            return None
        actual_identity = source_identity(input_path)
    else:
        actual_identity = source_identity(input_path, expected_identity)
    if not _identity_matches(expected_identity, actual_identity):
        return None
    map_summary = inspect_frame_index_map(map_path)
    if map_summary is None:
        return None
    for key, value in map_summary.items():
        if int(metadata.get(key, -1)) != value:
            return None
    if str(metadata.get("map_sha256") or "") != _file_sha256(map_path):
        return None
    if _portable_content_hash(expected_identity) is None:
        # Safe in-place migration for current v2 files: the legacy token was
        # already proven to describe this exact local file and the map digest
        # and structure have now been checked. Completion manifests remain
        # compatible through their matching legacy token and map digest.
        metadata = dict(metadata)
        metadata["source_identity"] = actual_identity
        _atomic_write_json(frame_index_map_metadata_path(map_path), metadata)
    return metadata


def frame_index_map_is_populated(
    map_path: Path,
    input_path: Path | None = None,
    output_fps: float | None = None,
) -> bool:
    if input_path is None:
        return inspect_frame_index_map(map_path) is not None
    return validated_frame_index_map_metadata(input_path, map_path, output_fps) is not None


def _base_metadata(
    input_path: Path,
    output_fps: float | None,
    status: str,
) -> dict[str, Any]:
    stream = probe_source_stream(input_path)
    source_fps = source_frame_rate_for_index_map(input_path)
    return {
        "schema": MAP_SCHEMA,
        "status": status,
        "frame_domain": "source_timestamp_to_generated_ordinal",
        "source_identity": source_identity(input_path),
        "source_stream": stream,
        "source_frame_rate": source_fps,
        "output_fps": output_fps,
        "map_file": "jcut_frame_map.tsv",
    }


def adopt_existing_frame_index_map(
    input_path: Path,
    map_path: Path,
    output_fps: float | None = None,
) -> dict[str, Any]:
    """Explicitly bind a previously verified legacy map to its source."""
    if output_fps is not None:
        raise ValueError(
            "Downsampled frame maps are not valid timeline mask sidecars; "
            "adopt a full-rate map with one mask for every decoded source frame."
        )
    summary = inspect_frame_index_map(map_path)
    if summary is None:
        raise RuntimeError(f"Cannot adopt invalid or incomplete frame map: {map_path}")
    if not _frame_index_map_matches_source(input_path, map_path):
        raise RuntimeError(
            "Cannot adopt a frame map whose source-frame keys do not exactly "
            f"match the decoded source: {map_path}"
        )
    metadata = _base_metadata(input_path, output_fps, "ready")
    metadata.update(summary)
    metadata["map_sha256"] = _file_sha256(map_path)
    _atomic_write_json(frame_index_map_metadata_path(map_path), metadata)
    return metadata


def replicate_validated_frame_index_map(
    input_path: Path,
    source_map_path: Path,
    destination_map_path: Path,
    output_fps: float | None = None,
) -> dict[str, Any]:
    """Atomically reuse one validated source map for another mask sidecar."""
    source_metadata = validated_frame_index_map_metadata(
        input_path, source_map_path, output_fps
    )
    if source_metadata is None:
        raise RuntimeError(f"Source frame map is not validated: {source_map_path}")
    existing = validated_frame_index_map_metadata(
        input_path, destination_map_path, output_fps
    )
    if existing is not None:
        return existing
    destination_metadata_path = frame_index_map_metadata_path(destination_map_path)
    if destination_map_path.exists():
        if _file_sha256(destination_map_path) == source_metadata.get("map_sha256"):
            _atomic_write_json(destination_metadata_path, source_metadata)
            recovered = validated_frame_index_map_metadata(
                input_path, destination_map_path, output_fps
            )
            if recovered is not None:
                return recovered
        raise RuntimeError(
            f"Refusing to replace a different destination frame map: {destination_map_path}"
        )
    if destination_metadata_path.exists():
        raise RuntimeError(
            f"Refusing to replace orphaned destination metadata: {destination_metadata_path}"
        )
    destination_map_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{destination_map_path.name}.",
            suffix=".tmp",
            dir=destination_map_path.parent,
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            with source_map_path.open("rb") as source:
                shutil.copyfileobj(source, output)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, destination_map_path)
        temporary_path = None
        _atomic_write_json(destination_metadata_path, source_metadata)
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
    validated = validated_frame_index_map_metadata(
        input_path, destination_map_path, output_fps
    )
    if validated is None:
        raise RuntimeError(f"Replicated frame map failed validation: {destination_map_path}")
    return validated


def write_jcut_frame_index_map(
    input_path: Path,
    map_path: Path,
    output_fps: float | None = None,
) -> None:
    """Atomically map DecoderContext frame IDs to generated mask ordinals.

    Timeline mask sidecars contain every presentation-order decoded frame.
    Downsampled mask artifacts are deliberately unsupported: TIME.md requires
    an exact sidecar sample for the parent's presented source frame.
    """
    if output_fps is not None:
        raise ValueError(
            "Downsampled frame maps are not valid timeline mask sidecars; "
            "generate one mask for every decoded source frame."
        )
    if validated_frame_index_map_metadata(input_path, map_path, output_fps) is not None:
        return

    metadata_path = frame_index_map_metadata_path(map_path)
    existing_metadata = _read_json_object(metadata_path)
    current_identity = source_identity(
        input_path, existing_metadata.get("source_identity") or {}
    )
    if map_path.exists() and not existing_metadata:
        raise RuntimeError(
            f"Refusing to reuse or replace an unverified frame map: {map_path}. "
            "Adopt it explicitly after verifying the source, or use a new output directory."
        )
    if existing_metadata and not _identity_matches(
        existing_metadata.get("source_identity") or {}, current_identity
    ):
        raise RuntimeError(
            f"Frame-map source identity does not match {input_path}; use a new output directory."
        )
    if existing_metadata and not _same_output_fps(
        existing_metadata.get("output_fps"), output_fps
    ):
        raise RuntimeError(
            "Frame-map extraction FPS differs from this run; use a new output directory."
        )

    source_fps = source_frame_rate_for_index_map(input_path)
    if source_fps is None or source_fps <= 0.0:
        raise RuntimeError(f"Unable to determine source frame rate for: {input_path}")
    source_stream = probe_source_stream(input_path)
    time_base = _rational_float(source_stream.get("time_base"))
    if time_base is None:
        raise RuntimeError(f"Unable to determine source time base for: {input_path}")
    map_path.parent.mkdir(parents=True, exist_ok=True)
    building_metadata = _base_metadata(input_path, output_fps, "building")
    _atomic_write_json(metadata_path, building_metadata)

    probe = subprocess.Popen(
        [
            "ffprobe",
            "-hide_banner",
            "-loglevel",
            "error",
            "-threads",
            "0",
            "-select_streams",
            "v:0",
            "-show_frames",
            "-show_entries",
            "frame=best_effort_timestamp",
            "-of",
            "csv=p=0",
            str(input_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    temporary_path: Path | None = None
    decoded_ordinal = 0
    min_source_frame: int | None = None
    max_source_frame: int | None = None
    max_mask_frame = -1
    previous_source_frame: int | None = None
    try:
        with tempfile.NamedTemporaryFile(
            prefix=f".{map_path.name}.",
            suffix=".tmp",
            dir=map_path.parent,
            mode="w",
            encoding="utf-8",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            output.write("# source_frame\tmask_frame\n")
            if probe.stdout is not None:
                for line in probe.stdout:
                    value = line.strip().split(",", 1)[0]
                    if not value:
                        continue
                    try:
                        timestamp = int(value) * time_base
                    except ValueError as error:
                        raise RuntimeError(
                            f"ffprobe returned an invalid frame PTS: {value!r}"
                        ) from error
                    if not math.isfinite(timestamp):
                        raise RuntimeError("ffprobe returned a non-finite frame timestamp")
                    source_frame = int(timestamp * source_fps + 0.5)
                    if source_frame < 0:
                        raise RuntimeError(
                            "A decoded frame maps to a negative DecoderContext "
                            f"source-frame key ({source_frame}); refusing an ambiguous map."
                        )
                    if (
                        previous_source_frame is not None
                        and source_frame < previous_source_frame
                    ):
                        raise RuntimeError(
                            "Decoded source-frame keys are not nondecreasing: "
                            f"{source_frame} follows {previous_source_frame}."
                        )
                    mask_frame = decoded_ordinal
                    output.write(f"{source_frame}\t{mask_frame}\n")
                    min_source_frame = (
                        source_frame
                        if min_source_frame is None
                        else min(min_source_frame, source_frame)
                    )
                    max_source_frame = (
                        source_frame
                        if max_source_frame is None
                        else max(max_source_frame, source_frame)
                    )
                    max_mask_frame = max(max_mask_frame, mask_frame)
                    previous_source_frame = source_frame
                    decoded_ordinal += 1
            output.flush()
            os.fsync(output.fileno())
        _, stderr = probe.communicate()
        if probe.returncode != 0:
            raise RuntimeError(f"ffprobe frame index map failed: {stderr.strip()}")
        if decoded_ordinal == 0 or min_source_frame is None or max_source_frame is None:
            raise RuntimeError(f"ffprobe returned no decoded video frames for: {input_path}")
        os.replace(temporary_path, map_path)
        temporary_path = None
        ready_metadata = dict(building_metadata)
        ready_metadata.update(
            {
                "status": "ready",
                "mapped_frame_count": decoded_ordinal,
                "min_source_frame": min_source_frame,
                "max_source_frame": max_source_frame,
                "max_mask_frame": max_mask_frame,
                "expected_output_frame_count": max_mask_frame + 1,
            }
        )
        ready_metadata["map_sha256"] = _file_sha256(map_path)
        _atomic_write_json(metadata_path, ready_metadata)
    finally:
        if probe.poll() is None:
            probe.kill()
            probe.communicate()
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


def decoded_ordinal_for_source_frame(
    input_path: Path, requested_source_frame: int
) -> tuple[int, int]:
    """Resolve DecoderContext's requested frame key to its first decoded ordinal."""
    if requested_source_frame < 0:
        raise ValueError("requested_source_frame must be non-negative")
    source_fps = source_frame_rate_for_index_map(input_path)
    stream = probe_source_stream(input_path)
    time_base = _rational_float(stream.get("time_base"))
    if source_fps is None or time_base is None:
        raise RuntimeError(f"Unable to determine source timing for: {input_path}")
    probe = subprocess.Popen(
        [
            "ffprobe", "-hide_banner", "-loglevel", "error", "-threads", "0",
            "-select_streams", "v:0", "-show_frames",
            "-show_entries", "frame=best_effort_timestamp", "-of", "csv=p=0",
            str(input_path),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        previous_source_frame: int | None = None
        if probe.stdout is not None:
            for decoded_ordinal, line in enumerate(probe.stdout):
                value = line.strip().split(",", 1)[0]
                try:
                    pts = int(value)
                except ValueError as error:
                    raise RuntimeError(
                        f"ffprobe returned an invalid frame PTS: {value!r}"
                    ) from error
                source_frame = int((pts * time_base) * source_fps + 0.5)
                if source_frame < 0:
                    raise RuntimeError(
                        "A decoded frame maps to a negative DecoderContext "
                        f"source-frame key ({source_frame}); refusing an ambiguous lookup."
                    )
                if (
                    previous_source_frame is not None
                    and source_frame < previous_source_frame
                ):
                    raise RuntimeError(
                        "Decoded source-frame keys are not nondecreasing: "
                        f"{source_frame} follows {previous_source_frame}."
                    )
                previous_source_frame = source_frame
                if source_frame >= requested_source_frame:
                    # DecoderContext stops at the first presentation-order
                    # frame whose rounded PTS key reaches the request. Equal
                    # rounded keys are valid for VFR media, so first-match is
                    # the deterministic contract shared with runtime lookup.
                    return decoded_ordinal, source_frame
        _, stderr = probe.communicate()
        if probe.returncode != 0:
            raise RuntimeError(f"ffprobe source-frame lookup failed: {stderr.strip()}")
        raise RuntimeError(
            f"Source frame {requested_source_frame} is outside the decoded video range."
        )
    finally:
        if probe.poll() is None:
            probe.kill()
            probe.communicate()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Build a validated JCut source-frame to generated-mask map."
    )
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--output-fps",
        type=float,
        help="Map to ordinals from an fps-filtered frame extraction.",
    )
    parser.add_argument(
        "--adopt-existing",
        action="store_true",
        help="Bind an already verified legacy map to this source without rebuilding it.",
    )
    parser.add_argument(
        "--lookup-source-frame",
        type=int,
        help="Print the 1-based decoded ordinal used for a DecoderContext source-frame key.",
    )
    args = parser.parse_args()
    if args.lookup_source_frame is not None:
        ordinal, _ = decoded_ordinal_for_source_frame(
            args.input, args.lookup_source_frame
        )
        print(ordinal + 1)
        return 0
    if args.output is None:
        parser.error("--output is required unless --lookup-source-frame is used")
    if args.adopt_existing:
        adopt_existing_frame_index_map(args.input, args.output, args.output_fps)
    else:
        write_jcut_frame_index_map(args.input, args.output, args.output_fps)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
