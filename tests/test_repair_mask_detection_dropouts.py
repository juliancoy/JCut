#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from PIL import Image

from repair_mask_detection_dropouts import (
    Repair,
    apply_repairs,
    frame_path,
    foreground_pixels,
    plan_repairs,
    scan_foreground_counts,
)


class RepairMaskDetectionDropoutsTests(unittest.TestCase):
    def test_plan_uses_nearest_healthy_original_and_prefers_past_on_tie(self):
        counts = [100, 100, 5, 100, 10, 60, 100]

        repairs = plan_repairs(
            counts,
            window_frames=2,
            weak_ratio=0.5,
            healthy_ratio=0.8,
            min_reference_pixels=1,
        )

        self.assertEqual(
            [(repair.target_ordinal, repair.donor_ordinal) for repair in repairs],
            [(2, 1), (4, 3)],
        )

    def test_plan_does_not_chain_from_repaired_frames(self):
        repairs = plan_repairs(
            [100, 0, 0, 0, 100],
            window_frames=1,
            weak_ratio=0.5,
            healthy_ratio=0.8,
            min_reference_pixels=1,
        )

        self.assertEqual(
            [(repair.target_ordinal, repair.donor_ordinal) for repair in repairs],
            [(1, 0), (3, 4)],
        )

    def test_apply_unions_original_pixels_and_keeps_byte_backup(self):
        with tempfile.TemporaryDirectory() as temporary:
            sidecar = Path(temporary)
            target_path = frame_path(sidecar, 0)
            donor_path = frame_path(sidecar, 1)
            target = Image.new("L", (3, 1), 0)
            target.putpixel((0, 0), 255)
            donor = Image.new("L", (3, 1), 0)
            donor.putpixel((1, 0), 255)
            target.save(target_path)
            donor.save(donor_path)
            original_bytes = target_path.read_bytes()

            manifest = apply_repairs(
                sidecar,
                [Repair(0, 1, 1, 1, 1)],
                fps=30.0,
                window_seconds=1.0,
                weak_ratio=0.5,
                healthy_ratio=0.8,
                min_reference_pixels=1,
            )

            self.assertIsNotNone(manifest)
            self.assertEqual(foreground_pixels(target_path), 2)
            self.assertEqual(
                (manifest.parent / target_path.name).read_bytes(),
                original_bytes,
            )

    def test_parallel_scan_preserves_frame_order(self):
        with tempfile.TemporaryDirectory() as temporary:
            sidecar = Path(temporary)
            for ordinal, pixels in enumerate((1, 3, 2)):
                image = Image.new("L", (3, 1), 0)
                for x in range(pixels):
                    image.putpixel((x, 0), 255)
                image.save(frame_path(sidecar, ordinal))

            self.assertEqual(scan_foreground_counts(sidecar, 3, workers=2), [1, 3, 2])


if __name__ == "__main__":
    unittest.main()
