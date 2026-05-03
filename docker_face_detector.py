#!/usr/bin/env python3
import argparse
import json
import math
import os
import re
import sys
from collections import deque


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def canonical_to_source_frame(frame30, source_fps):
    fps = float(source_fps) if source_fps and source_fps > 0 else 30.0
    return max(0, int(math.floor((float(frame30) / 30.0) * fps)))


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument("--backend", default="opencv_dnn")
    p.add_argument("--video", required=True)
    p.add_argument("--output-json", required=True)
    p.add_argument("--output-dir", required=True)
    p.add_argument("--start-frame", type=int, required=True)
    p.add_argument("--end-frame", type=int, required=True)
    p.add_argument("--step", type=int, default=6)
    p.add_argument("--source-fps", type=float, default=30.0)
    p.add_argument("--max-candidates", type=int, default=128)
    p.add_argument("--crop-prefix", default="continuity_track")
    p.add_argument("--progress-json", default="")
    p.add_argument("--preview-jpg", default="")
    return p.parse_args()


def natural_sort_key(name):
    parts = re.split(r"(\d+)", name)
    key = []
    for part in parts:
        key.append(int(part) if part.isdigit() else part.lower())
    return key


def list_sequence_frames(path):
    allowed = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".webp"}
    frames = []
    for name in os.listdir(path):
        full = os.path.join(path, name)
        if not os.path.isfile(full):
            continue
        if os.path.splitext(name)[1].lower() in allowed:
            frames.append(full)
    frames.sort(key=lambda p: natural_sort_key(os.path.basename(p)))
    return frames


def open_capture(path):
    import cv2
    if os.path.isdir(path):
        frames = list_sequence_frames(path)
        if not frames:
            raise RuntimeError(f"failed to open image sequence: {path}")
        return ("sequence", frames), 30.0
    cap = cv2.VideoCapture(path)
    if not cap.isOpened():
        raise RuntimeError(f"failed to open video: {path}")
    fps = cap.get(cv2.CAP_PROP_FPS)
    if fps is None or fps <= 0:
        fps = 30.0
    return ("video", cap), float(fps)


def iou_xywh(a, b):
    ax1, ay1 = a["x"] - a["box"] / 2.0, a["y"] - a["box"] / 2.0
    ax2, ay2 = a["x"] + a["box"] / 2.0, a["y"] + a["box"] / 2.0
    bx1, by1 = b["x"] - b["box"] / 2.0, b["y"] - b["box"] / 2.0
    bx2, by2 = b["x"] + b["box"] / 2.0, b["y"] + b["box"] / 2.0
    ix1, iy1 = max(ax1, bx1), max(ay1, by1)
    ix2, iy2 = min(ax2, bx2), min(ay2, by2)
    iw, ih = max(0.0, ix2 - ix1), max(0.0, iy2 - iy1)
    inter = iw * ih
    area_a = max(1e-6, (ax2 - ax1) * (ay2 - ay1))
    area_b = max(1e-6, (bx2 - bx1) * (by2 - by1))
    return inter / max(1e-6, area_a + area_b - inter)


def _weights_dir():
    return os.environ.get("JCUT_DNN_WEIGHTS_DIR", "").strip()


def detect_faces_haar(frame):
    import cv2

    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    gray = cv2.equalizeHist(gray)
    cascade = cv2.CascadeClassifier(
        os.path.join(cv2.data.haarcascades, "haarcascade_frontalface_default.xml")
    )
    if cascade.empty():
        return []
    return cascade.detectMultiScale(gray, scaleFactor=1.1, minNeighbors=4, minSize=(24, 24))


