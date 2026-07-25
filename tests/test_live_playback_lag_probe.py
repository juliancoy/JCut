#!/usr/bin/env python3

import unittest

import live_playback_lag_probe as probe


def healthy_sample(
    *,
    unique_presentation_misses: int = 0,
    late_sample_rate: float = 0.0,
) -> dict:
    return {
        "diagnostics": {
            "playing": True,
            "playback_active": True,
            "last_playhead_advance_age_ms": 0,
            "main_thread_heartbeat_age_ms": 0,
            "unique_presentation_misses": unique_presentation_misses,
            "active_frame_exact": True,
            "active_frame_up_to_date": True,
            "playback_smoothness": {
                "presented_fps_estimate": 30.0,
                "exact_hit_rate": 1.0,
                "missing_frame_rate": 0.0,
                "current_frame_failure_rate": late_sample_rate,
                "late_sample_rate": late_sample_rate,
                "max_frame_lag": 0,
            },
        }
    }


class LivePlaybackLagProbeTest(unittest.TestCase):
    def test_measurement_uses_ui_free_telemetry_and_one_compact_diagnostic(
        self,
    ) -> None:
        telemetry, diagnostics, screenshot = probe.measurement_urls(
            "127.0.0.1", 40130
        )

        self.assertEqual(
            telemetry,
            "http://127.0.0.1:40130/playback/telemetry",
        )
        self.assertEqual(
            diagnostics,
            "http://127.0.0.1:40130/playback/diagnostics",
        )
        self.assertNotIn("verbose", diagnostics)
        self.assertIn("/screenshot", screenshot)

    def test_fast_telemetry_detects_a_stalled_playback_clock(self) -> None:
        result = probe.classify_fast_telemetry(
            {
                "playback_active": True,
                "current_frame": 120,
                "main_thread_heartbeat_age_ms": 10,
                "last_playhead_advance_age_ms": 700,
            }
        )

        self.assertIsNotNone(result)
        self.assertEqual(result.reason, "playback_clock_not_advancing")

    def test_fast_telemetry_has_no_false_healthy_classification(self) -> None:
        result = probe.classify_fast_telemetry(
            {
                "playback_active": True,
                "current_frame": 120,
                "main_thread_heartbeat_age_ms": 10,
                "last_playhead_advance_age_ms": 10,
            }
        )

        self.assertIsNone(result)

    def test_historical_misses_do_not_reclassify_current_interval(self) -> None:
        result = probe.classify(
            healthy_sample(
                unique_presentation_misses=12,
                late_sample_rate=0.2,
            ),
            presentation_miss_delta=0,
            presentation_misses_since_baseline=0,
            observed_presented_fps=30.0,
        )

        self.assertEqual(result.reason, "late_or_inexact_frames_unclassified")
        self.assertEqual(
            result.details["unique_presentation_misses_interval_delta"],
            0,
        )

    def test_new_presentation_miss_is_attributed_in_its_interval(self) -> None:
        result = probe.classify(
            healthy_sample(unique_presentation_misses=13),
            presentation_miss_delta=1,
            presentation_misses_since_baseline=1,
            observed_presented_fps=30.0,
        )

        self.assertEqual(result.reason, "presenter_recorded_unique_misses")

    def test_low_present_cadence_is_visible_even_when_frames_are_exact(
        self,
    ) -> None:
        result = probe.classify(
            healthy_sample(),
            observed_presented_fps=25.5,
            expected_presented_fps=30.0,
            minimum_presented_fps_ratio=0.90,
        )

        self.assertEqual(
            result.reason,
            "preview_presentation_cadence_below_target",
        )
        self.assertAlmostEqual(
            result.details["observed_presented_fps_ratio"],
            0.85,
        )

    def test_present_cadence_at_threshold_is_healthy(self) -> None:
        result = probe.classify(
            healthy_sample(),
            observed_presented_fps=27.0,
            expected_presented_fps=30.0,
            minimum_presented_fps_ratio=0.90,
        )

        self.assertEqual(result.reason, "no_live_lag_detected")

    def test_counter_reset_does_not_create_a_delta(self) -> None:
        self.assertEqual(probe.nonnegative_counter_delta(2, 12), 0)
        self.assertEqual(probe.nonnegative_counter_delta(15, 12), 3)


if __name__ == "__main__":
    unittest.main()
