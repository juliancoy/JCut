#!/usr/bin/env python3
"""Generate resumable continuous-alpha sidecars with BiRefNet.

The optional SAM guidance directory identifies the intended foreground.  It is
used as a generously dilated spatial gate, not as alpha, so BiRefNet remains
responsible for soft hair, motion-blur, and semi-transparent boundaries.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import tempfile
import time
import traceback
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

import cv2
import numpy as np
import torch
from PIL import Image
from torchvision import transforms
from transformers import AutoModelForImageSegmentation
from jcut_frame_index_map import (
    source_identity,
    source_identities_match,
    validated_frame_index_map_metadata,
)
from sam3_resume import image_file_looks_complete


MODEL_ID = "ZhengPeng7/BiRefNet-matting"
MODEL_REVISION = "57f9f68b43ba337c75762b14cf3075d659007268"
IMAGE_SIZE = (1024, 1024)
CUDA_OOM_EXIT_CODE = 42
HOST_OOM_EXIT_CODE = 43
ERROR_ARTIFACT_NAME = "jcut_error.json"
ERROR_STDERR_PREFIX = "JCUT_BIREFNET_ERROR_JSON="


@dataclass
class RunState:
    phase: str = "initialization"
    frame_index: int | None = None
    completed_frame: int = 0
    total_frames: int = 0
    rendered_frames: int = 0
    started_monotonic: float = 0.0
    render_started_monotonic: float | None = None
    device: str = "unresolved"


def atomic_write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        prefix=f".{path.stem}.", suffix=".json", dir=path.parent,
        mode="w", encoding="utf-8", delete=False,
    ) as handle:
        temporary = Path(handle.name)
        json.dump(payload, handle, indent=2)
        handle.write("\n")
        handle.flush()
        os.fsync(handle.fileno())
    try:
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def write_progress(
    args: argparse.Namespace,
    state: RunState,
    status: str,
    error: dict | None = None,
) -> None:
    if not args.progress_json:
        return
    elapsed = max(0.0, time.monotonic() - state.started_monotonic)
    render_elapsed = (
        max(0.0, time.monotonic() - state.render_started_monotonic)
        if state.render_started_monotonic is not None
        else 0.0
    )
    render_fps = (
        state.rendered_frames / render_elapsed
        if state.rendered_frames > 0 and render_elapsed > 0.0
        else 0.0
    )
    remaining = max(0, state.total_frames - state.completed_frame)
    payload = {
        "schema": "jcut_processing_progress_v1",
        "status": status,
        "phase": state.phase,
        "current_frame": state.completed_frame,
        "active_frame": state.frame_index,
        "total_frames": state.total_frames,
        "rendered_this_run": state.rendered_frames,
        "percent": (
            100.0 * state.completed_frame / state.total_frames
            if state.total_frames > 0 else None
        ),
        "elapsed_seconds": elapsed,
        "render_elapsed_seconds": render_elapsed,
        "render_fps": render_fps,
        "eta_seconds": remaining / render_fps if render_fps > 0.0 else None,
        "updated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
    }
    if error:
        payload["error"] = error
    atomic_write_json(Path(args.progress_json), payload)


def identities_match(left: dict, right: dict) -> bool:
    return source_identities_match(left, right)


def read_json_object(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}
    return value if isinstance(value, dict) else {}


def small_file_identity(path: Path) -> dict | None:
    try:
        content = path.read_bytes()
    except OSError:
        return None
    return {
        "name": path.name,
        "size": len(content),
        "sha256": hashlib.sha256(content).hexdigest(),
    }


def guidance_identity(directory: Path | None) -> dict | None:
    if directory is None:
        return None
    return {
        "path": str(directory.resolve()),
        "frame_map": small_file_identity(directory / "jcut_frame_map.tsv"),
        "frame_map_metadata": small_file_identity(directory / "jcut_frame_map.json"),
        "completion": (
            small_file_identity(directory / "jcut_mask.json")
            or small_file_identity(directory / "jcut_alpha.json")
        ),
    }


def prepare_resume_artifacts(
    output_dir: Path,
    expected_provenance: dict,
    resume: bool,
) -> None:
    manifest_path = output_dir / "jcut_alpha_run.json"
    frame_paths = []
    for path in output_dir.glob("frame_*.png"):
        try:
            index = int(path.stem.removeprefix("frame_"))
        except ValueError:
            continue
        if index > 0:
            frame_paths.append(path)
    existing = read_json_object(manifest_path)
    if resume and frame_paths and existing != expected_provenance:
        raise RuntimeError(
            f"Refusing to resume unverified or mismatched alpha frames in {output_dir}. "
            "Use --no-resume to start this generated sidecar again."
        )
    if not resume:
        for path in frame_paths:
            path.unlink()
    if existing != expected_provenance:
        atomic_write_json(manifest_path, expected_provenance)


def try_write_progress(
    args: argparse.Namespace,
    state: RunState,
    status: str,
    error: dict | None = None,
) -> None:
    try:
        write_progress(args, state, status, error)
    except Exception as progress_error:
        print(f"[birefnet] unable to write progress: {progress_error}",
              file=sys.stderr, flush=True)


def first_missing_frame(output_dir: Path, total_frames: int) -> int:
    completed: set[int] = set()
    for path in output_dir.glob("frame_*.png"):
        try:
            index = int(path.stem.removeprefix("frame_"))
            if index > 0 and image_file_looks_complete(path):
                completed.add(index)
        except (OSError, ValueError):
            continue
    frame = 1
    limit = total_frames if total_frames > 0 else max(completed, default=0)
    while frame <= limit and frame in completed:
        frame += 1
    return frame


def seek_to_frame(capture: cv2.VideoCapture, frame: int) -> int:
    """Seek to a 1-based frame, falling back to sequential resume if unsupported."""
    if frame <= 1:
        return 1
    target = frame - 1
    if capture.set(cv2.CAP_PROP_POS_FRAMES, target):
        actual = int(round(capture.get(cv2.CAP_PROP_POS_FRAMES)))
        if actual == target:
            print(f"[birefnet] resuming at frame {frame}", flush=True)
            return frame
    capture.set(cv2.CAP_PROP_POS_FRAMES, 0)
    print("[birefnet] decoder seek unavailable; scanning existing frames from frame 1",
          flush=True)
    return 1


def atomic_save_grayscale(path: Path, alpha: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image = Image.fromarray(alpha, mode="L")
    with tempfile.NamedTemporaryFile(
        prefix=f".{path.stem}.", suffix=".png", dir=path.parent, delete=False
    ) as handle:
        temporary = Path(handle.name)
    try:
        image.save(temporary, format="PNG", compress_level=4)
        with temporary.open("rb") as completed:
            os.fsync(completed.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def atomic_save_rgb(path: Path, rgb: np.ndarray) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    image = Image.fromarray(rgb, mode="RGB")
    with tempfile.NamedTemporaryFile(
        prefix=f".{path.stem}.", suffix=".png", dir=path.parent, delete=False
    ) as handle:
        temporary = Path(handle.name)
    try:
        image.save(temporary, format="PNG", compress_level=4)
        with temporary.open("rb") as completed:
            os.fsync(completed.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def live_preview_strip(rgb: np.ndarray, alpha_u8: np.ndarray, max_height: int = 320) -> np.ndarray:
    height, width = rgb.shape[:2]
    scale = min(1.0, max_height / max(1, height))
    preview_size = (max(1, int(round(width * scale))), max(1, int(round(height * scale))))
    source = cv2.resize(rgb, preview_size, interpolation=cv2.INTER_AREA)
    alpha = cv2.resize(alpha_u8, preview_size, interpolation=cv2.INTER_LINEAR)
    yy, xx = np.indices(alpha.shape)
    checker_cells = ((xx // 12) + (yy // 12)) % 2
    checker = np.where(checker_cells[..., None] == 0, 58, 92).astype(np.uint8)
    checker = np.repeat(checker, 3, axis=2)
    coverage = alpha.astype(np.float32)[..., None] / 255.0
    composite = np.rint(source * coverage + checker * (1.0 - coverage)).astype(np.uint8)
    alpha_rgb = np.repeat(alpha[..., None], 3, axis=2)
    divider = np.full((source.shape[0], 4, 3), 24, dtype=np.uint8)
    return np.concatenate((source, divider, alpha_rgb, divider, composite), axis=1)


def guided_alpha(alpha: np.ndarray, guidance_path: Path | None, gate_radius: int) -> np.ndarray:
    if guidance_path is None or not guidance_path.exists():
        return alpha
    guidance = cv2.imread(str(guidance_path), cv2.IMREAD_GRAYSCALE)
    if guidance is None:
        return alpha
    if guidance.shape != alpha.shape:
        guidance = cv2.resize(
            guidance, (alpha.shape[1], alpha.shape[0]), interpolation=cv2.INTER_NEAREST
        )
    binary = (guidance >= 128).astype(np.uint8)
    if gate_radius > 0:
        size = gate_radius * 2 + 1
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (size, size))
        binary = cv2.dilate(binary, kernel)
    return alpha * binary.astype(np.float32)


def apply_alpha_tolerance(alpha: np.ndarray, tolerance: float) -> np.ndarray:
    """Reject low-confidence foreground while retaining continuous soft alpha."""
    if tolerance <= 0.0:
        return alpha
    return np.clip((alpha - tolerance) / (1.0 - tolerance), 0.0, 1.0)


def write_metadata(
    output_dir: Path,
    args: argparse.Namespace,
    device: torch.device,
    frame_map_metadata: dict,
    expected_frame_count: int,
) -> None:
    metadata = {
        "schema": "jcut_alpha_sidecar_v1",
        "complete": True,
        "source_type": "birefnet_continuous_alpha",
        "model": args.model,
        "fp16": bool(args.fp16),
        "revision": args.revision,
        "input": str(Path(args.input).resolve()),
        "guidance_dir": str(Path(args.guidance_dir).resolve()) if args.guidance_dir else None,
        "frame_pattern": "frame_%06d.png",
        "frame_domain": "decode_ordinal",
        "frame_index_map": "jcut_frame_map.tsv",
        "frame_index_metadata": "jcut_frame_map.json",
        "frame_map_sha256": frame_map_metadata.get("map_sha256"),
        "expected_frame_count": expected_frame_count,
        "source_identity": frame_map_metadata.get("source_identity"),
        "alpha_encoding": "grayscale_u8_unorm",
        "continuous_alpha": True,
        "alpha_tolerance": args.alpha_tolerance,
        "device": str(device),
        "fp16": bool(args.fp16 and device.type == "cuda"),
    }
    atomic_write_json(output_dir / "jcut_alpha.json", metadata)


def verify_contiguous_alpha_frames(output_dir: Path, expected_frames: int) -> None:
    completed: set[int] = set()
    for path in output_dir.glob("frame_*.png"):
        try:
            index = int(path.stem.removeprefix("frame_"))
            if index > 0 and image_file_looks_complete(path):
                completed.add(index - 1)
        except (OSError, ValueError):
            continue
    if (
        expected_frames <= 0
        or len(completed) != expected_frames
        or min(completed, default=-1) != 0
        or max(completed, default=-1) != expected_frames - 1
    ):
        raise RuntimeError(
            f"Incomplete alpha sidecar: found {len(completed)} valid frame(s), "
            f"expected contiguous ordinals 0..{expected_frames - 1}."
        )


def cuda_memory_diagnostics() -> dict:
    if not torch.cuda.is_available():
        return {}
    diagnostics: dict[str, int | str] = {}
    try:
        diagnostics["device_name"] = torch.cuda.get_device_name()
        diagnostics["allocated_bytes"] = int(torch.cuda.memory_allocated())
        diagnostics["reserved_bytes"] = int(torch.cuda.memory_reserved())
        free_bytes, total_bytes = torch.cuda.mem_get_info()
        diagnostics["free_bytes"] = int(free_bytes)
        diagnostics["total_bytes"] = int(total_bytes)
    except Exception as diagnostic_error:  # Diagnostics must not hide the OOM.
        diagnostics["diagnostic_error"] = str(diagnostic_error)
    return diagnostics


def is_cuda_oom(error: BaseException) -> bool:
    if isinstance(error, torch.OutOfMemoryError):
        return True
    message = str(error).lower()
    return "out of memory" in message and ("cuda" in message or "cudnn" in message)


def failure_payload(
    args: argparse.Namespace,
    state: RunState,
    error: BaseException,
    kind: str,
) -> dict:
    payload = {
        "schema": "jcut_birefnet_error_v1",
        "kind": kind,
        "message": str(error) or error.__class__.__name__,
        "exception_type": error.__class__.__name__,
        "phase": state.phase,
        "frame_index": state.frame_index,
        "device": state.device,
        "model": args.model,
        "input_size": list(IMAGE_SIZE),
        "completed_frames_preserved": True,
    }
    if kind == "cuda_oom":
        payload["cuda_memory"] = cuda_memory_diagnostics()
        payload["retry_options"] = ["enable_fp16", "cpu", "lighter_model"]
    elif kind == "host_oom":
        payload["retry_options"] = ["close_other_applications", "cpu_or_lighter_model"]
    return payload


def report_failure(output_dir: Path, payload: dict) -> None:
    # stderr remains available when the output directory itself is unwritable.
    print(ERROR_STDERR_PREFIX + json.dumps(payload, separators=(",", ":")),
          file=sys.stderr, flush=True)
    try:
        atomic_write_json(output_dir / ERROR_ARTIFACT_NAME, payload)
    except Exception as artifact_error:
        print(f"[birefnet] unable to write error artifact: {artifact_error}",
              file=sys.stderr, flush=True)


def run(args: argparse.Namespace, state: RunState) -> None:
    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    guidance_dir = Path(args.guidance_dir) if args.guidance_dir else None
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / ERROR_ARTIFACT_NAME).unlink(missing_ok=True)
    frame_map_metadata: dict = {}
    expected_frame_count = 0
    initial_source_identity: dict = {}
    initial_guidance_identity: dict | None = None
    if args.frame_index is None:
        validated = validated_frame_index_map_metadata(
            input_path, output_dir / "jcut_frame_map.tsv"
        )
        if validated is None:
            raise RuntimeError(
                "Missing, incomplete, or source-mismatched jcut_frame_map metadata."
            )
        frame_map_metadata = validated
        expected_frame_count = int(validated.get("expected_output_frame_count") or 0)
        if expected_frame_count <= 0:
            raise RuntimeError("Frame-map metadata has no positive output-frame count.")
        initial_source_identity = source_identity(input_path)
        initial_guidance_identity = guidance_identity(guidance_dir)
    state.started_monotonic = time.monotonic()
    try_write_progress(args, state, "starting")

    state.phase = "device_initialization"
    if args.device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested but is not available")
    device = torch.device(
        "cuda" if args.device == "auto" and torch.cuda.is_available() else
        "cpu" if args.device == "auto" else args.device
    )
    state.device = str(device)
    dtype = torch.float16 if args.fp16 and device.type == "cuda" else torch.float32
    if args.frame_index is None:
        prepare_resume_artifacts(
            output_dir,
            {
                "schema": "jcut_birefnet_alpha_run_v1",
                "source_identity": frame_map_metadata.get("source_identity"),
                "frame_map_sha256": frame_map_metadata.get("map_sha256"),
                "expected_frame_count": expected_frame_count,
                "model": args.model,
                "revision": args.revision,
                "device": str(device),
                "fp16": bool(args.fp16 and device.type == "cuda"),
                "alpha_tolerance": args.alpha_tolerance,
                "guidance_gate_radius": args.guidance_gate_radius,
                "guidance_identity": initial_guidance_identity,
            },
            args.resume,
        )
        # Completion is transient availability, not enable intent. Withdraw it
        # only after the map and resume provenance are valid, immediately
        # before this run may mutate an alpha frame.
        (output_dir / "jcut_alpha.json").unlink(missing_ok=True)
    print(f"[birefnet] loading {args.model}@{args.revision} on {device} ({dtype})", flush=True)
    state.phase = "model_load"
    model = AutoModelForImageSegmentation.from_pretrained(
        args.model,
        revision=args.revision,
        trust_remote_code=True,
    ).to(device=device, dtype=dtype).eval()
    transform = transforms.Compose([
        transforms.Resize(IMAGE_SIZE),
        transforms.ToTensor(),
        transforms.Normalize([0.485, 0.456, 0.406], [0.229, 0.224, 0.225]),
    ])

    capture = cv2.VideoCapture(str(input_path))
    if not capture.isOpened():
        raise RuntimeError(f"Unable to open input video: {input_path}")
    decoder_total = int(capture.get(cv2.CAP_PROP_FRAME_COUNT))
    total = expected_frame_count if args.frame_index is None else max(0, decoder_total)
    state.total_frames = total
    frame_index = 1  # Matches ffmpeg's default frame_%06d numbering used by SAM3.
    if args.resume and args.frame_index is None:
        first_missing = first_missing_frame(output_dir, total)
        state.completed_frame = first_missing - 1
        # Decode from the beginning even on resume. Positional OpenCV seeks can
        # land on an adjacent decoded frame while reporting the requested
        # ordinal, which would permanently shift every resumed matte.
    processed = 0
    try_write_progress(args, state, "running")
    try:
        while True:
            ok, bgr = capture.read()
            if not ok:
                break
            if args.frame_index is not None and frame_index < args.frame_index:
                frame_index += 1
                continue
            output_path = output_dir / f"frame_{frame_index:06d}.png"
            if args.resume and image_file_looks_complete(output_path):
                state.phase = "resume_scan"
                state.frame_index = frame_index
                state.completed_frame = frame_index
                if frame_index == 1 or frame_index % args.progress_every == 0:
                    try_write_progress(args, state, "running")
                frame_index += 1
                continue
            state.phase = "frame_preprocess"
            state.frame_index = frame_index
            if state.render_started_monotonic is None:
                state.render_started_monotonic = time.monotonic()
            rgb = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB)
            if args.frame_index is not None:
                atomic_save_rgb(output_dir / "preview_source.png", rgb)
            height, width = rgb.shape[:2]
            tensor = transform(Image.fromarray(rgb)).unsqueeze(0).to(device=device, dtype=dtype)
            state.phase = "model_inference"
            with torch.inference_mode():
                prediction = model(tensor)[-1].sigmoid()[0, 0].float().cpu().numpy()
            state.phase = "frame_postprocess"
            alpha = cv2.resize(prediction, (width, height), interpolation=cv2.INTER_LINEAR)
            guidance_path = (
                guidance_dir / f"frame_{frame_index:06d}.png" if guidance_dir else None
            )
            alpha = guided_alpha(alpha, guidance_path, args.guidance_gate_radius)
            alpha = apply_alpha_tolerance(alpha, args.alpha_tolerance)
            alpha_u8 = np.rint(np.clip(alpha, 0.0, 1.0) * 255.0).astype(np.uint8)
            atomic_save_grayscale(output_path, alpha_u8)
            processed += 1
            state.rendered_frames = processed
            state.completed_frame = frame_index
            if args.live_preview and (
                processed == 1 or processed % args.live_preview_every == 0
            ):
                atomic_save_rgb(
                    output_dir / "jcut_live_preview.png",
                    live_preview_strip(rgb, alpha_u8),
                )
            if processed == 1 or processed % args.progress_every == 0:
                suffix = f"/{total}" if total > 0 else ""
                print(f"[birefnet] frame {frame_index}{suffix}", flush=True)
                state.phase = "frame_complete"
                try_write_progress(args, state, "running")
            frame_index += 1
            if args.frame_index is not None:
                break
    finally:
        capture.release()
    if frame_index == 1:
        raise RuntimeError(f"No video frames could be decoded from: {input_path}")
    if args.frame_index is not None and processed == 0:
        raise RuntimeError(
            f"Preview frame {args.frame_index} is outside the decoded video range"
        )
    if args.frame_index is None:
        decoded_frame_count = frame_index - 1
        if decoded_frame_count != expected_frame_count:
            raise RuntimeError(
                "Video decode ended before the validated frame-map boundary: "
                f"decoded {decoded_frame_count}, expected {expected_frame_count}."
            )
        verify_contiguous_alpha_frames(output_dir, expected_frame_count)
        revalidated = validated_frame_index_map_metadata(
            input_path, output_dir / "jcut_frame_map.tsv"
        )
        if (
            revalidated is None
            or revalidated.get("map_sha256") != frame_map_metadata.get("map_sha256")
            or not identities_match(initial_source_identity, source_identity(input_path))
            or guidance_identity(guidance_dir) != initial_guidance_identity
        ):
            raise RuntimeError(
                "Source media, frame map, or guidance changed while alpha masks were generated."
            )
        frame_map_metadata = revalidated
        state.phase = "metadata_write"
        write_metadata(
            output_dir,
            args,
            device,
            frame_map_metadata,
            expected_frame_count,
        )
    state.phase = "complete"
    state.frame_index = state.completed_frame
    try_write_progress(args, state, "completed")
    print(f"[birefnet] complete: {frame_index - 1} frames ({processed} rendered)", flush=True)


def main() -> int:
    args = parse_args()
    state = RunState()
    output_dir = Path(args.output_dir)
    try:
        run(args, state)
        return 0
    except MemoryError as error:
        failure = failure_payload(args, state, error, "host_oom")
        report_failure(output_dir, failure)
        try_write_progress(args, state, "failed", failure)
        traceback.print_exc()
        return HOST_OOM_EXIT_CODE
    except RuntimeError as error:
        if is_cuda_oom(error):
            failure = failure_payload(args, state, error, "cuda_oom")
            report_failure(output_dir, failure)
            try_write_progress(args, state, "failed", failure)
            traceback.print_exc()
            try:
                torch.cuda.empty_cache()
            except Exception:
                pass
            return CUDA_OOM_EXIT_CODE
        failure = failure_payload(args, state, error, "runtime_error")
        report_failure(output_dir, failure)
        try_write_progress(args, state, "failed", failure)
        traceback.print_exc()
        return 1
    except Exception as error:
        failure = failure_payload(args, state, error, "runtime_error")
        report_failure(output_dir, failure)
        try_write_progress(args, state, "failed", failure)
        traceback.print_exc()
        return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate continuous-alpha BiRefNet mask frames.")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--guidance-dir")
    parser.add_argument("--model", default=MODEL_ID)
    parser.add_argument("--revision", default=MODEL_REVISION)
    parser.add_argument("--device", choices=["auto", "cuda", "cpu"], default="auto")
    parser.add_argument("--fp16", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--guidance-gate-radius", type=int, default=24)
    parser.add_argument(
        "--alpha-tolerance", type=float, default=0.0,
        help=("Minimum foreground confidence in [0, 1). Values below it become "
              "transparent and the remaining continuous alpha is remapped."),
    )
    parser.add_argument("--progress-every", type=int, default=10)
    parser.add_argument("--progress-json",
                        help="Atomically update this JSON file with durable job progress.")
    parser.add_argument("--frame-index", type=int, default=None,
                        help="Render only this 1-based frame and write preview_source.png.")
    parser.add_argument("--live-preview", action="store_true",
                        help="Continuously refresh jcut_live_preview.png during full runs.")
    parser.add_argument("--live-preview-every", type=int, default=1,
                        help="Refresh the live preview after this many rendered frames.")
    args = parser.parse_args()
    if args.guidance_gate_radius < 0:
        parser.error("--guidance-gate-radius must be non-negative")
    if not 0.0 <= args.alpha_tolerance < 1.0:
        parser.error("--alpha-tolerance must be in [0, 1)")
    if args.progress_every < 1:
        parser.error("--progress-every must be positive")
    if args.frame_index is not None and args.frame_index < 1:
        parser.error("--frame-index must be positive")
    if args.live_preview_every < 1:
        parser.error("--live-preview-every must be positive")
    return args


if __name__ == "__main__":
    raise SystemExit(main())