def detect_faces_opencv_dnn(frame):
    import cv2

    weights_dir = _weights_dir()
    proto = os.environ.get("JCUT_DNN_PROTOTXT", "").strip()
    model = os.environ.get("JCUT_DNN_MODEL", "").strip()
    if not proto and weights_dir:
        cand = os.path.join(weights_dir, "deploy.prototxt")
        if os.path.exists(cand):
            proto = cand
    if not model and weights_dir:
        cand = os.path.join(weights_dir, "res10_300x300_ssd_iter_140000_fp16.caffemodel")
        if os.path.exists(cand):
            model = cand
    if not proto or not model:
        raise RuntimeError(
            "opencv_dnn backend requires deploy.prototxt and res10_300x300_ssd_iter_140000_fp16.caffemodel"
        )

    cache_key = (proto, model)
    if not hasattr(detect_faces_opencv_dnn, "_cache"):
        detect_faces_opencv_dnn._cache = {}
    if cache_key not in detect_faces_opencv_dnn._cache:
        net = cv2.dnn.readNetFromCaffe(proto, model)
        prefer_cuda = os.environ.get("JCUT_DNN_USE_CUDA", "1").strip() not in {"0", "false", "False"}
        if prefer_cuda:
            try:
                net.setPreferableBackend(cv2.dnn.DNN_BACKEND_CUDA)
                net.setPreferableTarget(cv2.dnn.DNN_TARGET_CUDA_FP16)
            except Exception:
                net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
                net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
        else:
            net.setPreferableBackend(cv2.dnn.DNN_BACKEND_OPENCV)
            net.setPreferableTarget(cv2.dnn.DNN_TARGET_CPU)
        detect_faces_opencv_dnn._cache[cache_key] = net

    net = detect_faces_opencv_dnn._cache[cache_key]
    (h, w) = frame.shape[:2]
    blob = cv2.dnn.blobFromImage(
        frame,
        scalefactor=1.0,
        size=(300, 300),
        mean=(104.0, 177.0, 123.0),
        swapRB=False,
        crop=False,
    )
    net.setInput(blob)
    out = net.forward()
    conf_thresh = float(os.environ.get("JCUT_DNN_CONF", "0.45"))
    detections = []
    for i in range(out.shape[2]):
        conf = float(out[0, 0, i, 2])
        if conf < conf_thresh:
            continue
        x1 = int(out[0, 0, i, 3] * w)
        y1 = int(out[0, 0, i, 4] * h)
        x2 = int(out[0, 0, i, 5] * w)
        y2 = int(out[0, 0, i, 6] * h)
        x1 = max(0, min(w - 1, x1))
        y1 = max(0, min(h - 1, y1))
        x2 = max(0, min(w - 1, x2))
        y2 = max(0, min(h - 1, y2))
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)
        if bw >= 8 and bh >= 8:
            detections.append((x1, y1, bw, bh))
    return detections


def detect_faces_yolo(frame):
    import glob
    from ultralytics import YOLO

    weights_dir = _weights_dir()
    explicit_model = os.environ.get("JCUT_YOLO_MODEL", "").strip()
    model_path = explicit_model
    if not model_path:
        candidates = []
        if weights_dir and os.path.isdir(weights_dir):
            candidates.extend(
                sorted(
                    glob.glob(os.path.join(weights_dir, "*face*.pt")),
                    key=lambda p: os.path.basename(p).lower(),
                )
            )
            candidates.extend(
                sorted(
                    glob.glob(os.path.join(weights_dir, "yolov8*.pt")),
                    key=lambda p: os.path.basename(p).lower(),
                )
            )
        model_path = candidates[0] if candidates else "yolov8n.pt"

    conf = float(os.environ.get("JCUT_YOLO_CONF", "0.20"))
    iou = float(os.environ.get("JCUT_YOLO_IOU", "0.45"))
    imgsz = int(os.environ.get("JCUT_YOLO_IMGSZ", "960"))
    max_det = int(os.environ.get("JCUT_YOLO_MAX_DET", "300"))

    device = os.environ.get("JCUT_YOLO_DEVICE", "").strip()
    if not device:
        try:
            import torch

            device = "0" if torch.cuda.is_available() else "cpu"
        except Exception:
            device = "cpu"

    cache_key = (model_path, device)
    if not hasattr(detect_faces_yolo, "_model_cache"):
        detect_faces_yolo._model_cache = {}
    if cache_key not in detect_faces_yolo._model_cache:
        detect_faces_yolo._model_cache[cache_key] = YOLO(model_path)
    model = detect_faces_yolo._model_cache[cache_key]

    results = model.predict(
        source=frame,
        conf=conf,
        iou=iou,
        classes=None,
        verbose=False,
        imgsz=imgsz,
        max_det=max_det,
        device=device,
    )
    detections = []
    if not results:
        return detections
    r = results[0]
    if r.boxes is None:
        return detections
    for b in r.boxes.xyxy.cpu().numpy().tolist():
        x1, y1, x2, y2 = [int(v) for v in b]
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)
        if bw > 0 and bh > 0:
            detections.append((x1, y1, bw, bh))
    return detections


def detect_faces_retinaface(frame):
    try:
        from insightface.app import FaceAnalysis
    except Exception as exc:
        raise RuntimeError(f"insightface not installed: {exc}")

    if not hasattr(detect_faces_retinaface, "_app"):
        providers = []
        cuda_visible = os.environ.get("CUDA_VISIBLE_DEVICES", "").strip()
        if cuda_visible not in {"", "-1"}:
            providers.append("CUDAExecutionProvider")
        providers.append("CPUExecutionProvider")
        app = FaceAnalysis(name="buffalo_l", providers=providers)
        app.prepare(ctx_id=0 if "CUDAExecutionProvider" in providers else -1, det_size=(640, 640))
        detect_faces_retinaface._app = app
    app = detect_faces_retinaface._app

    faces = app.get(frame)
    out = []
    for face in faces:
        bbox = getattr(face, "bbox", None)
        if bbox is None or len(bbox) < 4:
            continue
        x1, y1, x2, y2 = [int(v) for v in bbox[:4]]
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)
        if bw > 0 and bh > 0:
            out.append((x1, y1, bw, bh))
    return out


