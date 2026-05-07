#!/usr/bin/env python3
import argparse
import json
import math
import os
import re
import sys


def clamp(value, low, high):
    return max(low, min(high, value))


def parse_args():
    parser = argparse.ArgumentParser(
        description="Speaker face autotrack from references with detector-assisted reacquisition"
    )
    parser.add_argument("--video", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--ref1-frame", type=int, required=True)
    parser.add_argument("--ref1-x", type=float, required=True)
    parser.add_argument("--ref1-y", type=float, required=True)
    parser.add_argument("--ref1-box", type=float, default=0.33)
    parser.add_argument("--ref2-frame", type=int, required=True)
    parser.add_argument("--ref2-x", type=float, required=True)
    parser.add_argument("--ref2-y", type=float, required=True)
    parser.add_argument("--ref2-box", type=float, default=0.33)
    parser.add_argument("--start-frame", type=int, default=None)
    parser.add_argument("--end-frame", type=int, default=None)
    parser.add_argument("--step", type=int, default=15)
    parser.add_argument("--prefer-gpu", action="store_true")
    parser.add_argument("--source-fps", type=float, default=0.0)

    # Optional quality controls (backward compatible defaults).
    parser.add_argument("--detector-interval", type=int, default=5)
    parser.add_argument("--min-track-confidence", type=float, default=0.20)
    parser.add_argument("--smoothing-alpha", type=float, default=0.35)
    return parser.parse_args()


def canonical_to_source_frame(frame30, source_fps):
    return max(0, int(math.floor((float(frame30) / 30.0) * source_fps)))


def natural_sort_key(name):
    parts = re.split(r"(\d+)", name)
    key = []
    for part in parts:
        if part.isdigit():
            key.append(int(part))
        else:
            key.append(part.lower())
    return key


def list_sequence_frames(path):
    if not os.path.isdir(path):
        return []
    allowed = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}
    frames = []
    for name in os.listdir(path):
        full = os.path.join(path, name)
        if not os.path.isfile(full):
            continue
        ext = os.path.splitext(name)[1].lower()
        if ext in allowed:
            frames.append(full)
    frames.sort(key=lambda p: natural_sort_key(os.path.basename(p)))
    return frames


class MediaReader:
    def __init__(self, mode, fps, cap=None, frame_paths=None):
        self.mode = mode
        self.fps = float(fps)
        self.cap = cap
        self.frame_paths = frame_paths or []

    def read(self, frame_index):
        import cv2

        idx = int(frame_index)
        if self.mode == "video":
            ok = self.cap.set(cv2.CAP_PROP_POS_FRAMES, idx)
            if not ok:
                return None
            ok, frame = self.cap.read()
            if not ok or frame is None:
                return None
            return frame
        if not self.frame_paths:
            return None
        idx = int(clamp(idx, 0, len(self.frame_paths) - 1))
        return cv2.imread(self.frame_paths[idx], cv2.IMREAD_COLOR)

    def close(self):
        if self.cap is not None:
            self.cap.release()
            self.cap = None


def open_media(path, fps_hint):
    import cv2

    if os.path.isdir(path):
        frame_paths = list_sequence_frames(path)
        if not frame_paths:
            raise RuntimeError(f"failed to open image sequence: {path}")
        fps = float(fps_hint) if fps_hint and fps_hint > 0 else 30.0
        return MediaReader("sequence", fps=fps, frame_paths=frame_paths), fps

    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise RuntimeError(f"failed to open video: {path}")
    fps = cap.get(cv2.CAP_PROP_FPS)
    if fps is None or fps <= 0:
        fps = float(fps_hint) if fps_hint and fps_hint > 0 else 30.0
    return MediaReader("video", fps=float(fps), cap=cap), float(fps)


