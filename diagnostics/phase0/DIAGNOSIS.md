# Phase 0 — Diagnosis (macOS leg, 2026-06-11)

> Scope per ambitious_plan.md D1/D2: this is the **Mac-verifiable** half of Phase 0. The
> cut-to-black on the RTX 3060 (NVDEC/CUDA path) cannot reproduce here and remains
> **[3060-deferred]**. What this leg established: a working build/test/diagnostics loop on macOS,
> answers to D6 and the CI question, and five concrete defects found and (three of them) fixed.

## 1. D6 answered — the CPU-upload fallback flip

- `6f93b71` (2026-05-31) **declared the strict zero-copy contract**: profiling reported
  `vulkan_visible_cpu_upload_fallback_enabled=false`, contract string "CPU image upload is
  rejected".
- Black frames persisted regardless (`00f3620` 2026-06-01 *"It's still cutting the black"*).
- **`414b765` (HEAD, 2026-06-10) deliberately relaxed the contract as a last move**, in five
  coupled changes inside `vulkan_preview_surface.cpp`:
  1. removed the forced `HardwareZeroCopy` decode preference (ctor + `ensureFramePipeline`),
  2. `requireDirectVulkanPayload` flipped `true → false` in `requestFramesForCurrentPosition`,
  3. the frame-validity predicate started accepting CPU-image payloads,
  4. added a held-frame mechanism re-presenting the last frame up to `ceil(fps × 2.0)` (~2 s!)
     stale during playback (`m_lastPresentedFrameByClip`, `maxHeldFrameDelta`) — forbidden move
     F3 (tolerance inflation) in its purest form,
  5. `warmPlaybackLookahead` failure now deactivates the playback pipeline.
- Per its own commit message, playback **still** cut to black afterward. Conclusion for Phase 1:
  the relaxation neither caused nor fixed the failure; it is symptom-patching layered over the
  real bug, and can be unwound once the root cause is fixed. The strict machinery
  (`requireHardwareOrGpuPayload`, `strict_payload_rejected` + tests) remains in place to wire as
  the default.

## 2. CI answered — dead on every push, two independent causes

`gh run list`: every run fails in ~1 min, on every push (verified across the last 10 runs).
- **build-linux / build-macos**: `jurplel/install-qt-action@v3` passes
  `--modules qtbase qtdeclarative` to aqtinstall for Qt 6.4.2 → "packages were not found while
  parsing XML"; the jobs die before configure.
- **asan-build / static-analysis**: get further and die at `find_package(CURL REQUIRED)`
  (CMakeLists.txt:29) — libcurl dev headers are not installed; they never even reach the Vulkan
  check the plan suspected.
Phase 4 starts from "CI has never been green in its current form," not "CI gates 16 tests."

## 3. macOS bring-up findings (3 defects found, 3 fixed, 2 leads open)

**F-A. Idle present storm (FIXED).** On macOS, every `vkQueuePresentKHR` re-dirties the
CAMetalLayer; AppKit calls `displayLayer:`; Qt synthesizes an ExposeEvent; the old
`exposeEvent` override unconditionally scheduled another render → each presented frame triggered
the next (≈91% of main-thread samples inside `startNextFrame`/`endFrame`), starving the UI
thread and the control-server bridge (all `/pipeline` live requests 503). Evidence:
`jcut_idle_mainthread_sample.txt`, backtrace dump `jcut_run6_feeder.log` (request #20–39 all
`CALayer display → QNSView displayLayer: → handleExposeEvent`). Fix: render-on-demand —
schedule from expose only on the not-exposed→exposed transition or surface-size change
(`m_scheduledWhileExposed`/`m_lastExposeScheduledSize`), plus an UpdateRequest gate (render only
when `m_updatePending` latched or playing) and routing all direct `requestUpdate()` handler call
sites through `schedulePreviewUpdate()`.