def detect_faces_mtcnn(frame):
    try:
        from facenet_pytorch import MTCNN
    except Exception as exc:
        raise RuntimeError(f"facenet-pytorch not installed for mtcnn backend: {exc}")

    if not hasattr(detect_faces_mtcnn, "_det"):
        device = "cpu"
        try:
            import torch

            if torch.cuda.is_available():
                device = "cuda:0"
        except Exception:
            device = "cpu"
        detect_faces_mtcnn._det = MTCNN(keep_all=True, device=device)

    det = detect_faces_mtcnn._det
    boxes, _ = det.detect(frame[..., ::-1])
    out = []
    if boxes is None:
        return out
    for b in boxes.tolist():
        if not b or len(b) < 4:
            continue
        x1, y1, x2, y2 = [int(round(v)) for v in b[:4]]
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)
        if bw > 0 and bh > 0:
            out.append((x1, y1, bw, bh))
    return out


def detect_faces(frame, backend):
    if backend == "insightface_hybrid":
        return detect_faces_retinaface(frame)
    if backend == "yolov8_face":
        return detect_faces_yolo(frame)
    if backend == "opencv_dnn":
        return detect_faces_opencv_dnn(frame)
    if backend == "insightface_retinaface":
        return detect_faces_retinaface(frame)
    if backend == "mtcnn":
        return detect_faces_mtcnn(frame)
    if backend == "opencv_haar":
        return detect_faces_haar(frame)
    raise RuntimeError(f"unsupported backend: {backend}")


def _cosine(a, b):
    if a is None or b is None:
        return -1.0
    import numpy as np

    aa = np.asarray(a, dtype=np.float32).reshape(-1)
    bb = np.asarray(b, dtype=np.float32).reshape(-1)
    na = float(np.linalg.norm(aa))
    nb = float(np.linalg.norm(bb))
    if na <= 1e-6 or nb <= 1e-6:
        return -1.0
    return float(np.dot(aa, bb) / (na * nb))


def detect_faces_retinaface_with_embeddings(frame):
    try:
        from insightface.app import FaceAnalysis
    except Exception as exc:
        raise RuntimeError(f"insightface not installed: {exc}")

    if not hasattr(detect_faces_retinaface_with_embeddings, "_app"):
        providers = []
        cuda_visible = os.environ.get("CUDA_VISIBLE_DEVICES", "").strip()
        if cuda_visible not in {"", "-1"}:
            providers.append("CUDAExecutionProvider")
        providers.append("CPUExecutionProvider")
        app = FaceAnalysis(name="buffalo_l", providers=providers)
        app.prepare(ctx_id=0 if "CUDAExecutionProvider" in providers else -1, det_size=(640, 640))
        detect_faces_retinaface_with_embeddings._app = app
    app = detect_faces_retinaface_with_embeddings._app
    out = []
    faces = app.get(frame)
    for face in faces:
        bbox = getattr(face, "bbox", None)
        if bbox is None or len(bbox) < 4:
            continue
        x1, y1, x2, y2 = [int(v) for v in bbox[:4]]
        bw = max(0, x2 - x1)
        bh = max(0, y2 - y1)
        if bw <= 0 or bh <= 0:
            continue
        emb = getattr(face, "embedding", None)
        out.append(((x1, y1, bw, bh), emb))
    return out


