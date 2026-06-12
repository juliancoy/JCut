# Phase 0 — Test Suite Baseline (macOS, 2026-06-11)

First-ever run of the suite on macOS (arm64, Qt 6.11.1, MoltenVK, bundled FFmpeg `safe` profile).
Invocation: `QT_QPA_PLATFORM=offscreen ctest --test-dir build --timeout 90`
Raw log: `ctest_baseline_raw.log`. **36 passed / 9 failed / 1 skipped of 45 registered tests** (the
suite has grown since the plan's 41-test census — e.g. `test_audio_mix_policy` is new).

Headline: the entire 13-test `temporal` contract label passes, as do all Vulkan
render/parity/snapshot tests (note: several pass in ~0.04 s, which suggests internal
skip/null-path behavior offscreen — Phase 4's "never skip silently" audit should verify what they
actually exercised).

## Failure triage

| Test | Class | Finding |
|---|---|---|
| `cppmonetize_tests`, `cppmonetize_oauth_tests` | not built | "Not Run" — registered but executables absent (likely EXCLUDE_FROM_ALL); decide build-or-deregister in Phase 4. |
| `test_async_decoder` | **real, macOS** | All test logic passes, then SIGSEGV at teardown in `testMultipleRequests` — crash during decoder shutdown with the shared **videotoolbox** hw device (macOS-only device type). Teardown race in AsyncDecoder hw-device release. |
| `test_integration` | **real, macOS** | `testDecodeRealVideo`: decoded `info.frameSize` is 0×0 (expected 320×240) on the videotoolbox path. Decode metadata broken on macOS. |
| `test_waveform_service` | **real, macOS** | Repeated FFmpeg `[SWR] Requested input sample rate 0 is invalid` → 18 s failure. The audio decode path hands sample_rate 0 to the resampler. **Likely shares a root with the live `waiting_for_playable_audio` playback block (see DIAGNOSIS.md §4).** |
| `test_ui_smoke_harness_offscreen`, `test_rest_smoke_offscreen`, `test_ui_responsiveness_budget_offscreen` | env/naming | Looked for `build/editor`; macOS bundle puts the binary at `build/jcut.app/Contents/MacOS/jcut`. Unblocked via `build/editor` symlink — `rest_smoke_test.py --offscreen` then passes green (ok=true, restart, playhead advance, 1.8 ms REST latency). Proper fix in Phase 4/5 (binary-name unification). |
| `test_speaker_flow_e2e_offscreen` | env/assets | Wants `testbench_assets/video/pseudorandom_source.json` (speaker/transcript artifact) — not in repo, not yet regenerated. |
| `test_live_playback_lag_probe` | expected skip | SKIP_RETURN_CODE 77 without a live instance — correct behavior. |

## Environment notes affecting tests

- AsyncDecoder on macOS creates the shared `videotoolbox` hw device; `cuda`/`vaapi` are
  unavailable and log fallbacks (expected).
- Audio default output was a 1ch/16kHz Bluetooth device, which made RtAudio's
  `openStream` fail (channel-count negotiation is strict); switched system default to the DELL
  display's 2ch/48k output (`SwitchAudioSource -s "DELL S2721H"`). RtAudio negotiation
  robustness is a Phase 3 item.