**F-B. First-run optimization livelock (FIXED) — not macOS-specific.** `ensureOptimizedProfile()`
cleared `m_optimizedProfileEnsureScheduled` at entry; `runStartupOptimizationPass()` spins nested
`processEvents` (via `warmPlaybackLookahead`); any ensure scheduled meanwhile was delivered
*into* that nested loop and recursed into a second pass before the first could finish/save.
On any **fresh checkout** (no cached `optimized_profile.json`) the app never leaves startup.
It never bit on the Linux box because a months-old cached profile short-circuits the pass.
Fix: re-entrancy guard (`m_optimizedProfileEnsureRunning` + `qScopeGuard`), returns
`{busy: true}` to overlapping calls. (Plus temporary `[opt-pass]` progress logging.)

**F-C. Projects root resolves inside the .app bundle (FIXED for session, code fix open).**
`ProjectManager::defaultRootDirPath()` walks up out of a directory *named* `build*` — on macOS
the binary lives in `jcut.app/Contents/MacOS/`, so the app created and loaded a fresh empty
`projects/default` **inside the bundle** instead of the repo's `projects/` (evidence: autosave
path in `jcut_offscreen_session.log`). Worked around via `JCUT_PROJECT_ROOT=<repo>`; the
resolver should also walk up out of `*.app` (Phase 1 hygiene item).

**F-D (open lead). Audio decode hands sample_rate 0 to the resampler.** Two independent
symptoms on macOS: `test_waveform_service` fails with repeated FFmpeg
`[SWR] Requested input sample rate 0 is invalid`, and live playback blocks at the start gate
with `audio_playback_blocked=true`, `last_mix_silence_reason="waiting_for_playable_audio"`,
underruns counting up while the audio-master clock holds at 0 — **the documented start-gate
behavior doing its job** (video correctly refuses to advance without the clock). Since audio is
the master clock, this blocks ALL Mac playback work — first target for the Phase 2 leg on this
machine. (Also: `test_integration` decodes 0×0 frame size and `test_async_decoder` segfaults at
videotoolbox teardown — possibly the same decode-metadata family.)

**F-E (open lead, environmental). Windowed presents stall ~1 s inside MoltenVK**
(`MVKCmdBeginRenderPass::encode` waiting for a drawable) on this mini even with the DELL display
attached and woken — consistent with the session/screen being locked or the window not actually
visible (cannot verify remotely: `screencapture` is blocked for this terminal context). Needs a
human glance at the screen / Screen Recording permission. Until then, **offscreen mode is the
diagnostics vehicle** — REST is fully responsive there (1.8 ms), but note the preview backend is
`offscreen_placeholder`: pipeline_stages are empty, so **visible-decode (H2) metrics require
windowed mode** (or the offscreen Vulkan renderer harness).

## 4. Where this leaves the black-frame hypotheses (§4 of the plan)

- **H1/H3 (clear-fallback draws; black hardware content)**: untestable on this Mac (no NVDEC/
  CUDA; direct-Vulkan windowed path environmentally stalled). [3060-deferred]
- **H2 (visible-decode starvation)**: the *machinery* around it is now exercisable on Mac —
  start gate, audio-clock blocking, underrun accounting all behave per spec and are observable
  via REST. The concrete macOS decode defect (F-D) sits exactly in the decode-delivery path H2
  cares about; fixing it gives a working playback loop on Mac and a second platform for the
  scheduling instrumentation.

## 5. Artifacts in this directory

`ENVIRONMENT.md` (toolchain, invocation), `TEST_BASELINE.md` (+ raw ctest log),
`idle_pipeline.json` / `idle_audio.json` / `play_t6_*` / `play_t20_*` (offscreen testbench
session with 5 clips loaded), `jcut_run*.log` (launch logs incl. storm backtraces),
`jcut_idle_mainthread_sample.txt` (storm sample). Testbench media regenerated under
`testbench_assets/` (1280×720@30 + frame-counter burn-in); `projects/testbench/state.json`
remapped to this checkout's absolute paths.