def main():
    args = parse_args()
    os.makedirs(args.output_dir, exist_ok=True)
    try:
        import cv2
    except Exception as exc:
        sys.stderr.write(f"OpenCV import failed: {exc}\n")
        return 1

    source, source_fps = open_capture(args.video)
    start = int(max(0, min(args.start_frame, args.end_frame)))
    end = int(max(args.start_frame, args.end_frame))
    step = int(max(1, args.step))
    max_track_gap = max(step * 2, 90)

    next_track_id = 1
    tracks = []
    active = []
    use_hybrid = args.backend == "insightface_hybrid"
    for frame30 in range(start, end + 1, step):
        src_frame = canonical_to_source_frame(frame30, source_fps)
        if source[0] == "video":
            cap = source[1]
            cap.set(cv2.CAP_PROP_POS_FRAMES, src_frame)
            ok, frame = cap.read()
        else:
            frames = source[1]
            idx = int(clamp(src_frame, 0, max(0, len(frames) - 1)))
            frame = cv2.imread(frames[idx], cv2.IMREAD_COLOR)
            ok = frame is not None
        if not ok or frame is None:
            continue
        h, w = frame.shape[:2]
        try:
            det_face_emb = []
            if use_hybrid:
                det_face_emb = detect_faces_retinaface_with_embeddings(frame)
                dets = [item[0] for item in det_face_emb]
            else:
                dets = detect_faces(frame, args.backend)
        except Exception as exc:
            sys.stderr.write(f"[{args.backend} backend error] {exc}\n")
            return 1

        if args.preview_jpg:
            try:
                preview = frame.copy()
                for (x, y, fw, fh) in dets:
                    cv2.rectangle(preview, (int(x), int(y)), (int(x + fw), int(y + fh)), (0, 255, 0), 2)
                cv2.putText(
                    preview,
                    f"Frame {frame30}  Det {len(dets)}",
                    (12, 28),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.8,
                    (255, 255, 255),
                    2,
                    cv2.LINE_AA,
                )
                cv2.imwrite(args.preview_jpg, preview)
            except Exception:
                pass

        if args.progress_json:
            try:
                with open(args.progress_json, "w", encoding="utf-8") as pf:
                    json.dump(
                        {
                            "frame": int(frame30),
                            "start": int(start),
                            "end": int(end),
                            "detections": int(len(dets)),
                            "tracks": int(len(tracks)),
                        },
                        pf,
                    )
            except Exception:
                pass
        min_side = float(max(1, min(w, h)))
        det_iter = []
        if use_hybrid:
            for (box, emb) in det_face_emb:
                det_iter.append((box[0], box[1], box[2], box[3], emb))
        else:
            for (x, y, fw, fh) in dets:
                det_iter.append((x, y, fw, fh, None))

        for (x, y, fw, fh, emb) in det_iter:
            side = float(max(fw, fh))
            det = {
                "frame": int(frame30),
                "x": clamp((x + fw / 2.0) / float(w), 0.0, 1.0),
                "y": clamp((y + fh / 2.0) / float(h), 0.0, 1.0),
                "box": clamp(side / min_side, 0.01, 1.0),
                "score": clamp(side / min_side, 0.0, 1.0),
            }
            det_emb = emb
            best = None
            best_score = -1.0
            for t in active:
                if frame30 - t["last_frame"] > max_track_gap:
                    continue
                score = iou_xywh(t["last_detection"], det)
                if use_hybrid and det_emb is not None and t.get("emb") is not None:
                    sim = _cosine(t.get("emb"), det_emb)
                    if sim >= 0.0:
                        score = (0.65 * score) + (0.35 * max(0.0, sim))
                if score > best_score:
                    best_score = score
                    best = t
            min_assoc = 0.12 if use_hybrid else 0.05
            if best is None or best_score < min_assoc:
                best = {
                    "track_id": next_track_id,
                    "start_frame": int(frame30),
                    "end_frame": int(frame30),
                    "last_frame": int(frame30),
                    "last_detection": dict(det),
                    "detections": [],
                    "emb": det_emb,
                    "smooth_q": deque(maxlen=4),
                }
                next_track_id += 1
                tracks.append(best)
                active.append(best)
            if use_hybrid:
                q = best.get("smooth_q")
                if q is None:
                    q = deque(maxlen=4)
                    best["smooth_q"] = q
                q.append((det["x"], det["y"], det["box"]))
                sx = sum(v[0] for v in q) / float(len(q))
                sy = sum(v[1] for v in q) / float(len(q))
                sb = sum(v[2] for v in q) / float(len(q))
                det["x"] = clamp(0.7 * sx + 0.3 * det["x"], 0.0, 1.0)
                det["y"] = clamp(0.7 * sy + 0.3 * det["y"], 0.0, 1.0)
                det["box"] = clamp(0.7 * sb + 0.3 * det["box"], 0.01, 1.0)
                if det_emb is not None:
                    best["emb"] = det_emb
            best["detections"].append(dict(det))
            best["end_frame"] = int(frame30)
            best["last_frame"] = int(frame30)
            best["last_detection"] = dict(det)

    if source[0] == "video":
        source[1].release()
    serialized = []
    for t in tracks:
        if not t["detections"]:
            continue
        serialized.append(
            {
                "track_id": int(t["track_id"]),
                "start_frame": int(t["start_frame"]),
                "end_frame": int(t["end_frame"]),
                "detections": list(t["detections"]),
            }
        )

    payload = {"tracks": serialized}
    with open(args.output_json, "w", encoding="utf-8") as f:
        json.dump(payload, f, indent=2)
    print(f"[docker-face-detector] backend={args.backend} tracks={len(serialized)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
