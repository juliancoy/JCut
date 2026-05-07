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
        description="Sample a clip and detect potential face crops for speaker assignment."
    )
    parser.add_argument("--video", required=True)
    parser.add_argument("--output-json", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--start-frame", type=int, required=True)
    parser.add_argument("--end-frame", type=int, required=True)
    parser.add_argument("--step", type=int, default=45)
    parser.add_argument("--source-fps", type=float, default=30.0)
    parser.add_argument("--max-candidates", type=int, default=24)
    parser.add_argument("--crop-prefix", default="candidate")
    return parser.parse_args()


def canonical_to_source_frame(frame30, source_fps):
    fps = float(source_fps) if source_fps and source_fps > 0 else 30.0
    return max(0, int(math.floor((float(frame30) / 30.0) * fps)))


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


def init_face_detector():
    import cv2

    cascade_path = os.path.join(cv2.data.haarcascades, "haarcascade_frontalface_default.xml")
    if not os.path.exists(cascade_path):
        return None
    detector = cv2.CascadeClassifier(cascade_path)
    if detector.empty():
        return None
    return detector


def crop_square(frame, x_norm, y_norm, box_norm):
    h, w = frame.shape[:2]
    min_side = min(w, h)
    side = int(round(clamp(box_norm, 0.01, 1.0) * float(min_side)))
    side = int(clamp(side, 24, min_side))
    cx = int(round(clamp(x_norm, 0.0, 1.0) * float(w)))
    cy = int(round(clamp(y_norm, 0.0, 1.0) * float(h)))
    left = int(clamp(cx - side // 2, 0, max(0, w - side)))
    top = int(clamp(cy - side // 2, 0, max(0, h - side)))
    crop = frame[top : top + side, left : left + side].copy()
    return crop


def merge_or_append(candidates, new_item, frame_window):
    for idx, item in enumerate(candidates):
        if (
            abs(item["x"] - new_item["x"]) <= 0.06
            and abs(item["y"] - new_item["y"]) <= 0.06
            and abs(item["box"] - new_item["box"]) <= 0.08
            and abs(item["frame"] - new_item["frame"]) <= frame_window
        ):
            if new_item["score"] > item["score"]:
                candidates[idx] = new_item
            return
    candidates.append(new_item)


def iou_xywh(a, b):
    ax1 = a["x"] - (a["box"] / 2.0)
    ay1 = a["y"] - (a["box"] / 2.0)
    ax2 = a["x"] + (a["box"] / 2.0)
    ay2 = a["y"] + (a["box"] / 2.0)
    bx1 = b["x"] - (b["box"] / 2.0)
    by1 = b["y"] - (b["box"] / 2.0)
    bx2 = b["x"] + (b["box"] / 2.0)
    by2 = b["y"] + (b["box"] / 2.0)
    ix1 = max(ax1, bx1)
    iy1 = max(ay1, by1)
    ix2 = min(ax2, bx2)
    iy2 = min(ay2, by2)
    iw = max(0.0, ix2 - ix1)
    ih = max(0.0, iy2 - iy1)
    inter = iw * ih
    area_a = max(1e-6, (ax2 - ax1) * (ay2 - ay1))
    area_b = max(1e-6, (bx2 - bx1) * (by2 - by1))
    return inter / max(1e-6, area_a + area_b - inter)


def main():
    args = parse_args()
    os.makedirs(args.output_dir, exist_ok=True)

    try:
        import cv2
    except Exception as exc:
        sys.stderr.write(f"OpenCV import failed: {exc}\n")
        return 1

    detector = init_face_detector()
    if detector is None:
        sys.stderr.write("OpenCV Haar face detector was not found.\n")
        return 1

    try:
        reader, source_fps = open_media(args.video, args.source_fps)
    except Exception as exc:
        sys.stderr.write(f"{exc}\n")
        return 1

    start = int(max(0, min(args.start_frame, args.end_frame)))
    end = int(max(args.start_frame, args.end_frame))
    step = int(max(1, args.step))
    frame_window = int(max(step * 2, 60))
    max_track_gap = max(step * 2, 90)
    next_track_id = 1
    tracks = []
    active_tracks = []
    try:
        for frame30 in range(start, end + 1, step):
            src_frame = canonical_to_source_frame(frame30, source_fps)
            frame = reader.read(src_frame)
            if frame is None:
                continue
            h, w = frame.shape[:2]
            if w <= 0 or h <= 0:
                continue
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            detections = detector.detectMultiScale(
                gray,
                scaleFactor=1.1,
                minNeighbors=4,
                minSize=(28, 28),
            )
            min_side = float(min(w, h))
            for (x, y, fw, fh) in detections:
                side = float(max(fw, fh))
                cx = float(x) + (float(fw) / 2.0)
                cy = float(y) + (float(fh) / 2.0)
                det = {
                    "frame": int(frame30),
                    "x": clamp(cx / float(w), 0.0, 1.0),
                    "y": clamp(cy / float(h), 0.0, 1.0),
                    "box": clamp(side / min_side, 0.01, 1.0),
                    "score": clamp(side / min_side, 0.0, 1.0),
                }
                best_track = None
                best_score = -1.0
                for t in active_tracks:
                    if frame30 - t["last_frame"] > max_track_gap:
                        continue
                    anchor = t["last_detection"]
                    iou = iou_xywh(anchor, det)
                    center_dist = abs(anchor["x"] - det["x"]) + abs(anchor["y"] - det["y"])
                    score = iou - (0.8 * center_dist)
                    if score > best_score:
                        best_score = score
                        best_track = t
                if best_track is None or best_score < 0.05:
                    best_track = {
                        "track_id": next_track_id,
                        "start_frame": int(frame30),
                        "end_frame": int(frame30),
                        "last_frame": int(frame30),
                        "last_detection": dict(det),
                        "detections": [],
                    }
                    next_track_id += 1
                    tracks.append(best_track)
                    active_tracks.append(best_track)
                best_track["detections"].append(dict(det))
                best_track["end_frame"] = int(frame30)
                best_track["last_frame"] = int(frame30)
                best_track["last_detection"] = dict(det)
    finally:
        reader.close()

    for t in tracks:
        detections = t.get("detections", [])
        if not detections:
            continue
        best = max(detections, key=lambda d: (d.get("score", 0.0), -d.get("frame", 0)))
        t["best"] = dict(best)
        t["avg_score"] = sum(d.get("score", 0.0) for d in detections) / float(max(1, len(detections)))
        t["length"] = len(detections)

    tracks = [t for t in tracks if t.get("detections")]
    tracks.sort(key=lambda t: (-t.get("avg_score", 0.0), -t.get("length", 0), t.get("start_frame", 0)))
    tracks = tracks[: max(1, int(args.max_candidates))]

    candidates = []
    for t in tracks:
        best = dict(t["best"])
        best["track_id"] = int(t["track_id"])
        merge_or_append(candidates, best, frame_window)
    candidates.sort(key=lambda c: (-c["score"], c["frame"]))

    for idx, item in enumerate(candidates):
        src_frame = canonical_to_source_frame(item["frame"], source_fps)
        frame = reader.read(src_frame) if reader.mode == "sequence" else None
        if frame is None:
            # Re-open for random access when needed after iteration.
            reopen_reader, _ = open_media(args.video, args.source_fps)
            frame = reopen_reader.read(src_frame)
            reopen_reader.close()
        if frame is None:
            continue
        crop = crop_square(frame, item["x"], item["y"], item["box"])
        crop_name = f"{args.crop_prefix}_idx{idx + 1:03d}.png"
        crop_path = os.path.join(args.output_dir, crop_name)
        cv2.imwrite(crop_path, crop)
        item["crop_path"] = crop_path
    serialized_tracks = []
    for t in tracks:
        d = {
            "track_id": int(t["track_id"]),
            "start_frame": int(t["start_frame"]),
            "end_frame": int(t["end_frame"]),
            "avg_score": float(t.get("avg_score", 0.0)),
            "length": int(t.get("length", 0)),
            "best": dict(t.get("best", {})),
            "detections": list(t.get("detections", [])),
        }
        serialized_tracks.append(d)

    payload = {"candidates": candidates, "tracks": serialized_tracks}
    with open(args.output_json, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
    print(f"Wrote {len(candidates)} candidates to {args.output_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
