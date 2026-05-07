#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple


def _now_stamp() -> str:
    return time.strftime("%Y%m%d-%H%M%S")


def _safe_float(v: Any, default: float = 0.0) -> float:
    try:
        return float(v)
    except Exception:
        return default


def _find_bbox_obj(obj: Any) -> Optional[Tuple[float, float, float]]:
    if not isinstance(obj, dict):
        return None

    if all(k in obj for k in ("x", "y", "box")):
        x = _safe_float(obj.get("x"), -1.0)
        y = _safe_float(obj.get("y"), -1.0)
        box = _safe_float(obj.get("box"), -1.0)
        if x >= 0.0 and y >= 0.0 and box > 0.0:
            return (x, y, box)

    keys = [
        ("left", "top", "right", "bottom"),
        ("x1", "y1", "x2", "y2"),
        ("xmin", "ymin", "xmax", "ymax"),
    ]
    for (lx, ty, rx, by) in keys:
        if all(k in obj for k in (lx, ty, rx, by)):
            left = _safe_float(obj.get(lx), -1.0)
            top = _safe_float(obj.get(ty), -1.0)
            right = _safe_float(obj.get(rx), -1.0)
            bottom = _safe_float(obj.get(by), -1.0)
            w = right - left
            h = bottom - top
            if left >= 0.0 and top >= 0.0 and w > 0.0 and h > 0.0:
                return (left + 0.5 * w, top + 0.5 * h, max(w, h))

    bbox = obj.get("bbox")
    if isinstance(bbox, list) and len(bbox) >= 4:
        left = _safe_float(bbox[0], -1.0)
        top = _safe_float(bbox[1], -1.0)
        w = _safe_float(bbox[2], -1.0)
        h = _safe_float(bbox[3], -1.0)
        if left >= 0.0 and top >= 0.0 and w > 0.0 and h > 0.0:
            return (left + 0.5 * w, top + 0.5 * h, max(w, h))

    return None


def _collect_bboxes_tree(node: Any, out: List[Tuple[float, float, float]]) -> None:
    if isinstance(node, dict):
        bbox = _find_bbox_obj(node)
        if bbox is not None:
            out.append(bbox)
        for v in node.values():
            _collect_bboxes_tree(v, out)
    elif isinstance(node, list):
        for v in node:
            _collect_bboxes_tree(v, out)


def _stats_from_tracks(tracks: List[Dict[str, Any]]) -> Dict[str, Any]:
    track_lengths: List[int] = []
    scores: List[float] = []
    frames: List[int] = []
    boxes: List[float] = []

    for t in tracks:
        dets = t.get("detections", [])
        if not isinstance(dets, list):
            dets = []
        track_lengths.append(len(dets))
        for d in dets:
            if isinstance(d, dict):
                scores.append(_safe_float(d.get("score"), 0.0))
                boxes.append(_safe_float(d.get("box"), 0.0))
                f = d.get("frame")
                try:
                    frames.append(int(f))
                except Exception:
                    pass

    det_count = sum(track_lengths)
    return {
        "track_count": len(tracks),
        "detection_count": det_count,
        "avg_track_len": (sum(track_lengths) / len(track_lengths)) if track_lengths else 0.0,
        "max_track_len": max(track_lengths) if track_lengths else 0,
        "median_track_len": statistics.median(track_lengths) if track_lengths else 0.0,
        "avg_score": (sum(scores) / len(scores)) if scores else 0.0,
        "avg_box": (sum(boxes) / len(boxes)) if boxes else 0.0,
        "first_frame": min(frames) if frames else None,
        "last_frame": max(frames) if frames else None,
    }


def _parse_tracks_from_json(path: Path) -> List[Dict[str, Any]]:
    if not path.exists():
        return []
    try:
        root = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return []

    if isinstance(root, dict) and isinstance(root.get("tracks"), list):
        return [t for t in root["tracks"] if isinstance(t, dict)]

    # Fallback: assemble one synthetic track from all bbox-like objects.
    bboxes: List[Tuple[float, float, float]] = []
    _collect_bboxes_tree(root, bboxes)
    if not bboxes:
        return []
    detections = []
    for i, (x, y, box) in enumerate(bboxes):
        detections.append({"frame": i, "x": x, "y": y, "box": box, "score": 1.0})
    return [{"track_id": 1, "detections": detections}]


@dataclass
class BenchCase:
    name: str
    cmd: List[str]
    output_json: Optional[Path]
    cwd: Path
    progress_json: Optional[Path] = None
    skip_reason: str = ""