def crop_square(frame, x_norm, y_norm, box_norm):
    h, w = frame.shape[:2]
    min_side = min(w, h)
    side = int(round(clamp(box_norm, 0.01, 1.0) * float(min_side)))
    side = clamp(side, 24, min_side)
    cx = int(round(clamp(x_norm, 0.0, 1.0) * float(w)))
    cy = int(round(clamp(y_norm, 0.0, 1.0) * float(h)))
    left = int(clamp(cx - side // 2, 0, max(0, w - side)))
    top = int(clamp(cy - side // 2, 0, max(0, h - side)))
    return frame[top : top + side, left : left + side].copy(), left, top, side


def track_one(frame, predicted_x, predicted_y, predicted_box, tmpl):
    import cv2

    if tmpl is None or tmpl.size == 0:
        return predicted_x, predicted_y, predicted_box, 0.0

    h, w = frame.shape[:2]
    min_side = min(w, h)
    side = int(round(clamp(predicted_box, 0.01, 1.0) * float(min_side)))
    side = clamp(side, max(24, tmpl.shape[0]), min_side)

    cx = int(round(clamp(predicted_x, 0.0, 1.0) * float(w)))
    cy = int(round(clamp(predicted_y, 0.0, 1.0) * float(h)))

    search_side = int(clamp(side * 2, side, min_side))
    left = int(clamp(cx - search_side // 2, 0, max(0, w - search_side)))
    top = int(clamp(cy - search_side // 2, 0, max(0, h - search_side)))
    search = frame[top : top + search_side, left : left + search_side]

    if search.shape[0] < tmpl.shape[0] or search.shape[1] < tmpl.shape[1]:
        return predicted_x, predicted_y, predicted_box, 0.0

    search_gray = cv2.cvtColor(search, cv2.COLOR_BGR2GRAY)
    tmpl_gray = cv2.cvtColor(tmpl, cv2.COLOR_BGR2GRAY)
    result = cv2.matchTemplate(search_gray, tmpl_gray, cv2.TM_CCOEFF_NORMED)
    _, max_val, _, max_loc = cv2.minMaxLoc(result)

    match_left = left + int(max_loc[0])
    match_top = top + int(max_loc[1])
    match_cx = match_left + tmpl.shape[1] // 2
    match_cy = match_top + tmpl.shape[0] // 2

    x_norm = clamp(float(match_cx) / float(w), 0.0, 1.0)
    y_norm = clamp(float(match_cy) / float(h), 0.0, 1.0)
    box_norm = clamp(float(tmpl.shape[0]) / float(min_side), 0.01, 1.0)
    return x_norm, y_norm, box_norm, float(max_val)


def track_one_gpu(frame, predicted_x, predicted_y, predicted_box, tmpl):
    import cv2

    if tmpl is None or tmpl.size == 0:
        return None
    try:
        h, w = frame.shape[:2]
        min_side = min(w, h)
        side = int(round(clamp(predicted_box, 0.01, 1.0) * float(min_side)))
        side = clamp(side, max(24, tmpl.shape[0]), min_side)

        cx = int(round(clamp(predicted_x, 0.0, 1.0) * float(w)))
        cy = int(round(clamp(predicted_y, 0.0, 1.0) * float(h)))

        search_side = int(clamp(side * 2, side, min_side))
        left = int(clamp(cx - search_side // 2, 0, max(0, w - search_side)))
        top = int(clamp(cy - search_side // 2, 0, max(0, h - search_side)))
        search = frame[top : top + search_side, left : left + search_side]
        if search.shape[0] < tmpl.shape[0] or search.shape[1] < tmpl.shape[1]:
            return None

        search_gray = cv2.cvtColor(search, cv2.COLOR_BGR2GRAY)
        tmpl_gray = cv2.cvtColor(tmpl, cv2.COLOR_BGR2GRAY)

        search_gpu = cv2.cuda_GpuMat()
        tmpl_gpu = cv2.cuda_GpuMat()
        search_gpu.upload(search_gray)
        tmpl_gpu.upload(tmpl_gray)

        matcher = cv2.cuda.createTemplateMatching(cv2.CV_8UC1, cv2.TM_CCOEFF_NORMED)
        result_gpu = matcher.match(search_gpu, tmpl_gpu)
        result = result_gpu.download()
        _, max_val, _, max_loc = cv2.minMaxLoc(result)

        match_left = left + int(max_loc[0])
        match_top = top + int(max_loc[1])
        match_cx = match_left + tmpl.shape[1] // 2
        match_cy = match_top + tmpl.shape[0] // 2

        x_norm = clamp(float(match_cx) / float(w), 0.0, 1.0)
        y_norm = clamp(float(match_cy) / float(h), 0.0, 1.0)
        box_norm = clamp(float(tmpl.shape[0]) / float(min_side), 0.01, 1.0)
        return x_norm, y_norm, box_norm, float(max_val)
    except Exception:
        return None


def init_face_detector():
    import cv2

    cascade_path = os.path.join(cv2.data.haarcascades, "haarcascade_frontalface_default.xml")
    if not os.path.exists(cascade_path):
        return None
    detector = cv2.CascadeClassifier(cascade_path)
    if detector.empty():
        return None
    return detector


def run_detector_candidate(frame, detector, predicted_x, predicted_y, predicted_box, tmpl1, tmpl2):
    import cv2

    if detector is None:
        return None

    h, w = frame.shape[:2]
    min_side = min(w, h)
    predicted_side = int(round(clamp(predicted_box, 0.01, 1.0) * float(min_side)))
    predicted_side = int(clamp(predicted_side, 24, min_side))

    cx = int(round(clamp(predicted_x, 0.0, 1.0) * float(w)))
    cy = int(round(clamp(predicted_y, 0.0, 1.0) * float(h)))

    search_side = int(clamp(predicted_side * 3, predicted_side, min_side))
    left = int(clamp(cx - search_side // 2, 0, max(0, w - search_side)))
    top = int(clamp(cy - search_side // 2, 0, max(0, h - search_side)))
    roi = frame[top : top + search_side, left : left + search_side]
    if roi.size == 0:
        return None
    gray = cv2.cvtColor(roi, cv2.COLOR_BGR2GRAY)

    detections = detector.detectMultiScale(
        gray,
        scaleFactor=1.1,
        minNeighbors=4,
        minSize=(24, 24),
    )
    if len(detections) == 0:
        return None

    best = None
    for (dx, dy, dw, dh) in detections:
        face_cx = left + (dx + (dw // 2))
        face_cy = top + (dy + (dh // 2))
        face_side = max(dw, dh)
        cand_x = clamp(float(face_cx) / float(w), 0.0, 1.0)
        cand_y = clamp(float(face_cy) / float(h), 0.0, 1.0)
        cand_b = clamp(float(face_side) / float(min_side), 0.01, 1.0)

        _, _, _, score1 = track_one(frame, cand_x, cand_y, cand_b, tmpl1)
        _, _, _, score2 = track_one(frame, cand_x, cand_y, cand_b, tmpl2)
        template_score = max(score1, score2)

        dist = math.sqrt(((cand_x - predicted_x) ** 2) + ((cand_y - predicted_y) ** 2))
        final_score = float(template_score - (0.25 * dist))
        if best is None or final_score > best["score"]:
            best = {
                "x": cand_x,
                "y": cand_y,
                "box": cand_b,
                "score": final_score,
                "confidence": float(clamp(template_score, 0.0, 1.0)),
            }
    return best


def main():
    args = parse_args()

    try:
        import cv2  # noqa: F401
        import numpy  # noqa: F401
    except Exception as exc:
        print(f"Missing dependency: {exc}", file=sys.stderr)
        return 2

    if args.step <= 0:
        args.step = 15
    args.detector_interval = int(clamp(args.detector_interval, 1, 300))
    args.min_track_confidence = float(clamp(args.min_track_confidence, 0.0, 1.0))
    args.smoothing_alpha = float(clamp(args.smoothing_alpha, 0.0, 1.0))

    reader = None
    try:
        reader, source_fps = open_media(args.video, args.source_fps)
        import cv2

        gpu_active = False
        gpu_failed = False
        if args.prefer_gpu:
            try:
                gpu_active = cv2.cuda.getCudaEnabledDeviceCount() > 0
            except Exception:
                gpu_active = False
        detector = init_face_detector()
        detector_hits = 0

        ref1_src = canonical_to_source_frame(args.ref1_frame, source_fps)
        ref2_src = canonical_to_source_frame(args.ref2_frame, source_fps)

        frame1 = reader.read(ref1_src)
        frame2 = reader.read(ref2_src)
        if frame1 is None or frame2 is None:
            print("Failed to decode one or both reference frames", file=sys.stderr)
            return 3

        tmpl1, _, _, _ = crop_square(frame1, args.ref1_x, args.ref1_y, args.ref1_box)
        tmpl2, _, _, _ = crop_square(frame2, args.ref2_x, args.ref2_y, args.ref2_box)

        ref_start = min(int(args.ref1_frame), int(args.ref2_frame))
        ref_end = max(int(args.ref1_frame), int(args.ref2_frame))
        start = int(args.start_frame) if args.start_frame is not None else ref_start
        end = int(args.end_frame) if args.end_frame is not None else ref_end
        if end < start:
            start, end = end, start
        span_ref = max(1, ref_end - ref_start)

        keyframes = []
        used_gpu_any = False
        last_x = None
        last_y = None
        last_b = None
        processed = 0

        for frame30 in range(start, end + 1, args.step):
            if last_x is None:
                t_ref = clamp(float(frame30 - ref_start) / float(span_ref), 0.0, 1.0)
                px = float(args.ref1_x) + (float(args.ref2_x) - float(args.ref1_x)) * t_ref
                py = float(args.ref1_y) + (float(args.ref2_y) - float(args.ref1_y)) * t_ref
                pb = float(args.ref1_box) + (float(args.ref2_box) - float(args.ref1_box)) * t_ref
            else:
                px = last_x
                py = last_y
                pb = last_b

            src_idx = canonical_to_source_frame(frame30, source_fps)
            frame = reader.read(src_idx)
            if frame is None:
                continue

            x1 = y1 = b1 = c1 = None
            x2 = y2 = b2 = c2 = None
            if gpu_active:
                r1 = track_one_gpu(frame, px, py, pb, tmpl1)
                r2 = track_one_gpu(frame, px, py, pb, tmpl2)
                if r1 is not None and r2 is not None:
                    x1, y1, b1, c1 = r1
                    x2, y2, b2, c2 = r2
                    used_gpu_any = True
                else:
                    gpu_active = False
                    gpu_failed = True
            if x1 is None or x2 is None:
                x1, y1, b1, c1 = track_one(frame, px, py, pb, tmpl1)
                x2, y2, b2, c2 = track_one(frame, px, py, pb, tmpl2)

            if c2 > c1:
                x, y, box, conf = x2, y2, b2, c2
            else:
                x, y, box, conf = x1, y1, b1, c1
            source_tag = "template"

            # Periodic detector-assisted reacquisition.
            should_detect = (
                detector is not None
                and (
                    (processed % args.detector_interval == 0)
                    or (conf < args.min_track_confidence)
                )
            )
            if should_detect:
                detected = run_detector_candidate(frame, detector, x, y, box, tmpl1, tmpl2)
                if detected is not None and detected["confidence"] >= max(conf + 0.03, args.min_track_confidence):
                    x = detected["x"]
                    y = detected["y"]
                    box = detected["box"]
                    conf = detected["confidence"]
                    source_tag = "detector"
                    detector_hits += 1

            # Low-confidence guardrail: damp movement to avoid jumps.
            if conf < args.min_track_confidence and last_x is not None and last_y is not None:
                x = (0.75 * last_x) + (0.25 * x)
                y = (0.75 * last_y) + (0.25 * y)
                box = (0.75 * last_b) + (0.25 * box)
                source_tag = "held"

            # Output-level temporal smoothing.
            if last_x is not None and args.smoothing_alpha > 0.0:
                alpha = args.smoothing_alpha
                x = (last_x * (1.0 - alpha)) + (x * alpha)
                y = (last_y * (1.0 - alpha)) + (y * alpha)
                box = (last_b * (1.0 - alpha)) + (box * alpha)

            x = float(clamp(x, 0.0, 1.0))
            y = float(clamp(y, 0.0, 1.0))
            box = float(clamp(box, 0.01, 1.0))
            conf = float(clamp(conf, 0.0, 1.0))

            last_x = x
            last_y = y
            last_b = box
            processed += 1

            mode_tag = "gpu" if used_gpu_any else "cpu"
            keyframes.append(
                {
                    "frame": int(frame30),
                    "x": x,
                    "y": y,
                    "box_size": box,
                    "confidence": conf,
                    "source": f"autotrack_docker_{mode_tag}_v2_{source_tag}",
                }
            )

        if keyframes and keyframes[-1]["frame"] != int(end):
            tail_x = float(clamp(last_x if last_x is not None else args.ref2_x, 0.0, 1.0))
            tail_y = float(clamp(last_y if last_y is not None else args.ref2_y, 0.0, 1.0))
            tail_b = float(clamp(last_b if last_b is not None else args.ref2_box, 0.01, 1.0))
            mode_tag = "gpu" if used_gpu_any else "cpu"
            keyframes.append(
                {
                    "frame": int(end),
                    "x": tail_x,
                    "y": tail_y,
                    "box_size": tail_b,
                    "confidence": 0.5,
                    "source": f"autotrack_docker_{mode_tag}_v2_tail",
                }
            )

        os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
        with open(args.output, "w", encoding="utf-8") as f:
            json.dump(keyframes, f, indent=2)

        mode = "gpu" if used_gpu_any else "cpu"
        if gpu_failed:
            print("GPU path failed; switched to CPU fallback", file=sys.stderr)
        print(
            f"wrote {len(keyframes)} keyframes to {args.output} "
            f"(mode={mode}, detector_hits={detector_hits}, detector={'on' if detector is not None else 'off'})"
        )
        return 0
    finally:
        if reader is not None:
            reader.close()


if __name__ == "__main__":
    sys.exit(main())
