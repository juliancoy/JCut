#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "apply_media_alignment.py"
)
SPEC = importlib.util.spec_from_file_location("apply_media_alignment", SCRIPT_PATH)
assert SPEC and SPEC.loader
ALIGNMENT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ALIGNMENT)


def clip(
    clip_id: str,
    start_sample: int,
    source_sample: int,
    duration_sample_count: int,
    **extra,
):
    value = {
        "id": clip_id,
        "label": clip_id,
        "mediaType": "audio",
        "playbackRate": 1,
        "sourceFps": 30,
        "sourceDurationFrames": 10_000,
        "startFrame": start_sample // 1600,
        "startSubframeSamples": start_sample % 1600,
        "sourceInFrame": source_sample // 1600,
        "sourceInSubframeSamples": source_sample % 1600,
        "durationFrames": duration_sample_count // 1600,
        "durationSubframeSamples": duration_sample_count % 1600,
        "transformKeyframes": [],
        "gradingKeyframes": [],
        "opacityKeyframes": [],
        "effectEnabledKeyframes": [],
        "titleKeyframes": [],
        "correctionPolygons": [],
    }
    value.update(extra)
    return value


class ApplyMediaAlignmentTest(unittest.TestCase):
    def setUp(self):
        self.parent = clip("audio", 1600, 8000, 16000)
        self.child_a = clip(
            "title-a",
            3200,
            0,
            1600,
            mediaType="title",
            linkedSourceClipId="audio",
        )
        self.child_b = clip(
            "title-b",
            11200,
            0,
            1600,
            mediaType="title",
            linkedSourceClipId="audio",
        )
        self.state = {
            "stateRevision": 4,
            "timeline": [self.parent, self.child_a, self.child_b],
            "renderSyncMarkers": [
                {"clipId": "audio", "frame": 7, "action": "skip", "count": 1}
            ],
        }
        self.plan = {
            "clipId": "audio",
            "expectedOriginal": {
                "startFrame": 1,
                "startSubframeSamples": 0,
                "sourceInFrame": 5,
                "sourceInSubframeSamples": 0,
                "durationFrames": 10,
                "durationSubframeSamples": 0,
                "playbackRate": 1,
            },
            "segments": [
                {
                    "id": "audio",
                    "label": "audio sync 1/2",
                    "timelineStartSample": 1600,
                    "sourceStartSample": 8000,
                    "durationSamples": 6400,
                },
                {
                    "id": "audio-b",
                    "label": "audio sync 2/2",
                    "timelineStartSample": 9600,
                    "sourceStartSample": 14400,
                    "durationSamples": 8000,
                },
            ],
        }

    def test_piecewise_mapping_is_exact_and_reassigns_children(self):
        transformed, summary = ALIGNMENT.transform_state(self.state, self.plan)
        parents = [
            value
            for value in transformed["timeline"]
            if value["id"] in {"audio", "audio-b"}
        ]
        self.assertEqual(len(parents), 2)
        self.assertEqual(ALIGNMENT.timeline_sample(parents[0]), 1600)
        self.assertEqual(ALIGNMENT.source_sample(parents[0]), 8000)
        self.assertEqual(ALIGNMENT.duration_samples(parents[0]), 6400)
        self.assertEqual(ALIGNMENT.timeline_sample(parents[1]), 9600)
        self.assertEqual(ALIGNMENT.source_sample(parents[1]), 14400)
        self.assertEqual(ALIGNMENT.duration_samples(parents[1]), 8000)

        children = {
            value["id"]: value
            for value in transformed["timeline"]
            if value["id"].startswith("title-")
        }
        self.assertEqual(children["title-a"]["linkedSourceClipId"], "audio")
        self.assertEqual(ALIGNMENT.timeline_sample(children["title-a"]), 3200)
        self.assertEqual(children["title-b"]["linkedSourceClipId"], "audio-b")
        self.assertEqual(ALIGNMENT.timeline_sample(children["title-b"]), 12800)
        self.assertEqual(
            transformed["renderSyncMarkers"][0]["clipId"], "audio-b"
        )
        self.assertEqual(transformed["stateRevision"], 5)
        self.assertEqual(summary["linkedChildCount"], 2)
        self.assertEqual(summary["linkedOutputChildCount"], 2)

    def test_child_crossing_a_segment_boundary_is_split_and_rebased(self):
        crossing = clip(
            "title-crossing",
            6400,
            0,
            6400,
            mediaType="title",
            linkedSourceClipId="audio",
            titleKeyframes=[
                {"frame": 0, "text": "A"},
                {"frame": 2, "text": "B"},
                {"frame": 3, "text": "C"},
            ],
        )
        state = json.loads(json.dumps(self.state))
        state["timeline"].append(crossing)
        transformed, summary = ALIGNMENT.transform_state(state, self.plan)
        pieces = [
            value
            for value in transformed["timeline"]
            if value.get("label") == "title-crossing"
        ]
        self.assertEqual(len(pieces), 2)
        self.assertEqual(pieces[0]["linkedSourceClipId"], "audio")
        self.assertEqual(pieces[1]["linkedSourceClipId"], "audio-b")
        self.assertEqual(
            sum(ALIGNMENT.duration_samples(value) for value in pieces), 6400
        )
        self.assertEqual(pieces[1]["titleKeyframes"][0]["frame"], 0)
        self.assertEqual(summary["splitLinkedChildCount"], 1)

    def test_stale_state_and_overlap_are_rejected(self):
        stale = json.loads(json.dumps(self.state))
        stale["timeline"][0]["durationFrames"] = 9
        with self.assertRaisesRegex(ALIGNMENT.AlignmentError, "no longer matches"):
            ALIGNMENT.transform_state(stale, self.plan)

        overlapping = json.loads(json.dumps(self.plan))
        overlapping["segments"][1]["timelineStartSample"] = 7000
        with self.assertRaisesRegex(ALIGNMENT.AlignmentError, "overlap"):
            ALIGNMENT.transform_state(self.state, overlapping)

    def test_apply_creates_backup_and_replaces_state_atomically(self):
        with tempfile.TemporaryDirectory() as directory:
            state_path = Path(directory) / "state.json"
            state_path.write_text(json.dumps(self.state), encoding="utf-8")
            backup_path = Path(directory) / "state.before.json"
            with mock.patch.object(ALIGNMENT, "running_editor_pid", return_value=None):
                summary, actual_backup = ALIGNMENT.apply_state_file(
                    state_path, self.plan, backup_path
                )
            self.assertEqual(actual_backup, backup_path)
            self.assertEqual(
                json.loads(backup_path.read_text(encoding="utf-8")), self.state
            )
            saved = json.loads(state_path.read_text(encoding="utf-8"))
            self.assertEqual(saved["stateRevision"], 5)
            self.assertEqual(summary["backupPath"], str(backup_path))


if __name__ == "__main__":
    unittest.main()
