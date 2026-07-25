#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import jcut_frame_index_map as frame_map


STREAM = {
    "nb_frames": "3",
    "duration": "1.0",
    "format_duration": "1.0",
    "r_frame_rate": "3/1",
    "avg_frame_rate": "3/1",
    "time_base": "1/1000",
}


class FrameIndexMapTest(unittest.TestCase):
    def setUp(self) -> None:
        frame_map._SOURCE_CONTENT_HASH_CACHE.clear()
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source_a = self.root / "a.mp4"
        self.source_b = self.root / "b.mp4"
        self.source_a.write_bytes(b"source-a")
        self.source_b.write_bytes(b"source-b")
        self.map_path = self.root / "jcut_frame_map.tsv"
        self.map_path.write_text(
            "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
            "0\t0\t0\n1\t100\t1\n2\t200\t2\n",
            encoding="utf-8",
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def adopt(self) -> dict:
        with mock.patch.object(frame_map, "probe_source_stream", return_value=STREAM), \
             mock.patch.object(frame_map, "_frame_index_map_matches_source", return_value=True):
            return frame_map.adopt_existing_frame_index_map(
                self.source_a, self.map_path
            )

    def write_completed_v2_sidecar(self) -> tuple[Path, dict, dict]:
        self.map_path.write_text(
            "# source_frame\tmask_frame\n0\t0\n0\t1\n2\t2\n",
            encoding="utf-8",
        )
        identity = frame_map.source_identity(self.source_a)
        metadata = {
            "schema": "jcut_frame_index_map_v2",
            "status": "ready",
            "frame_domain": "source_timestamp_to_generated_ordinal",
            "source_identity": identity,
            "output_fps": None,
            "map_file": self.map_path.name,
            "map_sha256": frame_map._file_sha256(self.map_path),
            "mapped_frame_count": 3,
            "min_source_frame": 0,
            "max_source_frame": 2,
            "max_mask_frame": 2,
            "expected_output_frame_count": 3,
        }
        frame_map._atomic_write_json(
            frame_map.frame_index_map_metadata_path(self.map_path),
            metadata,
        )
        completion_path = self.root / "jcut_mask.json"
        completion = {
            "schema": "jcut_mask_sidecar_v1",
            "complete": True,
            "source_type": "sam3_binary_frames",
            "frame_domain": "decode_ordinal",
            "frame_index_map": self.map_path.name,
            "frame_index_metadata":
                frame_map.frame_index_map_metadata_path(self.map_path).name,
            "frame_map_sha256": metadata["map_sha256"],
            "expected_frame_count": 3,
            "source_identity": identity,
        }
        frame_map._atomic_write_json(completion_path, completion)
        for one_based in range(1, 4):
            (self.root / f"frame_{one_based:06d}.png").write_bytes(b"png")
        return completion_path, metadata, completion

    def test_adopted_map_is_bound_to_content_identity_and_digest(self) -> None:
        metadata = self.adopt()
        self.assertEqual(metadata["schema"], frame_map.MAP_SCHEMA)
        self.assertEqual(
            metadata["source_identity"]["identity_schema"],
            frame_map.SOURCE_IDENTITY_SCHEMA,
        )
        self.assertEqual(
            metadata["source_identity"]["content_sha256"],
            "0f9f5ce47831e099e77e295ed8bb627f089efa8672ee6fbdc49eac6f0d7f5275",
        )
        self.assertTrue(metadata["source_identity"]["middle_sha256"])
        self.assertTrue(metadata["map_sha256"])
        self.assertIsNotNone(
            frame_map.validated_frame_index_map_metadata(
                self.source_a, self.map_path
            )
        )
        self.assertIsNone(
            frame_map.validated_frame_index_map_metadata(
                self.source_b, self.map_path
            )
        )

        # Preserve all structural counts/ranges while substituting content.
        self.map_path.write_text(
            "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
            "0\t0\t0\n1\t100\t0\n2\t200\t2\n",
            encoding="utf-8",
        )
        self.assertIsNone(
            frame_map.validated_frame_index_map_metadata(
                self.source_a, self.map_path
            )
        )

    def test_unsampled_source_mutation_is_rejected_by_complete_hash(self) -> None:
        large_source = self.root / "large.mp4"
        large_source.write_bytes(b"a" * (6 * 1024 * 1024))
        before = frame_map.source_identity(large_source)
        time.sleep(0.002)
        with large_source.open("r+b") as source:
            source.seek(1536 * 1024)  # Outside the head, middle, and tail samples.
            source.write(b"b")
            source.flush()
        after = frame_map.source_identity(large_source)
        self.assertEqual(before["head_tail_sha256"], after["head_tail_sha256"])
        self.assertEqual(before["middle_sha256"], after["middle_sha256"])
        self.assertNotEqual(before["mtime_ns"], after["mtime_ns"])
        self.assertNotEqual(before["content_sha256"], after["content_sha256"])
        self.assertFalse(frame_map._identity_matches(before, after))

    def test_unchanged_version_token_reuses_verified_content_hash(self) -> None:
        identity = frame_map.source_identity(self.source_a)
        frame_map._SOURCE_CONTENT_HASH_CACHE.clear()
        with mock.patch.object(
            frame_map,
            "_source_content_sha256",
            side_effect=AssertionError("unchanged source must not be rehashed"),
        ):
            actual = frame_map.source_identity(self.source_a, identity)
        self.assertEqual(actual["content_sha256"], identity["content_sha256"])

    def test_portable_identity_accepts_byte_identical_copy(self) -> None:
        self.adopt()
        copied_source = self.root / "copied.mp4"
        copied_source.write_bytes(self.source_a.read_bytes())
        validated = frame_map.validated_frame_index_map_metadata(
            copied_source, self.map_path
        )
        self.assertIsNotNone(validated)
        self.assertNotEqual(
            frame_map._source_version_token(self.source_a)["inode"],
            frame_map._source_version_token(copied_source)["inode"],
        )

    def test_legacy_v2_identity_migrates_locally_but_copy_fails_closed(self) -> None:
        metadata = self.adopt()
        legacy_identity = dict(metadata["source_identity"])
        legacy_identity.pop("identity_schema")
        legacy_identity.pop("content_sha256")
        legacy_identity.pop("cache_token_schema")
        metadata["source_identity"] = legacy_identity
        metadata_path = frame_map.frame_index_map_metadata_path(self.map_path)
        metadata_path.write_text(json.dumps(metadata), encoding="utf-8")

        copied_source = self.root / "legacy-copy.mp4"
        copied_source.write_bytes(self.source_a.read_bytes())
        self.assertIsNone(
            frame_map.validated_frame_index_map_metadata(
                copied_source, self.map_path
            )
        )

        migrated = frame_map.validated_frame_index_map_metadata(
            self.source_a, self.map_path
        )
        self.assertIsNotNone(migrated)
        self.assertEqual(
            migrated["source_identity"]["identity_schema"],
            frame_map.SOURCE_IDENTITY_SCHEMA,
        )
        persisted = json.loads(metadata_path.read_text(encoding="utf-8"))
        self.assertEqual(
            persisted["source_identity"]["content_sha256"],
            migrated["source_identity"]["content_sha256"],
        )

    def test_portable_comparison_ignores_cache_token_after_full_hash(self) -> None:
        left = frame_map.source_identity(self.source_a)
        right = dict(left)
        right.update(
            {
                "path": "/a/different/host/path.mp4",
                "mtime_ns": "1",
                "ctime_ns": "2",
                "device": "3",
                "inode": "4",
                "head_tail_sha256": "0" * 64,
                "middle_sha256": "f" * 64,
            }
        )
        self.assertTrue(frame_map.source_identities_match(left, right))

    def test_corrupt_and_non_monotonic_maps_are_rejected(self) -> None:
        self.map_path.write_text(
            "0\t0\t0\n2\t100\t1\n1\t200\t2\n",
            encoding="utf-8",
        )
        self.assertIsNone(frame_map.inspect_frame_index_map(self.map_path))
        with mock.patch.object(frame_map, "probe_source_stream", return_value=STREAM):
            with self.assertRaises(RuntimeError):
                frame_map.adopt_existing_frame_index_map(
                    self.source_a, self.map_path
                )

    def test_duplicate_and_gapped_mask_ordinals_are_rejected(self) -> None:
        for contents in (
            "0\t0\t0\n1\t100\t0\n2\t200\t2\n",
            "0\t0\t0\n1\t100\t2\n2\t200\t3\n",
        ):
            with self.subTest(contents=contents):
                self.map_path.write_text(contents, encoding="utf-8")
                self.assertIsNone(frame_map.inspect_frame_index_map(self.map_path))

    def test_duplicate_rounded_source_keys_preserve_every_ordinal(self) -> None:
        self.map_path.write_text(
            "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
            "0\t0\t0\n0\t100\t1\n2\t200\t2\n",
            encoding="utf-8",
        )
        self.assertEqual(
            frame_map.inspect_frame_index_map(self.map_path),
            {
                "mapped_frame_count": 3,
                "min_source_frame": 0,
                "max_source_frame": 2,
                "min_source_presentation_timestamp": 0,
                "max_source_presentation_timestamp": 200,
                "max_mask_frame": 2,
                "expected_output_frame_count": 3,
            },
        )

    def test_duplicate_exact_presentation_timestamps_are_rejected(self) -> None:
        self.map_path.write_text(
            "0\t100\t0\n0\t100\t1\n2\t200\t2\n",
            encoding="utf-8",
        )
        self.assertIsNone(frame_map.inspect_frame_index_map(self.map_path))

    def test_map_only_upgrade_rejects_authentication_and_coverage_mismatch(
        self,
    ) -> None:
        completion_path, _, completion = self.write_completed_v2_sidecar()
        completion["frame_map_sha256"] = "0" * 64
        frame_map._atomic_write_json(completion_path, completion)
        with mock.patch.object(frame_map, "write_jcut_frame_index_map") as write:
            with self.assertRaisesRegex(RuntimeError, "authenticated"):
                frame_map.upgrade_completed_sidecar_frame_map(
                    self.source_a, self.map_path
                )
            write.assert_not_called()

        _, _, completion = self.write_completed_v2_sidecar()
        (self.root / "frame_000002.png").unlink()
        with mock.patch.object(frame_map, "write_jcut_frame_index_map") as write:
            with self.assertRaisesRegex(RuntimeError, "coverage"):
                frame_map.upgrade_completed_sidecar_frame_map(
                    self.source_a, self.map_path
                )
            write.assert_not_called()

    def test_map_only_upgrade_publishes_completion_digest_last(self) -> None:
        completion_path, _, completion = self.write_completed_v2_sidecar()
        old_digest = completion["frame_map_sha256"]

        def publish_v3(input_path: Path, map_path: Path, _fps=None) -> None:
            still_old = json.loads(completion_path.read_text(encoding="utf-8"))
            self.assertEqual(still_old["frame_map_sha256"], old_digest)
            map_path.write_text(
                "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
                "0\t0\t0\n0\t10\t1\n2\t20\t2\n",
                encoding="utf-8",
            )
            metadata = frame_map._base_metadata(
                input_path, None, "ready"
            )
            metadata.update(frame_map.inspect_frame_index_map(map_path) or {})
            metadata["map_sha256"] = frame_map._file_sha256(map_path)
            frame_map._atomic_write_json(
                frame_map.frame_index_map_metadata_path(map_path),
                metadata,
            )

        with mock.patch.object(
            frame_map, "probe_source_stream", return_value=STREAM
        ), mock.patch.object(
            frame_map, "write_jcut_frame_index_map", side_effect=publish_v3
        ):
            upgraded = frame_map.upgrade_completed_sidecar_frame_map(
                self.source_a, self.map_path
            )

        refreshed = json.loads(completion_path.read_text(encoding="utf-8"))
        self.assertEqual(refreshed["frame_map_sha256"], upgraded["map_sha256"])
        self.assertNotEqual(refreshed["frame_map_sha256"], old_digest)
        self.assertEqual(refreshed["source_identity"], upgraded["source_identity"])

    def test_map_only_upgrade_refreshes_suspended_birefnet_resume_last(
        self,
    ) -> None:
        completion_path, metadata, completion = self.write_completed_v2_sidecar()
        completion_path.unlink()
        (self.root / "frame_000003.png").unlink()
        run_path = self.root / "jcut_alpha_run.json"
        run_manifest = {
            "schema": "jcut_birefnet_alpha_run_v1",
            "source_identity": metadata["source_identity"],
            "frame_map_sha256": metadata["map_sha256"],
            "expected_frame_count": 3,
            "model": "test/model",
            "revision": "pinned",
            "device": "cuda",
            "fp16": True,
        }
        frame_map._atomic_write_json(run_path, run_manifest)

        def publish_v3(input_path: Path, map_path: Path, _fps=None) -> None:
            still_old = json.loads(run_path.read_text(encoding="utf-8"))
            self.assertEqual(
                still_old["frame_map_sha256"],
                completion["frame_map_sha256"],
            )
            map_path.write_text(
                "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
                "0\t0\t0\n0\t10\t1\n2\t20\t2\n",
                encoding="utf-8",
            )
            upgraded_metadata = frame_map._base_metadata(
                input_path, None, "ready"
            )
            upgraded_metadata.update(
                frame_map.inspect_frame_index_map(map_path) or {}
            )
            upgraded_metadata["map_sha256"] = frame_map._file_sha256(map_path)
            frame_map._atomic_write_json(
                frame_map.frame_index_map_metadata_path(map_path),
                upgraded_metadata,
            )

        with mock.patch.object(
            frame_map, "probe_source_stream", return_value=STREAM
        ), mock.patch.object(
            frame_map, "write_jcut_frame_index_map", side_effect=publish_v3
        ):
            upgraded = frame_map.upgrade_completed_sidecar_frame_map(
                self.source_a, self.map_path
            )

        refreshed = json.loads(run_path.read_text(encoding="utf-8"))
        self.assertEqual(refreshed["frame_map_sha256"], upgraded["map_sha256"])
        self.assertEqual(refreshed["source_identity"], upgraded["source_identity"])
        self.assertEqual(frame_map._contiguous_mask_frame_count(self.root), 2)
        self.assertFalse((self.root / "jcut_alpha.json").exists())

    def test_adoption_rejects_downsampled_or_source_mismatched_maps(self) -> None:
        with self.assertRaisesRegex(ValueError, "full-rate"):
            frame_map.adopt_existing_frame_index_map(
                self.source_a, self.map_path, output_fps=10.0
            )
        with mock.patch.object(
            frame_map, "_frame_index_map_matches_source", return_value=False
        ):
            with self.assertRaisesRegex(RuntimeError, "do not exactly match"):
                frame_map.adopt_existing_frame_index_map(
                    self.source_a, self.map_path
                )

    def test_adoption_verification_compares_every_generated_pair(self) -> None:
        def write_expected(_input: Path, output: Path, _fps=None) -> None:
            output.write_text(
                "# source_frame\tsource_best_effort_timestamp\tmask_frame\n"
                "0\t0\t0\n1\t100\t1\n2\t200\t2\n",
                encoding="utf-8",
            )

        with mock.patch.object(
            frame_map, "write_jcut_frame_index_map", side_effect=write_expected
        ):
            self.assertTrue(
                frame_map._frame_index_map_matches_source(
                    self.source_a, self.map_path
                )
            )
            self.map_path.write_text(
                "0\t0\t0\n9\t100\t1\n2\t200\t2\n",
                encoding="utf-8",
            )
            self.assertFalse(
                frame_map._frame_index_map_matches_source(
                    self.source_a, self.map_path
                )
            )

    def test_downsampled_timeline_maps_are_explicitly_rejected(self) -> None:
        with self.assertRaisesRegex(ValueError, "one mask for every decoded"):
            frame_map.write_jcut_frame_index_map(
                self.source_a, self.map_path, output_fps=10.0
            )

    def test_invalid_probe_values_and_format_duration_fallback(self) -> None:
        self.assertIsNone(frame_map._rational_float("N/A"))
        self.assertIsNone(frame_map._rational_float("0/0"))
        self.assertIsNone(frame_map._rational_float("garbage"))
        stream = dict(STREAM)
        stream.update({"nb_frames": "30", "duration": "N/A", "format_duration": "1"})
        with mock.patch.object(frame_map, "probe_source_stream", return_value=stream):
            self.assertEqual(
                frame_map.source_frame_rate_for_index_map(self.source_a), 30.0
            )


if __name__ == "__main__":
    unittest.main()
