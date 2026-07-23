#!/usr/bin/env python3
import json
import tempfile
import unittest
import struct
from pathlib import Path

from sam3_resume import (
    FrameResumeState,
    center_frames_from_jsonl,
    frame_indices_from_files,
)


class Sam3ResumeTests(unittest.TestCase):
    def test_binary_mask_only_resume_skips_existing_masks(self):
        state = FrameResumeState(
            require_binary_masks=True,
            binary_mask_frames={0, 2},
        )

        self.assertTrue(state.frame_complete(0))
        self.assertFalse(state.frame_complete(1))
        self.assertTrue(state.frame_complete(2))
        self.assertEqual(state.resumed_count(total=4), 2)

    def test_centers_only_resume_uses_jsonl_frames(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "centers.jsonl"
            path.write_text(
                json.dumps({"frame": 0}) + "\n" +
                json.dumps({"frame": 3}) + "\n",
                encoding="utf-8",
            )

            state = FrameResumeState(
                require_centers=True,
                center_frames=center_frames_from_jsonl(path),
            )

            self.assertTrue(state.frame_complete(0))
            self.assertFalse(state.frame_complete(1))
            self.assertTrue(state.frame_complete(3))
            self.assertEqual(state.resumed_count(total=5), 2)

    def test_mixed_artifacts_require_all_requested_outputs(self):
        state = FrameResumeState(
            require_centers=True,
            require_binary_masks=True,
            center_frames={0, 1},
            binary_mask_frames={1, 2},
        )

        self.assertFalse(state.frame_complete(0))
        self.assertTrue(state.frame_complete(1))
        self.assertFalse(state.frame_complete(2))
        self.assertEqual(state.resumed_count(total=3), 1)

    def test_produce_output_video_disables_frame_resume(self):
        state = FrameResumeState(
            require_centers=True,
            require_binary_masks=True,
            produce_output_video=True,
            center_frames={0, 1},
            binary_mask_frames={0, 1},
        )

        self.assertFalse(state.frame_complete(0))
        self.assertEqual(state.resumed_count(total=2), 0)

    def test_frame_indices_ignore_empty_and_malformed_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            png = (
                b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR"
                + struct.pack(">II", 1, 1)
                + b"\x00\x00\x00\x00IEND\x00\x00\x00\x00"
            )
            (directory / "frame_000001.png").write_bytes(png)
            (directory / "frame_000002.png").write_bytes(b"")
            (directory / "not_a_frame.png").write_bytes(b"x")

            self.assertEqual(frame_indices_from_files(directory, "frame_*.png"), {0})


if __name__ == "__main__":
    unittest.main()
