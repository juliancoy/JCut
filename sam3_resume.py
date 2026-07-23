from __future__ import annotations

import json
import struct
from dataclasses import dataclass, field
from pathlib import Path


def image_file_looks_complete(path: Path) -> bool:
    try:
        size = path.stat().st_size
        if size <= 0:
            return False
        suffix = path.suffix.lower()
        with path.open("rb") as source:
            if suffix == ".png":
                header = source.read(24)
                if (
                    len(header) != 24
                    or header[:8] != b"\x89PNG\r\n\x1a\n"
                    or header[12:16] != b"IHDR"
                    or struct.unpack(">I", header[16:20])[0] <= 0
                    or struct.unpack(">I", header[20:24])[0] <= 0
                    or size < 36
                ):
                    return False
                source.seek(-12, 2)
                return source.read(8) == b"\x00\x00\x00\x00IEND"
            if suffix in {".jpg", ".jpeg"}:
                if source.read(2) != b"\xff\xd8" or size < 4:
                    return False
                source.seek(-2, 2)
                return source.read(2) == b"\xff\xd9"
        return True
    except (OSError, struct.error):
        return False


def frame_indices_from_files(directory: Path | None, pattern: str) -> set[int]:
    if directory is None or not directory.exists():
        return set()
    indices: set[int] = set()
    for file_path in directory.glob(pattern):
        try:
            if not image_file_looks_complete(file_path):
                continue
            indices.add(int(file_path.stem.split("_")[-1]) - 1)
        except Exception:
            continue
    return indices


def center_frames_from_jsonl(path: Path | None) -> set[int]:
    if path is None or not path.exists():
        return set()
    frames: set[int] = set()
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            try:
                frames.add(int(json.loads(line).get("frame", -1)))
            except Exception:
                continue
    return frames


@dataclass
class FrameResumeState:
    require_centers: bool = False
    require_binary_masks: bool = False
    require_output_frames: bool = False
    produce_output_video: bool = False
    center_frames: set[int] = field(default_factory=set)
    binary_mask_frames: set[int] = field(default_factory=set)
    output_frames: set[int] = field(default_factory=set)

    def has_resume_artifact(self) -> bool:
        return self.require_centers or self.require_binary_masks or self.require_output_frames

    def frame_complete(self, frame_idx: int) -> bool:
        if self.produce_output_video or not self.has_resume_artifact():
            return False
        if self.require_centers and frame_idx not in self.center_frames:
            return False
        if self.require_binary_masks and frame_idx not in self.binary_mask_frames:
            return False
        if self.require_output_frames and frame_idx not in self.output_frames:
            return False
        return True

    def resumed_count(self, total: int | None = None) -> int:
        if self.produce_output_video:
            return 0
        if total is not None:
            return sum(1 for idx in range(total) if self.frame_complete(idx))
        candidates = set()
        candidates.update(self.center_frames)
        candidates.update(self.binary_mask_frames)
        candidates.update(self.output_frames)
        return sum(1 for idx in candidates if idx >= 0 and self.frame_complete(idx))

    def mark_complete(self, frame_idx: int) -> None:
        if self.require_centers:
            self.center_frames.add(frame_idx)
        if self.require_binary_masks:
            self.binary_mask_frames.add(frame_idx)
        if self.require_output_frames:
            self.output_frames.add(frame_idx)

    def artifact_labels(self) -> list[str]:
        labels: list[str] = []
        if self.require_centers:
            labels.append("centers")
        if self.require_binary_masks:
            labels.append("binary masks")
        if self.require_output_frames:
            labels.append("preview frames")
        return labels