def _format_eta_from_progress(progress: Dict[str, Any], elapsed: float) -> str:
    start = int(progress.get("start", 0))
    frame = int(progress.get("frame", start))
    end = int(progress.get("end", start))
    done = max(0, frame - start)
    total = max(1, end - start)
    if done <= 0 or elapsed <= 0:
        return "--:--"
    sec_per_frame = elapsed / float(done)
    eta = sec_per_frame * float(max(0, total - done))
    eta_i = max(0, int(round(eta)))
    return f"{eta_i // 60:02d}:{eta_i % 60:02d}"


def _run_case(case: BenchCase, env: Dict[str, str], live: bool, jsonl_path: Optional[Path]) -> Dict[str, Any]:
    if case.skip_reason:
        return {
            "name": case.name,
            "return_code": 0,
            "skipped": True,
            "skip_reason": case.skip_reason,
            "elapsed_sec": 0.0,
            "output_json": str(case.output_json) if case.output_json else "",
            "stdout": "",
            "stderr": case.skip_reason,
            "track_count": 0,
            "detection_count": 0,
            "avg_track_len": 0.0,
            "max_track_len": 0,
            "median_track_len": 0.0,
            "avg_score": 0.0,
            "avg_box": 0.0,
            "first_frame": None,
            "last_frame": None,
        }

    start = time.perf_counter()
    proc = subprocess.Popen(
        case.cmd,
        cwd=str(case.cwd),
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    live_progress: Dict[str, Any] = {}
    next_tick = 0.0
    while proc.poll() is None:
        now = time.perf_counter()
        elapsed = now - start
        if case.progress_json and case.progress_json.exists():
            try:
                live_progress = json.loads(case.progress_json.read_text(encoding="utf-8"))
            except Exception:
                live_progress = {}
        if live and now >= next_tick:
            if live_progress:
                frame = int(live_progress.get("frame", 0))
                end = int(live_progress.get("end", 0))
                det = int(live_progress.get("detections", 0))
                trk = int(live_progress.get("tracks", 0))
                eta = _format_eta_from_progress(live_progress, elapsed)
                print(
                    f"  [{case.name}] t={elapsed:6.1f}s frame={frame}/{end} det={det} tracks={trk} eta={eta}",
                    flush=True,
                )
            else:
                print(f"  [{case.name}] t={elapsed:6.1f}s running...", flush=True)
            next_tick = now + 0.5
        if jsonl_path:
            evt = {
                "ts": time.time(),
                "case": case.name,
                "event": "tick",
                "elapsed_sec": elapsed,
                "progress": live_progress,
            }
            with jsonl_path.open("a", encoding="utf-8") as jf:
                jf.write(json.dumps(evt) + "\n")
        time.sleep(0.1)

    stdout, stderr = proc.communicate()
    elapsed = time.perf_counter() - start

    tracks: List[Dict[str, Any]] = []
    if proc.returncode == 0 and case.output_json is not None:
        tracks = _parse_tracks_from_json(case.output_json)

    stats = _stats_from_tracks(tracks)
    return {
        "name": case.name,
        "return_code": proc.returncode,
        "elapsed_sec": elapsed,
        "output_json": str(case.output_json) if case.output_json else "",
        "stdout": stdout,
        "stderr": stderr,
        **stats,
    }


def _markdown_table(rows: List[Dict[str, Any]]) -> str:
    headers = [
        "Tracer",
        "Status",
        "Runtime(s)",
        "Tracks",
        "Detections",
        "AvgTrackLen",
        "MaxTrackLen",
        "AvgScore",
        "AvgBox",
        "FrameRange",
    ]
    out = ["| " + " | ".join(headers) + " |", "|" + "|".join(["---"] * len(headers)) + "|"]
    for r in rows:
        if r.get("skipped"):
            status = "SKIPPED"
        else:
            status = "OK" if r["return_code"] == 0 else f"ERR({r['return_code']})"
        fr = "-"
        if r.get("first_frame") is not None and r.get("last_frame") is not None:
            fr = f"{r['first_frame']}..{r['last_frame']}"
        out.append(
            "| "
            + " | ".join(
                [
                    r["name"],
                    status,
                    f"{r['elapsed_sec']:.2f}",
                    str(r["track_count"]),
                    str(r["detection_count"]),
                    f"{r['avg_track_len']:.2f}",
                    str(r["max_track_len"]),
                    f"{r['avg_score']:.3f}",
                    f"{r['avg_box']:.3f}",
                    fr,
                ]
            )
            + " |"
        )
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--video", required=True)
    ap.add_argument("--out-dir", default="build/face_tracer_bench")
    ap.add_argument("--start-frame", type=int, default=0)
    ap.add_argument("--end-frame", type=int, default=3600)
    ap.add_argument("--step", type=int, default=6)
    ap.add_argument("--source-fps", type=float, default=30.0)
    ap.add_argument("--max-candidates", type=int, default=128)
    ap.add_argument("--live", action="store_true", help="Show real-time progress output.")
    args = ap.parse_args()

    repo = Path(__file__).resolve().parent
    video = Path(args.video).resolve()
    if not video.exists():
        print(f"Video not found: {video}", file=sys.stderr)
        return 2

    out_root = (repo / args.out_dir / _now_stamp()).resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    jsonl_path = out_root / "benchmark_live.jsonl"

    env = dict(os.environ)
    env.setdefault("PYTHONUNBUFFERED", "1")

    docker_available = shutil.which("docker") is not None
    weights_dir = (repo / "external" / "opencv" / "samples" / "dnn" / "face_detector").resolve()

    cases: List[BenchCase] = []

    # Legacy Python candidate tracker.
    legacy_out = out_root / "legacy_python_haar.json"
    cases.append(
        BenchCase(
            name="legacy_python_haar",
            cmd=[
                sys.executable,
                str(repo / "legacy/speaker_face_candidates.py"),
                "--video",
                str(video),
                "--output-json",
                str(legacy_out),
                "--output-dir",
                str(out_root),
                "--start-frame",
                str(args.start_frame),
                "--end-frame",
                str(args.end_frame),
                "--step",
                str(max(1, args.step * 7)),
                "--source-fps",
                str(args.source_fps),
                "--max-candidates",
                str(max(8, args.max_candidates // 4)),
                "--crop-prefix",
                "legacy",
            ],
            output_json=legacy_out,
            cwd=repo,
            progress_json=None,
        )
    )

    # Local backend that has no external python deps beyond OpenCV.
    for backend in ["opencv_haar"]:
        out_json = out_root / f"docker_runner_{backend}.json"
        cases.append(
            BenchCase(
                name=f"docker_runner_{backend}",
                cmd=[
                    sys.executable,
                    str(repo / "legacy/docker_face_detector.py"),
                    "--backend",
                    backend,
                    "--video",
                    str(video),
                    "--output-json",
                    str(out_json),
                    "--output-dir",
                    str(out_root),
                    "--start-frame",
                    str(args.start_frame),
                    "--end-frame",
                    str(args.end_frame),
                    "--step",
                    str(args.step),
                    "--source-fps",
                    str(args.source_fps),
                    "--max-candidates",
                    str(args.max_candidates),
                    "--crop-prefix",
                    backend,
                ],
                output_json=out_json,
                cwd=repo,
                progress_json=out_root / f"docker_progress_{backend}.json",
            )
        )

    # Dockerized backends to avoid host dependency failures.
    docker_backends = [
        ("opencv_dnn", os.environ.get("JCUT_BOXSTREAM_IMAGE_OPENCV_DNN", "python:3.11-slim")),
        ("yolov8_face", os.environ.get("JCUT_BOXSTREAM_IMAGE_YOLOFACE", "ultralytics/ultralytics:latest")),
        ("insightface_retinaface", os.environ.get("JCUT_BOXSTREAM_IMAGE_INSIGHTFACE", "pytorch/pytorch:2.4.1-cuda12.1-cudnn9-runtime")),
        ("mtcnn", os.environ.get("JCUT_BOXSTREAM_IMAGE_MTCNN", "pytorch/pytorch:2.4.1-cuda12.1-cudnn9-runtime")),
    ]
    for backend, image in docker_backends:
        out_json = out_root / f"docker_runner_{backend}.json"
        progress_json = out_root / f"docker_progress_{backend}.json"
        if not docker_available:
            cases.append(
                BenchCase(
                    name=f"docker_runner_{backend}",
                    cmd=[],
                    output_json=out_json,
                    cwd=repo,
                    progress_json=progress_json,
                    skip_reason="docker not installed on host",
                )
            )
            continue
        media_mount = "/jcut_media"
        out_mount = "/jcut_out"
        runner_mount = "/jcut_runner"
        weights_mount = "/jcut_weights"
        in_container = f"{media_mount}/{video.name}"
        out_container = f"{out_mount}/{out_json.name}"
        progress_container = f"{out_mount}/{progress_json.name}"
        boot = "python3 -m pip install --no-cache-dir -q opencv-python-headless >/dev/null 2>&1; "
        if backend == "yolov8_face":
            boot += "python3 -m pip install --no-cache-dir -q ultralytics >/dev/null 2>&1; "
        elif backend == "insightface_retinaface":
            boot += "python3 -m pip install --no-cache-dir -q insightface onnxruntime-gpu >/dev/null 2>&1; "
        elif backend == "mtcnn":
            boot += "python3 -m pip install --no-cache-dir -q facenet-pytorch >/dev/null 2>&1; "
        run_py = (
            f"python3 {runner_mount}/docker_face_detector.py "
            f"--backend {backend} "
            f"--video {in_container} "
            f"--output-json {out_container} "
            f"--output-dir {out_mount} "
            f"--progress-json {progress_container} "
            f"--start-frame {args.start_frame} "
            f"--end-frame {args.end_frame} "
            f"--step {args.step} "
            f"--source-fps {args.source_fps} "
            f"--max-candidates {args.max_candidates} "
            f"--crop-prefix {backend}"
        )
        cmd = [
            "docker",
            "run",
            "--rm",
            "--gpus",
            os.environ.get("JCUT_BOXSTREAM_DOCKER_GPUS", "all"),
            "-v",
            f"{video.parent}:{media_mount}:ro",
            "-v",
            f"{out_root}:{out_mount}",
            "-v",
            f"{repo / 'legacy/docker_face_detector.py'}:{runner_mount}/docker_face_detector.py:ro",
            "-v",
            f"{weights_dir}:{weights_mount}:ro",
            "-e",
            f"JCUT_DNN_WEIGHTS_DIR={weights_mount}",
            image,
            "/bin/bash",
            "-lc",
            boot + run_py,
        ]
        cases.append(
            BenchCase(
                name=f"docker_runner_{backend}",
                cmd=cmd,
                output_json=out_json,
                cwd=repo,
                progress_json=progress_json,
            )
        )

    # SAM3 (best-effort, optional dependencies).
    sam3_out_dir = out_root / "sam3"
    sam3_out_dir.mkdir(parents=True, exist_ok=True)
    sam3_skip = ""
    if not (repo / "sam3").exists():
        sam3_skip = "sam3 runtime bundle missing at ./sam3"
    cases.append(
        BenchCase(
            name="sam3_face",
            cmd=[
                "/bin/bash",
                str(repo / "sam3.sh"),
                str(video),
                "--prompt",
                "face",
                "--out",
                str(sam3_out_dir),
            ],
            output_json=None,
            cwd=repo,
            progress_json=None,
            skip_reason=sam3_skip,
        )
    )

    results: List[Dict[str, Any]] = []
    for case in cases:
        print(f"[bench] running {case.name} ...", flush=True)
        # Ensure stale progress file is removed so live display always reflects current tracer.
        if case.progress_json and case.progress_json.exists():
            try:
                case.progress_json.unlink()
            except Exception:
                pass
        # Inject per-case progress file for docker-style runner.
        if case.progress_json:
            patched = []
            skip_next = False
            for i, token in enumerate(case.cmd):
                if skip_next:
                    skip_next = False
                    continue
                if token == "--progress-json" and i + 1 < len(case.cmd):
                    patched.extend([token, str(case.progress_json)])
                    skip_next = True
                    continue
                patched.append(token)
            if "--progress-json" not in patched and "docker_face_detector.py" in " ".join(patched):
                patched.extend(["--progress-json", str(case.progress_json)])
            case.cmd = patched
        r = _run_case(case, env, args.live, jsonl_path)

        # For SAM3, parse latest JSON file in out dir after run.
        if case.name == "sam3_face" and r["return_code"] == 0:
            json_files = sorted(sam3_out_dir.glob("*.json"), key=lambda p: p.stat().st_mtime, reverse=True)
            if json_files:
                tracks = _parse_tracks_from_json(json_files[0])
                stats = _stats_from_tracks(tracks)
                r.update(stats)
                r["output_json"] = str(json_files[0])

        results.append(r)

    report_json = out_root / "benchmark_results.json"
    report_md = out_root / "benchmark_report.md"
    report_raw = out_root / "benchmark_raw_logs.txt"

    report_json.write_text(json.dumps({"video": str(video), "results": results}, indent=2), encoding="utf-8")
    md = _markdown_table(results)
    report_md.write_text(md + "\n", encoding="utf-8")

    with report_raw.open("w", encoding="utf-8") as fh:
        for r in results:
            fh.write(f"===== {r['name']} =====\n")
            fh.write(f"return_code={r['return_code']} elapsed={r['elapsed_sec']:.3f}s\n")
            fh.write("--- stdout ---\n")
            fh.write(r.get("stdout", ""))
            fh.write("\n--- stderr ---\n")
            fh.write(r.get("stderr", ""))
            fh.write("\n\n")

    print("\nFace tracer benchmark complete.")
    print(f"Results JSON: {report_json}")
    print(f"Report MD:    {report_md}")
    print(f"Raw logs:     {report_raw}\n")
    print(f"Live stream:  {jsonl_path}\n")
    print(md)
    return 0


if __name__ == "__main__":
    sys.exit(main())
