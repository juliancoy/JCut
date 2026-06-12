# ambitious_plan.md — JCut Reliability & Render-Pipeline Hardening (v2, fact-checked)

> **Authoring note:**
> This is the operational prompt for the **Fable** model. Fable executes the work. Read this
> whole file before touching code. Version 2 supersedes the v1 draft: every grounding claim in
> v1 was verified against the tree at `/Users/natan/Documents/JCut` (HEAD `414b765`,
> 2026-06-10), and the plan was corrected through an interview with the project owner on
> 2026-06-11. Claims that failed verification have been fixed; work v1 prescribed that is
> already done in the tree is marked done. Nothing here is aspirational hand-waving — every
> claim is traceable to a file, a line, or a commit hash.

---

## 0. Role and Prime Directive

**You are Fable.** You have been brought onto JCut because the previous agent (Codex / ChatGPT)
could not make the editor reliable. The HEAD commit literally reads:

> `414b765` (2026-06-10) — *"Playback is still near the fit choppy and it cuts to black
> frequently on Nvidia RTX 3060. Claude is our only hope"*

Your mission, in one sentence:

> **Make JCut's preview/render pipeline behave exactly as `render.md`, `synchronization.md`, and
> `scheduling.md` specify — so that playback never cuts to black, every stream is aligned to one
> clock and properly buffered, and every invariant is enforced by a regression test that runs in
> CI.**

This is not a feature project. It is a **reliability and professionalization** project, with one
deliberate scope addition decided by the owner: **JCut must become buildable and runnable on
macOS (full app via MoltenVK)**, because that is the machine where development now happens.

### Owner decisions (interview, 2026-06-11) — these are binding

| # | Decision |
|---|---|
| D1 | **macOS is a build target.** Phase 0 makes the full app build and run on this Mac via MoltenVK, not just the test suite. |
| D2 | **Do not assume access to the RTX 3060 box.** All gates are split into *Mac-verifiable now* vs *deferred 3060 validation* (§7). Nvidia-specific gates are executed when that box becomes available. |
| D3 | **CI targets GitHub-hosted runners.** No self-hosted GPU runner. GPU-adjacent tests run via software Vulkan (lavapipe) offscreen where possible. |
| D4 | **The target architecture is Vulkan-only.** OpenGL preview is present-state compatibility scheduled for removal once the parity gates in `OPENGL_DEPRECATION_AND_REMOVAL_PLAN.md` pass. Docs describe Vulkan-only. |
| D5 | **The black-frame mechanism is an open hypothesis** (§4). Prior on-box evidence contradicts "every cut-to-black is a clear-fallback draw"; Phase 0 diagnoses without presuming. |
| D6 | **The CPU-upload fallback flip is unexplained.** Phase 0 includes a bisect: find when and why `vulkan_visible_cpu_upload_fallback_enabled` became hardcoded `true` while `plan.md` requires `false`, before Phase 1 decides how to re-tighten. |
| D7 | **Policies get professional defaults and become tunable.** Tolerances/retention should default well on all systems and be modifiable via UI **and** REST — without ever using tunability to mask starvation (§5.3a). |
| D8 | **`projects/testbench` is the representative project** for all gates that can run from this repo. |
| D9 | **Docs describe the target professional architecture**, with present-state deviations marked inline as **Present state:** callouts. The core spec docs were overhauled to this convention on 2026-06-11. |

### The single most important realization

JCut does **not** lack a design. It has an unusually rigorous one:

- `render.md` — render stages and allowed data flow (preview authority + export path).
- `synchronization.md` — thread/queue/GPU/frame-lifetime contract, frame lifecycle state machine,
  failure policy table, **Performance Targets** (present interval: no repeated >50 ms spikes),
  CI mapping, and Invariants 1–13 (plus 6a and 7b).
- `scheduling.md` — start gate, visible/prefetch/decode-queue policy, stale tolerance, retention,
  backpressure shed order, failure modes, the Practical Debugging Playbook, Invariants 1–13.
- `TIME.md` — the single-clock contract and every temporal-domain conversion; Invariants 1–10.
- `DATAFLOW.md` — persistence boundaries; refresh may *read* but must never *write* state.

**The bug is not in the docs. The bug is that the implementation violates them in places, and CI
does not fail when it does.** Your job is to close that gap, permanently.

(Verified caveat: the docs themselves had drifted in spots — three different stale-tolerance
numbers across three docs, a duplicated Invariant 7, a Performance Targets section misattributed
to render.md. These were reconciled in the 2026-06-11 doc overhaul; trust the docs as they now
stand, and treat any remaining doc/code disagreement without a **Present state:** marker as a
defect to surface.)

---

## 1. TL;DR for the impatient (but you must still read the rest)

1. **Build first.** Nothing in this checkout currently builds: submodules are uninitialized,
   cmake/ctest are not installed, and `build.sh` is Linux-only. Phase 0 produces a working macOS
   build (full app, MoltenVK) and a running test suite.
2. **Diagnose from diagnostics, not from vibes.** The pipeline is already instrumented
   (`/pipeline`, `/audio` on port 40130, `clear_fallback_draw_count`, `strict_payload_rejected`,
   `stale_frame_rejected`, `exact_hit_rate`, …) and HEAD already shipped purpose-built probes
   (`tests/live_playback_sync_probe.py`, `tests/live_playback_lag_probe.py`,
   `scripts/capture_callstacks.sh`). Use them. Prove the failure mechanism before changing a line.
3. **The black-frame mechanism is NOT settled.** Captured on-box evidence (Appendix A6) shows
   black output with `clear_fallback_draw_count: 0` and `last_handoff_mode: hardware_direct` —
   i.e. the presenter believed it was drawing real hardware frames. Treat clear-fallback draws,
   visible-decode starvation, and black hardware-frame *content* (CUDA↔Vulkan sync) as competing
   hypotheses (§4).
4. **Kill every silent fallback — building on what exists.** Strict payload rejection already
   exists in the cache layer (`requireHardwareOrGpuPayload`, `strict_payload_rejected`, with
   tests). What's missing is making it the *default* for direct-Vulkan visible video and gating
   the live `cpu_upload` path (`vulkan_preview_surface.cpp:1469`) behind an explicit, logged
   opt-in — after the D6 bisect explains why it's currently enabled.
5. **One clock, all streams.** Everything derives from `absolutePlaybackSample` through the
   canonical converters (TIME.md). No subsystem advances its own notion of "now."
6. **Buffer properly.** Honor the documented retention (`effectiveVisibleDecodeKeepWindow()`,
   24/96/240 frames) and the stale tolerance (`previewMaxPlaybackStaleFrameDelta()`: ~67 ms of
   source media, clamped **4–8** source frames — *not* "≤4"). Per D7, make these tunable with
   good defaults rather than buried constants.
7. **Total regression coverage, enforced in CI.** 41 tests are registered; CI gates only 16 (and
   probably doesn't even configure — see Phase 4). Fix CI for GitHub-hosted runners, tier the
   suite, gate everything.
8. **Behavior-preserving, gated, incremental.** Each phase has an exit gate and a verification
   command. You do not advance until the gate is green. This is how you avoid repeating Codex's
   regression thrash.

---

## 2. Understand your predecessor: why Codex struggled

You must internalize these failure modes, because **the forbidden moves below are exactly the
ones that produced the black frames.** The git log is the evidence (164 commits, 2026-03-20 to
2026-06-10); the specs already name the anti-patterns.

| # | Codex failure mode | Evidence (hashes verified) | The rule it broke |
|---|---|---|---|
| F1 | **Symptom-patching.** Made playback "look better" locally instead of fixing root scheduling/decode cause. | `314fb03`, `6ea7d36` *"did i finally fix detection"*; `f673c36` *"Hopefully some modest improvement."*; `47e9b7b`, `231f93f` *"Still, the playback has become broken somehow."* | synchronization.md: *"Any change that only 'makes playback look better' without updating these facts is incomplete."* |
| F2 | **Silent fallback to hide starvation.** CPU bounce / clearing to black instead of reporting the miss. | `cpu_upload` live at `vulkan_preview_surface.cpp:1469`; `vulkan_preview_surface_profiling.cpp:87` hardcodes `vulkan_visible_cpu_upload_fallback_enabled=true` while `plan.md` requires `false`. | scheduling.md Failure Modes; render.md Failure Behavior. |
| F3 | **Tolerance inflation.** Widening stale/retention windows to mask decode lag. | The three spec docs ended up with three different tolerance numbers (4 / 4–8 / 4–12) — drift consistent with repeated fiddling. | scheduling.md: *"Do not increase this limit to hide decode starvation without also documenting the measured user-visible drift."* |
| F4 | **Half-finished migrations left in dual-path limbo.** OpenGL + Vulkan both fully compiled; OpenGL still the runtime fallback surface (`preview_surface_factory.cpp:123,136,159,169`). | `0ed050a` *"about to deprecate OpenGL"* — never finished; README.md:121 still says "OpenGL preview". | OPENGL_DEPRECATION_AND_REMOVAL_PLAN.md exit gates never reached. |
| F5 | **Context-window-driven edits.** Split files to save tokens, losing the cross-cutting invariant picture. | `ae82b63` *"I only broke up the damn file so that my token don't get sucked up"*. | FILESIZE_REDUCTION_STRATEGY.md (split by *responsibility*). |
| F6 | **No enforced gate.** Regressions shipped because CI doesn't fail on contract violations — and likely doesn't run at all (`find_package(Vulkan REQUIRED)` with no Vulkan SDK installed in any CI job). | `.github/workflows/ci.yml` (last touched `a11b09e`, 2026-05-31). | render.md/synchronization.md CI-mapping sections. |
| F7 | **Hope-driven verification.** Committed without running the suite. | `73d0721` *"did it get better?"*; `514c202`, `fcd653a` *"idk latest"*; `fa22392` *"wth"*; `9c6b82e` *"wtf its doing"*. | This plan's §6 verification discipline. |

**Your standard is the opposite of every row above.** When you are tempted to make a black frame
disappear quickly, that temptation *is* F1/F2/F3. Stop and find the cause.

---

## 3. The professional render pipeline (the target state)

This is the canonical ownership chain from `render.md` §"Interactive Preview Path" and
`synchronization.md` §"Decode-To-Preview Steps". **This is the law.** Verification on 2026-06-11
confirmed the running code already matches the *shape* of this chain (the presenter has zero
references to TimelineCache; the export path constructs its own `AsyncDecoder` at
`render_export.cpp:593`); your job is to make the *behavior* match too.

```
EditorWindow            owns the playback clock; publishes absolutePlaybackSample
   │  (timeline sample, never a source-frame guess)
   ▼
VulkanPreviewSurface    maps sample → per-clip media source frame (trim, rate, fps, sync markers)
   │                    decides the visible request; builds VulkanPreviewClipFrameStatus
   ▼
TimelineCache           cache/playback-buffer lookup; pending-visible coalescing; queued callbacks
   │                    (paused/scrub/seek visible requests live here)
   ▼
PlaybackFramePipeline   during active playback: bounded request window + playback buffer
   │                    (PlaybackBuffer kMaxFrames=96; adaptive window-ahead with diagnostics)
   ▼
AsyncDecoder            one per pipeline instance; worker lanes, hw-device refs, priority, cancel
   │                    visible requests are NOT proximity-superseded; exact frame completes or fails
   ▼
DirectVulkanFrameHandoffPipeline   hardware/external frame → sampled Vulkan image (per active clip)
   ▼
DirectVulkanPreviewRenderer        latches PreviewInteractionState; records swapchain commands
   ▼
DirectVulkanPreviewPresenter / QVulkanWindow   presents; marks presented_source_frame
```

**Hard ownership rules (do not violate, do not "simplify"):**

- The presenter consumes **`VulkanPreviewClipFrameStatus` only**. It must never read
  `TimelineCache` directly (synchronization.md Invariant 5/9). *Verified currently true — keep it
  true.*
- **No generic decode dispatcher** in front of `AsyncDecoder` (render.md Core Rule).
- **Decode targets are media source frames; priority distance is timeline-frame distance**
  (scheduling.md Invariants 2–3). Never compare a source frame to a timeline frame (a 60 fps
  source frame is *not* a 30 fps timeline frame — TIME.md explicit failure case, and the
  committed TODO.md called this out for visible-request cancellation: source-frame requests must
  not be cancelled merely because the playhead advanced by timeline-frame cadence).
- The export path (`renderTimelineToFile`, `render.h:104`) creates **its own** `AsyncDecoder` and
  must not borrow preview decoder/cache/buffer/swapchain (render.md Export Path). *Verified
  currently true.*

---

## 4. The black-frame mechanism: competing hypotheses, grounded

v1 of this plan asserted "every cut-to-black is a clear-fallback draw." **That claim is
contradicted by the only captured on-box evidence we have** (preserved in Appendix A6, rescued
from the committed TODO.md before its rewrite): black output was observed on the 3060 with

```
qvulkanwindow_valid: true        handoff_successes > 0
texture_draw_count > 0           checker_draw_count: 0
clear_fallback_draw_count: 0     last_handoff_mode: hardware_direct
last_external_image_size: 1920x1080
```

— the presenter was *successfully drawing hardware frames* that were black, or stale, or both.
Phase 0 must therefore distinguish at least three hypotheses:

- **H1 — Clear-fallback draws.** The presenter has no displayable frame and clears the swapchain
  to the background color, which defaults to black (`direct_vulkan_preview_window.cpp:1701-1704`).
  Counted by `clearFallbackDraws`, surfaced as `clear_fallback_draw_count`
  (`direct_vulkan_preview_presenter.cpp:1137`) — **but note the same counter is keyed
  `fallback_draw_count` in the second snapshot (`:1247`) and at the surface
  (`vulkan_preview_surface.cpp:2194`)**. Any grep or probe must match both key names.
- **H2 — Visible-decode starvation with stale/held presentation.** Decode/cache scheduling fails
  to deliver the exact current frame; symptoms are low `exact_hit_rate`, high
  `missing_frame_rate`, visible null callbacks, `active_frame_stale_rejected=true`. The fix lives
  in scheduling/retention/priority, never in tolerances (scheduling.md).
- **H3 — Black hardware-frame content.** The CUDA→Vulkan handoff *succeeds* but the image content
  is black: the committed TODO.md flags the current CUDA/Vulkan interop (Vulkan external memory
  imported into CUDA, CPU-side `cuStreamSynchronize`, then Vulkan barriers) as a suspect, and
  prescribes an env-gated readback/luma checksum to confirm. If readback luma ≈ 0 while FFmpeg
  source luma is nonzero, the NV12 copy/conversion or cross-API synchronization is producing
  black — the fix is proper CUDA/Vulkan external-semaphore synchronization.

H1 and H3 are distinguishable in one capture: H1 increments `clear_fallback_draw_count`/
`fallback_draw_count`; H3 shows healthy draw counters with black content. H2 underlies either.
It is possible (even likely) that more than one mode is real — `00f3620` (2026-06-01, *"It's
still cutting the black"*) landed the stale-frame policy *after* the A6 evidence was captured,
so the dominant mode may have shifted.

**The investigation order is fixed** (scheduling.md Practical Debugging Playbook):
`/audio` (stream running, no underrun, sidecar ready) → `playback_smoothness` (exact hit rate,
frame lag, presented FPS) → `visible_decode_diagnostics` (hardware vs null completions) →
`visible_decode_retention_policy` (effective keep window + reason) → decoder null-callback
reasons (`superseded`?) → pending visible + block reason → handoff/presentation metrics → overlay
metrics last. Two isolation tools exist and must be used before touching code:
`JCUT_VULKAN_PREVIEW_FORCE_CHECKER=1 ./build/jcut` (black checker ⇒ pipeline/present bug;
visible checker ⇒ move to handoff/decode), and the A6 readback-checksum prescription for H3.

You will produce a written diagnosis (Phase 0) that names, with numbers from a running instance,
which hypothesis (or combination) holds on the hardware you can reach. You do not get to guess.

---

## 5. Operating principles (non-negotiable)

1. **Diagnostics-first.** Reproduce, measure, and *name the failing stage* before editing.
   Capture `/pipeline?live=1&force=1` and `/audio` (default port **40130**, localhost-only;
   override with `EDITOR_CONTROL_PORT`) at idle and during playback. Save them as artifacts.
2. **Root cause only.** No symptom patches. If a change makes the black frame vanish but you
   cannot point to the scheduling/decode/handoff/sync cause it fixed, you have not fixed it.
3. **No silent fallback, ever.** Direct-Vulkan visible video uses hardware/GPU payloads. Failures
   are explicit in diagnostics. No implicit OpenGL fallback, no implicit CPU upload, no silent
   readback (synchronization.md Invariant 8; scheduling.md Invariant 11).
   - **3a (D7). Tunable ≠ inflatable.** Policies (stale tolerance, retention, backlog limits) get
     professional defaults and UI + REST tunability. Tuning away from defaults must be visible in
     diagnostics (the effective value *and* whether it is default), and no code path may
     auto-widen a policy to relieve pressure. The runtime already has precedent:
     `visibleBacklogLimit` is REST-patchable as `preview_visible_backlog_limit`
     (`editor_profiling.cpp:314-315`, `:382-392`). (Note: the symbol `kMaxVisibleBacklog` named in
     PROFESSIONALIZE.md and v1 of this plan **does not exist in code** — the tuning knob is
     `playbackTuning().visibleBacklogLimit`.)
4. **One clock.** Everything derives from `absolutePlaybackSample` via canonical converters
   (TIME.md Invariants 1–10). Never advance an independent video/caption/overlay clock.
5. **Preserve the diagnostics contract.** A change that improves appearance but removes/hides a
   required diagnostic field (synchronization.md §Diagnostics) is **incomplete and will be
   rejected.** Also fix inconsistencies *in* the contract when found — e.g. the
   `clear_fallback_draw_count` vs `fallback_draw_count` key split (§4 H1) should be unified (keep
   both during a deprecation window).
6. **Behavior-preserving, gated, incremental.** Small compile-safe steps. Build + run the relevant
   regression label after each. Never combine a structural split with a behavior change in one
   step. Beware **source-text contract tests**: `test_direct_vulkan_handoff_pipeline_contract.cpp:511-513`
   asserts the literal strings `kVisibleDecodeBaseKeepFrames = 96` / `kVisibleDecodeMaxKeepFrames
   = 240` — constants cannot change without updating the test (which is the point).
7. **Respect dataflow rules.** No JSON/state/history/transcript *writes* on the playback hot path;
   refresh() may read documents for display but must never write state/history/transcript/
   artifacts; guard widget rebuilds; debounce bursts (DATAFLOW.md §Guardrails).
8. **File-size discipline as a *consequence*, not a goal.** Keep files <1500 lines by splitting on
   responsibility (FILESIZE_REDUCTION_STRATEGY.md), never "to save tokens" (F5). The 19 current
   offenders are listed in §A2 (all line counts re-verified 2026-06-11).
9. **Every invariant gets a test, every test runs in CI.** Untested synchronization behavior is
   temporary and must be called out (synchronization.md §CI/Regression Mapping).
10. **Report honestly.** If a gate is red, say so with the output. If you skipped a step, say so.
    No hopeful commit messages.

---

## 6. The plan — phased, with hard exit gates

> Each phase ends with an **Exit Gate**. You may not begin phase N+1 until phase N's gate is
> green and committed. If a gate cannot be met, stop and report why — do not paper over it.
> Per D2, gates marked **[3060-deferred]** are executed when that box is available; they do not
> block phase progression but remain open items in §7.

### Phase 0 — Make JCut build, run, and the failure measurable (macOS first)

This checkout cannot currently build anything: all git submodules are uninitialized (`ffmpeg/`,
`rtaudio/`, `external/*`, `nv-codec-headers`, `loiacono` — all `-` in `git submodule status`),
cmake/ctest are not installed, and `build.sh` is hard Linux-only (`apt-get`, `nproc`, glibc
paths, a pinned Qt 6.4.2 private-header download, `LD_PRELOAD` hacks). The committed CTest
leftovers (`CTestTestfile.cmake`, `DartConfiguration.tcl`, `Testing/`) are from the old Linux box
('compai', `/mnt/Cancer/PanelVid2TikTok/editor`) and are stale. Per D1, the target is the **full
app on macOS via MoltenVK**, not merely the test suite.

Tasks:
1. **Toolchain + submodules.** `git submodule update --init --recursive`; install build deps via
   Homebrew (cmake, ninja, meson for Rubber Band, Qt 6, Vulkan SDK / MoltenVK). Document exact
   versions in `diagnostics/phase0/ENVIRONMENT.md`.
2. **Port `build.sh` to macOS** (keep Linux behavior intact): platform branch for core count
   (`sysctl -n hw.ncpu`), FFmpeg `safe` profile on Apple Silicon (the `nvidia` profile and
   nv-codec-headers are Linux/NVDEC-only; VideoToolbox hwaccel is optional later work, software
   decode is acceptable initially), Rubber Band Meson static build, no `apt-get`/`LD_PRELOAD`
   paths on Darwin. The binary is `jcut` (CMake target `editor`); note DEBUG.md still says
   `./build/editor` — unify naming while you're in there (pick `jcut`, fix docs).
3. **MoltenVK bring-up.** Get `WITH_VULKAN=ON` configuring and the direct-Vulkan preview
   presenting through MoltenVK. Expect portability work (`VK_KHR_portability_subset`, instance
   extensions, format support). OpenGL fallback must NOT be the quiet escape hatch (D4): if
   MoltenVK init fails, fail loudly with diagnostics, and fix it.
4. **Test suite running.** Configure with `EDITOR_BUILD_TESTS=ON` (default ON); run
   `ctest --test-dir build --output-on-failure`. Record per-test pass/fail/skip in
   `diagnostics/phase0/TEST_BASELINE.md`, classifying each of the 41 registered tests as
   logic / decode-dependent / GPU-dependent / live-instance (the 5 Python harnesses).
5. **Repro attempt on Mac with `projects/testbench` (D8).** Play it at 1.0×/1.5×/3.0×; capture
   `/pipeline?live=1&force=1` + `/audio` at idle and mid-playback; run
   `tests/live_playback_sync_probe.py` and `tests/live_playback_lag_probe.py`. The cut-to-black
   may or may not reproduce on Apple silicon — either result is signal. If testbench's media is
   not in the repo, generating it (known fps mixes incl. 60 fps source in a 30 fps timeline) is
   part of this task.
6. **Checker + readback isolation (per §4).** `JCUT_VULKAN_PREVIEW_FORCE_CHECKER=1` to validate
   the present path; if black persists with healthy draws, implement the env-gated readback/luma
   checksum from A6 (debug-only, off by default) to test H3.
7. **D6 bisect.** `git log -S vulkan_visible_cpu_upload_fallback_enabled` (and `-S cpu_upload`)
   to establish when the visible CPU-upload fallback was (re-)enabled and what broke when it was
   strict (`plan.md` claims it was disabled and its acceptance criteria require `false`;
   `vulkan_preview_surface_profiling.cpp:87,292` hardcode `true`). Write the answer down — Phase 1
   depends on it.
8. **CI archaeology.** `gh run list` (or the Actions tab): has `.github/workflows/ci.yml` *ever*
   run green? Expected finding: configure fails on `find_package(Vulkan REQUIRED)`
   (CMakeLists.txt:31) since no job installs a Vulkan SDK. Record the truth — Phase 4 starts from
   it.
9. **Write `diagnostics/phase0/DIAGNOSIS.md`:** a one-page, numbers-backed statement of which §4
   hypothesis (or combination) holds on the hardware you can reach, following the scheduling.md
   playbook order; plus explicit notes on what could *only* be confirmed on the 3060
   **[3060-deferred]**.

**Exit Gate 0:** Full app builds and presents via MoltenVK on this Mac; test suite runs with a
recorded baseline; `diagnostics/phase0/` contains ENVIRONMENT.md, TEST_BASELINE.md, captured
endpoint artifacts, the D6 bisect answer, the CI-archaeology answer, and DIAGNOSIS.md. No code
behavior changed yet (build-system/portability changes excepted).

---

### Phase 1 — Enforce the pipeline contract and kill silent fallbacks

Goal: the running preview path matches §3 exactly, and every fallback is explicit. **Much of the
strict machinery already exists — wire it up as the default, don't rebuild it:**
`TimelineCache` request APIs already take `requireHardwareOrGpuPayload` (default `false`;
`timeline_cache.h:154,169,174`), already drop CPU-only frames with trace
`TimelineCache::visible-strict-payload-rejected` (`timeline_cache_requests.cpp:251-261`), and
already count `strict_payload_rejected` (`timeline_cache.h:237`, `timeline_cache_requests.cpp:600-601`)
with tests (`test_standard_preview_presentation_pipeline.cpp:145`,
`test_direct_vulkan_handoff_pipeline_contract.cpp:255`).

Tasks:
1. **Audit the visible path against `synchronization.md` §Decode-To-Preview Steps 1–12.** For
   each step, confirm the owning object, the data object crossing the boundary, and the failure
   behavior. Produce a checklist of every deviation. (The presenter/cache separation and export
   isolation are already verified clean — focus on steps 4–10.)
2. **Make strict payload the default for direct-Vulkan visible video.** Informed by the D6 bisect:
   pass `requireHardwareOrGpuPayload=true` on the visible path; demote the `cpu_upload` decode
   path (`vulkan_preview_surface.cpp:1463-1469`) and the CPU correction-mask path (`:1432-1445`,
   `vulkan_correction_masks_require_cpu_upload_frame`) to an explicit, logged, diagnosable opt-in
   (REST-visible flag, default off during playback). Replace the hardcoded
   `vulkan_visible_cpu_upload_fallback_enabled=true` (`vulkan_preview_surface_profiling.cpp:87,292`)
   with the real runtime state. If the bisect shows strict mode was relaxed because it *caused*
   black/frozen frames, fix that cause first — do not re-tighten into a known breakage.
3. **Ensure decode never reports an older batch frame as the completed visible request**
   (scheduling.md Visible Decode §; synchronization.md Step 5; committed-TODO.md:
   "`decodeThroughFrame(...)` must not satisfy a visible request with a different older frame. An
   exact miss is a failure to diagnose, not a successful current-frame completion.").
4. **Unify the fallback-counter key names** (`clear_fallback_draw_count` at presenter:1137 vs
   `fallback_draw_count` at presenter:1247 / surface:2194) so probes and CI guards can't miss one
   spelling. Emit both keys for one deprecation window if external tooling reads them.
5. **Make the clear-to-black draw an explicit, counted, reasoned state.** When it must happen
   (genuinely no displayable frame), it carries a named reason (decode starvation vs handoff
   failure vs device loss) in the same diagnostics snapshot.

**Exit Gate 1:** `ctest -L temporal` green (12 tests; the live probe may skip via
SKIP_RETURN_CODE 77 without a running instance — that is a *skip*, record it as such);
`test_direct_vulkan_handoff_pipeline_contract`, `test_no_proxy_hardware_playback_contract`,
`test_standard_preview_presentation_pipeline` green; no code path can reach a CPU upload or
OpenGL present on the visible direct-Vulkan path without an explicit, logged opt-in; the
diagnosis from Phase 0 still reproduces (you have not hidden it, only made it explicit) — or is
already partially improved with a named reason.

---

### Phase 2 — Eliminate the black frame at the root

This is the heart of the project. Fix whatever Phase 0 identified, within the documented
policies — **do not invent new policy to mask starvation.** Apply the task groups the diagnosis
points to:

**If H2 (starvation) is implicated:**
1. **Visible decode priority** is computed in timeline-frame distance after mapping
   source→timeline (scheduling.md Visible Decode Scheduling). Verify current visible work always
   outranks prefetch and is never starved (Invariant 4). Verify 60fps-source-in-30fps-timeline
   does not get visible requests cancelled by timeline-cadence advances (A6, §3).
2. **Pending-visible coalescing** by `(clipId, sourceFrame)` is correct and a stale pending
   request cannot block newer ones indefinitely. The bound is `playbackTuning().visibleBacklogLimit`
   (REST: `preview_visible_backlog_limit`) — justify its default with a test or replace with a
   bounded coalescing queue.
3. **Retention window**: the one adaptive policy `effectiveVisibleDecodeKeepWindow()`
   (`timeline_cache.cpp:858-870`; constants 24/96/240 at `:34-36`; formula
   max(base, lookahead×4, latency×2+lookahead, observedLag×2+lookahead) clamped 24..240). The
   current visible window must **not be evicted before presentation consumes it**
   (synchronization.md Resource Ownership). Remember the source-text test pinning (§5.6).
4. **Stale tolerance** stays the shared `previewMaxPlaybackStaleFrameDelta()` policy: **~67 ms of
   source media (`kPreviewMaxPlaybackStaleSeconds=0.067`), clamped 4–8 source frames** (floor 4,
   cap `kPreviewMaxHeldPresentationFrameDelta=8`; 30fps→4, 60fps→5). If
   `active_frame_stale_rejected` fires inside tolerance, the fix is throughput/scheduling/gating
   — never raising the limit (scheduling.md). Per D7, expose the policy (UI + REST) with these as
   defaults and diagnostics showing effective-vs-default.
5. **Playback start gate & buffering** (scheduling.md Playback Start Gate): playback does not
   start until the selected clock's media is ready; blocked reasons explicit
   (`waiting_for_visible_frames`, `decode_queue_starved`, `waiting_for_retimed_audio`, …). The
   bounded playback window in `PlaybackFramePipeline` (adaptive window-ahead,
   `playback_frame_pipeline.cpp:525-609`) warms before play and keeps current+near-future frames
   resident.
6. **Backpressure shed order** (scheduling.md): drop stale prefetch first; never switch backends,
   never CPU-bounce, never advance an independent clock to relieve pressure.

**If H3 (black hardware content) is implicated:**
7. **Readback confirms, then fix the interop.** Replace the CPU-side `cuStreamSynchronize` +
   barrier pattern with proper CUDA/Vulkan **external semaphore** synchronization (A6 step 5).
   Do not fall back to OpenGL; do not use QImage for original-media Vulkan mode. *(NVIDIA-only
   code path — implementable on Mac, verifiable only on the 3060.)* **[3060-deferred]**

**If H1 (clear-fallback) is implicated:**
8. **Handoff** (synchronization.md Step 10): hardware frames transitioned to sampled-image layout
   before draw; handoff failure explicit; descriptor lifetime honors in-flight frame ownership
   (Invariant 11) — a clip leaving the active set cannot invalidate a submitted frame's resources.

**Exit Gate 2 (Mac-verifiable):** during steady 1.0× playback of `projects/testbench` on this
Mac: `clear_fallback_draw_count`/`fallback_draw_count` does not increase; `exact_hit_rate > 0.90`
after warmup; `avg_frame_lag ≤ 1` source frame; `audio.last_callback_underrun_samples = 0`; no
repeated present intervals > 50 ms (synchronization.md Performance Targets). Re-capture the
Phase 0 diagnostics and show the before/after under `diagnostics/phase2/`.
**Exit Gate 2 [3060-deferred]:** the same numbers, plus "cut-to-black no longer reproduces,"
during NVDEC hardware-decode playback on the RTX 3060.

---

### Phase 3 — Stream alignment on output and proper buffering

Goal (the owner's explicit ask): **all streams well aligned on the output and properly
buffered.** Every visible/audible element is one clock's projection, with a bounded, correct
buffer.

Tasks:
1. **Single-clock proof (TIME.md Invariants 1–10).** Verify video frame, audio sample, transcript
   frame, speaker-label timing, and overlay timing all derive from `absolutePlaybackSample` via
   canonical converters. Subtitles prefer the **presented** media source frame when available so
   text never leads late video (TIME.md Invariant 4 sub-clause; synchronization.md Step 9).
2. **A/V lock.** Audio-master clock is the *effective audible* position, not queued/submitted
   position (TIME.md). In pitch-preserving `time_stretch`, video must hold when the retimed
   segment is blocked — never ride the timer through a blocked audio segment (TIME.md
   Invariant 7).
3. **Caption/overlay buffering.** Overlay workers never own a clock and never block video
   (scheduling.md Overlay Scheduling; `plan.md` — the overlay-GPU plan — is mostly implemented;
   verify rather than rebuild). Stale worker results dropped by key/generation; prior snapshot
   held briefly rather than blocking. FaceDetections/speaker boxes default **off** and use
   source-frame-indexed candidates, never a full per-tick track scan (synchronization.md
   Invariant 6a).
4. **Bounded continuity reads (todo.md Required Next Work).** Preview/Speakers overlays use the
   bounded current-playhead reader over `tracks.idx`; UI paths never call full
   `continuityStreamsForRoot(...)` (which can allocate tens of GB and freeze the UI).
5. **Export-path alignment (render.md Export Path).** A 60 fps export of a 30 fps edit range
   renders twice the output samples by mapping each output-PTS to a fractional timeline position
   — not by changing the encoder `time_base`. The export decoder/cache stays separate from
   preview. `scripts/vulkan_headless_export_compare.sh` exists for preview/export comparison —
   use it.

**Exit Gate 3:** `test_temporal_sync_contract`, `test_transcript_logic`,
`test_transform_skip_aware_timing`, `test_audio_time_stretch`, `test_audio_time_stretch_cache`,
`test_playback_policy`, `test_facestream_overlay_snapshot`, `test_facedetections_preview_smoke`
green. Manual on testbench: preview and exported frame agree at a known timestamp (WYSIWYG);
captions track audio at 1.0×/1.5×/3.0×; no caption-leads-video drift; RSS does not jump multiple
GB toggling track visibility/clicking faces (todo.md Acceptance Criteria).

---

### Phase 4 — Total regression coverage, enforced in CI (GitHub-hosted, per D3)

Current verified reality: **41 tests are registered** (33 in `EDITOR_TEST_TARGETS`, 3 standalone
executables, 5 Python harnesses — `tests/CMakeLists.txt`); the `temporal` label covers **12**
(11 C++ at lines 85-98 plus `test_live_playback_lag_probe`, labels `temporal;live`,
SKIP_RETURN_CODE 77). CI (`.github/workflows/ci.yml`) gates only 16 (5 named + temporal;
`test_timeline_cache` is double-run; an ASAN job re-runs `test_frame_handle`/`test_memory_budget`
with `detect_leaks=0`); the macOS job builds but runs **zero** tests; clang-tidy/cppcheck end in
`|| true` (non-gating). **25 of 41 tests have no CI coverage at all** — including every Vulkan
render/parity/snapshot test and all Python offscreen smoke harnesses. And per Phase 0's
archaeology, the workflow likely cannot even configure (no Vulkan SDK installed for
`find_package(Vulkan REQUIRED)`).

Tasks:
1. **Make CI configure and build at all** on `ubuntu-latest` and `macos-14`: install the Vulkan
   SDK/headers; for runtime-GPU-ish tests install **lavapipe** (Mesa software Vulkan) on Linux so
   offscreen Vulkan tests can execute without hardware. Keep `LD_LIBRARY_PATH`/FFmpeg-prefix
   handling correct.
2. **Tier the suite** (tier = ctest label):
   - **logic** — no GPU, no real decode: runs on ubuntu *and* macos runners (the macOS job stops
     being build-only).
   - **decode** — FFmpeg software decode: ubuntu runner.
   - **gpu-soft** — Vulkan via lavapipe offscreen (`QT_QPA_PLATFORM=offscreen`): parity,
     subtitle/text render, handoff-contract, presentation-pipeline tests. Where lavapipe cannot
     support a test, the test must *assert contract metadata on the null/offscreen path* — never
     skip silently.
   - **gpu-hw** — needs real hardware (NVDEC/CUDA interop, live probes): **not CI** (D3); a
     documented local gate, with the 3060 items **[3060-deferred]**.
3. **Map every invariant → test** using §A4. Add the missing tests (rows marked **ADD**):
   - clear-fallback/fallback draw count stays zero during steady playback (offscreen presenter
     probe);
   - visible request never completes via an older batch frame;
   - no CPU upload / OpenGL present on the visible direct-Vulkan path without explicit opt-in
     (extend the existing `strictPayloadRequirementRejectsCpuFrames` to the surface layer);
   - current visible window is not evicted before presentation consumes it;
   - export at 60 fps from a 30 fps range produces 2× output samples, not duplicated frames;
   - bounded continuity reads: UI path never triggers full derivation (RSS bound or call-count
     assert).
4. **Make CI fail on any tier's failure.** Replace the hand-listed five + single label with:
   build all test targets, then `ctest --output-on-failure -L <tier>` per tier per platform.
   Remove the double/triple-run redundancy unless intentional (keep ASAN as its own job; consider
   widening it beyond 2 tests).
5. **CI guards for forbidden patterns:** a script (run in CI) that greps for new silent
   OpenGL/CPU-upload fallbacks on the visible path (match *both* fallback-counter key spellings),
   and enforces the file-size cap (fail >1500 lines, warn >1200) per
   FILESIZE_REDUCTION_STRATEGY.md. `scripts/scan_redundant_code.sh` exists — evaluate/extend it.

**Exit Gate 4:** Every invariant in §A4 has a named test row; `ctest` green locally for all
runnable tiers; the GitHub workflow runs logic+decode+gpu-soft tiers on push/PR and fails red on
any failure; macOS CI job runs the logic tier; forbidden-pattern + file-size guards wired in and
green.

---

### Phase 5 — Professionalization cleanup (verify, finish, remove)

Goal: remove the structural debt that lets regressions hide. Behavior-preserving only.
**Corrected against the tree — v1 prescribed work that is already done:**

| v1 item | Verified status (2026-06-11) |
|---|---|
| Unify speaker-hover/profile helpers into `preview_speaker_profiles.*` | **DONE** — exists (603/38 lines); all three consumers include and call it; zero duplicate definitions remain. Only per-backend hover-card *drawing* still differs (acceptable; dies with OpenGL removal). |
| Unify audio-pan on `resolveAudioPreviewViewport(...)` | **DONE** — single definition (`audio_preview_support.cpp:61`); both backends consume it (Vulkan via `syncAudioPreviewPanToPlayhead`). |
| Transcript Findings 1, 3, 4 (TRANSCRIPT_LOGIC.md) | **DONE** — `activeTranscriptPathForClipFile` used by all consumers; `transcriptDocumentChanged` handlers refresh skip-aware ranges (`editor_tabs.cpp:411,496,681`); tests `testSpeechFilterUsesActiveTranscriptCut`, `testAllSkippedWordsYieldNoSpeechRanges` exist. |

Remaining tasks:
1. **Transcript Finding 2 (still open):** de-duplicate prepend/postpend state between
   EditorWindow and TranscriptTab (`m_transcriptPrependMs` in `transcript_tab.cpp`). With a test.
2. **OpenGL removal (D4).** Drive `OPENGL_DEPRECATION_AND_REMOVAL_PLAN.md` to its exit gates and
   **remove** the OpenGL preview path: 9 `opengl_preview_window*.cpp` files are still fully
   compiled and `preview_surface_factory.cpp` still constructs `PreviewWindow` as the runtime
   fallback (`:123,136,159,169`). Sequence: Vulkan parity gates green (incl. MoltenVK on Mac) →
   factory fallback becomes explicit-failure-with-diagnostics → OpenGL files deleted → README and
   FILES.md/ARCHITECTURE.md updated. If a parity gate cannot pass, stop and write the blocker
   list — no indefinite dual-path limbo (F4).
3. **Bring over-cap files under 1500 lines** by responsibility (19 offenders, §A2), following the
   `domain_subject_role` scheme. Move-only first; behavior changes never in the same step.
4. **Repo hygiene:** delete the stale committed CTest artifacts (`CTestTestfile.cmake`,
   `DartConfiguration.tcl`, `Testing/`) and the empty `.codex` marker; unify the binary/target
   naming (`jcut` binary, `editor` CMake target, "editor" in DEBUG.md — pick one story and make
   docs match); decide the fate of build-excluded sources (`imgui_preview_window.cpp` is #20 by
   size but not compiled — remove or revive, don't warehouse).

**Exit Gate 5:** No first-party `.cpp`/`.h` over 1500 lines; transcript Finding 2 fixed with a
test; OpenGL path removed (or a written parity-blocker list exists and the fallback is an
explicit failure, not a silent backend switch); repo hygiene items done. Full suite still green.

---

### Phase 6 — Soak, document, and hand back

1. **Soak test** on available hardware (Mac now; 3060 when available **[3060-deferred]**): long
   playback, resize storms, scrub bursts, reopen loops, 1.0×/1.5×/3.0× (stress list from
   REMAINING_WORK.md; speed matrix from this plan). Capture diagnostics; no fallback-draw growth,
   no underruns, no unbounded lag, stable RSS.
2. **Update the living docs** to match reality. The core spec docs were overhauled 2026-06-11
   (Target-vs-Present convention, D9); your job is to keep them true as the code changes: clear
   **Present state:** callouts when you fix the deviation they describe; update
   REMAINING_WORK.md (frozen at 2026-05-06), PROBLEMS.md, FILES.md/ARCHITECTURE.md if ownership
   moved. Docs that drift from code are defects.
3. **Write `diagnostics/FINAL_REPORT.md`:** before/after metrics, the root cause(s) fixed, tests
   added, CI changes — so the next engineer (or model) inherits a proof, not a vibe.

**Exit Gate 6 (Definition of Done):** see §7.

---

## 7. Definition of Done (acceptance gates)

### Mac-verifiable now

- [ ] **Builds and runs on macOS** (full app, MoltenVK preview) and on Linux (build.sh unchanged
      behavior); submodule + dependency bootstrap documented and reproducible.
- [ ] **No cut-to-black on Mac:** `clear_fallback_draw_count`/`fallback_draw_count` does not
      increase across multi-minute testbench playback at 1.0×/1.5×/3.0×.
- [ ] **Exact-frame health on Mac:** `exact_hit_rate > 0.90` after warmup at 1.0×;
      `avg_frame_lag ≤ 1` source frame; `active_frame_not_up_to_date_failure` not sustained.
- [ ] **No silent fallback:** no CPU-image upload or OpenGL present on the visible direct-Vulkan
      path without an explicit, logged opt-in; every fallback/clear/failure draw carries a named
      reason; `vulkan_visible_cpu_upload_fallback_enabled` reports the real runtime state.
- [ ] **One clock, aligned streams:** audio underruns = 0 in steady playback; captions/labels/
      overlays track the presented source frame; preview and export agree at a known timestamp
      (WYSIWYG).
- [ ] **Proper buffering, tunable policies (D7):** retention and stale tolerance use the
      documented single policies with professional defaults, tunable via UI + REST, effective
      values visible in diagnostics; current visible window never evicted before presentation;
      playback start gates explicit.
- [ ] **Bounded UI:** Speakers/preview never trigger full raw-track derivation; RSS stable when
      toggling tracks/clicking faces.
- [ ] **Total regression coverage:** every invariant in §A4 maps to a test; GitHub-hosted CI runs
      logic/decode/gpu-soft tiers on Linux + logic on macOS and **fails red** on any violation;
      forbidden-pattern + file-size guards active.
- [ ] **Structural health:** no first-party file > 1500 lines; OpenGL resolved per D4; repo
      hygiene done; binary naming unified.
- [ ] **Diagnostics intact:** every required `/pipeline` and `/audio` field in synchronization.md/
      scheduling.md still present and accurate; counter key names unified.
- [ ] **Honest paper trail:** `diagnostics/FINAL_REPORT.md` with before/after numbers; living
      docs updated to match the code.

### Deferred until the RTX 3060 box is available (D2)

- [ ] Cut-to-black no longer reproduces during NVDEC hardware-decode playback (the original
      complaint, `414b765`).
- [ ] Exit Gate 2 metrics hold on the 3060 (exact-hit rate, frame lag, underruns, present
      intervals).
- [ ] H3 verification: CUDA/Vulkan external-semaphore sync validated with the readback checksum
      (then readback removed/env-gated per A6 step 7).
- [ ] Soak matrix on the 3060.

---

## 8. How to work and report

- **Branch or trunk:** the owner accepts commits on `main` for this effort (decision 2026-06-11);
  keep commits small, factual, and specific — the opposite of the log you inherited. End commit
  bodies with the required co-author trailer.
- **One change, one reason.** Structural split and behavior change are never in the same commit.
- **Run before you claim.** Every "fixed" is backed by a green gate and, for playback, a captured
  diagnostic delta. If a gate is 3060-deferred, *say so* — don't claim it.
- **Surface, don't hide.** If a contract is wrong or impossible, propose a contract change
  *first* (synchronization.md requires naming the new owner/threads/tests before moving
  responsibilities) — do not quietly diverge. Same for docs: fix or flag, never silently
  contradict (D9).
- **When blocked,** report the exact gate, the exact failing metric/test, and the scheduling.md
  debugging-playbook step you reached. Ask a sharp question, not a vague one.

---

## Appendix A — Reference material

### A1. Read order (do this first)
1. `render.md` — render stages, ownership boundaries, failure behavior, diagnostics.
2. `synchronization.md` — decode→preview contract, frame lifecycle, primitives, resource
   lifetime, failure policy, **Performance Targets**, CI/regression mapping, Invariants 1–13
   (+6a, 7b).
3. `scheduling.md` — start gate, visible/prefetch/decode-queue policy, stale tolerance,
   retention, backpressure, failure modes, debugging playbook, Invariants 1–13.
4. `TIME.md` — temporal domains, conversions, single-clock contract, Invariants 1–10, failure
   playbook.
5. `DATAFLOW.md` — persistence boundaries, refresh invariants, playback hot-path rule.
6. `todo.md`, `REMAINING_WORK.md` (stale: 2026-05-06), `PROBLEMS.md` (all items resolved;
   historical), `PROFESSIONALIZE.md`, `OPTIMIZE.md` — current state & open items.
7. `OPENGL_DEPRECATION_AND_REMOVAL_PLAN.md`, `QT_DEPRECATION_PLAN.md`,
   `DIRECT_VULKAN_PREVIEW_BREAKDOWN_PLAN.md`, `FILESIZE_REDUCTION_STRATEGY.md`, `migration.md`,
   `plan.md` (overlay-GPU plan; mostly done; its cpu-upload acceptance criterion conflicts with
   present code — see D6) — structural plans you will finish or fence.
8. `tests/MANUAL_TEST_CHECKLIST.md`, `scripts/` (parity/throughput/export-compare/callstack
   tooling) — existing verification assets v1 of this plan overlooked.

### A2. Grounding facts (re-verified 2026-06-11 against HEAD 414b765)
- Black-frame counters: `direct_vulkan_preview_presenter.cpp:1137-1138` (keys
  `clear_fallback_draw_count`/`explicit_failure_draw_count`), `:1247-1248` (key
  `fallback_draw_count` for the same counter — **naming split**); background defaults black at
  `direct_vulkan_preview_window.cpp:1701-1704`; surfaced at `vulkan_preview_surface.cpp:1905-1908`
  (read) and `:2194-2195` (emitted as `fallback_draw_count`). `explicitFailureDraws` incremented
  at `direct_vulkan_preview_window.cpp:2268`.
- Live CPU-upload visible path: `vulkan_preview_surface.cpp:1432-1469` (`decodePath="cpu_upload"`
  at `:1469`; `vulkan_correction_masks_require_cpu_upload_frame` at `:1443`;
  `unsupported_payload` at `:1471`). Contradiction: `vulkan_preview_surface_profiling.cpp:87,292`
  hardcode `vulkan_visible_cpu_upload_fallback_enabled=true`; `plan.md` requires `false` (D6).
- Strict machinery (exists, not default): `requireHardwareOrGpuPayload`
  (`timeline_cache.h:154,169,174`); rejection trace `timeline_cache_requests.cpp:251-261`;
  counter `strict_payload_rejected` (`timeline_cache.h:237`, `timeline_cache_requests.cpp:600-601`);
  tests `test_standard_preview_presentation_pipeline.cpp:79,145`,
  `test_direct_vulkan_handoff_pipeline_contract.cpp:255`.
- Stale tolerance: `previewMaxPlaybackStaleFrameDelta()` (`preview_frame_selection.h:21-28`) =
  `qBound(4, ceil(sourceFps × 0.067 s), 8)` — floor 4, cap 8 (30fps→4, 60fps→5). Stale rejection
  wired: `rejectedStale` (`preview_frame_selection.h:65`), diag `stale_frame_rejected`
  (`vulkan_preview_surface.cpp:2061`).
- Retention: `effectiveVisibleDecodeKeepWindow()` (`timeline_cache.cpp:858-870`), constants
  24/96/240 (`:34-36`), formula in `calculateVisibleDecodeRetentionPolicy` (`:59-95`).
  Source-text contract test pins constants: `test_direct_vulkan_handoff_pipeline_contract.cpp:511-513`.
- Backlog bound: `playbackTuning().visibleBacklogLimit`, REST-patchable as
  `preview_visible_backlog_limit` (`editor_profiling.cpp:314-315,382-392`). `kMaxVisibleBacklog`
  does not exist in code (stale name from PROFESSIONALIZE.md).
- Tests/CI: 41 registered tests (33 EDITOR_TEST_TARGETS + 3 standalone + 5 Python); `temporal`
  label = 12 tests (`tests/CMakeLists.txt:85-98` + live probe at `:200-210`, SKIP 77); CI gates
  16, runs zero tests on macOS, 25 tests never gated; ASAN job covers 2 tests; static analysis
  non-gating (`|| true`); configure likely fails (Vulkan REQUIRED at CMakeLists.txt:31, no SDK in
  CI). `EDITOR_BUILD_TESTS` default ON (CMakeLists.txt:14, 702-705).
- Control server: default port 40130 (`editor_main.cpp:175`), env `EDITOR_CONTROL_PORT`,
  localhost-only (`control_server_worker.cpp:105`); `GET /pipeline` at
  `control_server_worker_routes.cpp:842`, `GET /audio` at `:1568`.
- Build: binary `jcut`, CMake target `editor`; `build.sh` Linux-only (apt-get, nproc, Qt 6.4.2
  private headers, LD_PRELOAD); FFmpeg profiles auto|nvidia|safe|safe-noasm, NVENC deliberately
  disabled (decode-only); Rubber Band via Meson, SOLA fallback intentionally disabled. Submodules
  all uninitialized in this checkout.
- Environment history: old Linux box 'compai' (`/mnt/Cancer/PanelVid2TikTok/editor`; docs also
  reference `/home/julian/Documents/JCut`); committed CTest leftovers + an April LastTest.log
  listing a deleted test (`test_visual_effects_parity`); ENDPOINT_TESTING_SUMMARY.md's FFmpeg
  `.so` ABI mismatch is **historical** (that box), not this Mac.
- Over-cap files (>1500 lines, all counts exact): `vulkan_facedetections_offscreen_runner.cpp`
  3368, `offscreen_vulkan_renderer_backend.cpp` 3327, `audio_engine.cpp` 3270, `tracks.cpp` 3242,
  `speakers_tab.cpp` 2659, `direct_vulkan_preview_window.cpp` 2616,
  `speakers_tab_interactions_ai.cpp` 2457, `facedetections_continuity_artifacts.cpp` 2412,
  `vulkan_detector_frame_handoff.cpp` 2274, `vulkan_preview_surface.cpp` 2249,
  `editor_ai_integration.cpp` 2125, `editor_shared_keyframes.cpp` 2089, `inspector_pane.cpp`
  2055, `editor.cpp` 1943, `control_server_worker_routes_ui.cpp` 1922,
  `control_server_worker_routes.cpp` 1813, `speakers_tab_interactions.cpp` 1726,
  `transcript_tab_document.cpp` 1720, `timeline_cache.cpp` 1578. (#20 `imgui_preview_window.cpp`
  1498 is build-excluded.)

### A3. Verification command bank
```bash
# One-time setup in this checkout
git submodule update --init --recursive
brew install cmake ninja meson qt vulkan-headers vulkan-loader molten-vk   # exact set TBD Phase 0

# Build (after Phase 0's macOS port; Linux behavior preserved)
./build.sh --with-tests           # add --asan for the sanitizer pass; --run to launch

# The contract suite (run after EVERY change in the relevant area)
ctest --test-dir build --output-on-failure -L temporal
ctest --test-dir build --output-on-failure -R \
  'test_timeline_cache|test_transcript_logic|test_direct_vulkan_handoff_pipeline_contract|test_vulkan_text_generation|test_vulkan_subtitle_render|test_standard_preview_presentation_pipeline|test_no_proxy_hardware_playback_contract|test_realtime_render_contract|test_temporal_sync_contract|test_realtime_frame_loss_probe'

# Full suite
ctest --test-dir build --output-on-failure

# Live diagnosis (running instance; default port 40130, localhost only)
curl -s "http://127.0.0.1:40130/pipeline?live=1&force=1" | jq .
curl -s "http://127.0.0.1:40130/audio" | jq .
python3 tests/live_playback_sync_probe.py        # HEAD-shipped probes — use them
python3 tests/live_playback_lag_probe.py --seconds 2

# Present-path isolation
JCUT_VULKAN_PREVIEW_FORCE_CHECKER=1 ./build/jcut

# Per-thread CPU during a freeze/black episode
# macOS:
top -pid "$(pgrep -x jcut)" -stats pid,th,cpu -l 2 | tail -40
# Linux (3060 box):
ps -L -p "$(pidof jcut)" -o pid,tid,psr,pcpu,comm --sort=-pcpu | head -30

# Hygiene
git diff --check
```

### A4. Invariant → test → diagnostic matrix (extend to "total coverage")

| Invariant (source) | Minimum test | Key diagnostic field |
|---|---|---|
| Timeline sample → source frame at 1.0×/non-1.0× (sched I2, TIME I2) | `test_timeline_cache`, `test_transcript_logic` | `preview.active_requested_source_frame` |
| `getLatestAtOrBefore` never returns a future frame (sync Step 4) | `test_timeline_cache::testLatestAtOrBeforeNeverReturnsFutureFrame` | — |
| Visible request never completes via an older batch frame (sched, sync Step 5) | **ADD** | `decoder_diagnostics.null_callbacks.superseded`, `visible_decode.null_completed` |
| Direct-Vulkan handoff has no implicit CPU fallback (sync I8) | `test_direct_vulkan_handoff_pipeline_contract`; extend `strictPayloadRequirementRejectsCpuFrames` to the surface layer (**ADD**) | `preview.last_handoff_mode/error`, `strict_payload_rejected`, `vulkan_visible_cpu_upload_fallback_enabled` |
| Renderer consumes a latched per-frame snapshot (sync I13) | `test_direct_vulkan_handoff_pipeline_contract::rendererConsumesLatchedPreviewSnapshot` | — |
| Per-clip handoff descriptor lifetime / in-flight ownership (sync I11) | `test_direct_vulkan_handoff_pipeline_contract::directPreviewUsesPerClipHandoffDescriptors` | `preview.active_clip_handoff_resource_count` |
| Overlay snapshots keyed; stale worker results dropped (sched, sync Step 8) | `test_direct_vulkan_handoff_pipeline_contract::overlayWorkerKeepsNewestCoalescedRequest`, `test_facestream_overlay_snapshot` | overlay worker metrics |
| No-proxy hardware decode is the normal direct-Vulkan path (sched Presentation) | `test_no_proxy_hardware_playback_contract` | `preview.last_visible_request_exact_cached` |
| Approximate frame rejected outside stale tolerance (sched I7) | `test_realtime_render_contract`, `test_realtime_frame_loss_probe` | `preview.active_frame_stale_rejected`, `stale_frame_rejected` |
| **No clear-fallback draw during steady playback** (this plan §7) | **ADD** (offscreen presenter probe; match both counter key names) | `preview.clear_fallback_draw_count` / `fallback_draw_count` |
| Hardware-frame content is non-black when source is non-black (H3) | **ADD** (env-gated readback luma check, debug builds) **[3060-deferred for CUDA path]** | readback luma min/max in `/profile` |
| Caption/transcript timing follows source/presented frame (TIME I4, sync Step 9) | `test_transcript_logic`, `test_temporal_sync_contract` | `preview.active_presented_source_frame` |
| Dynamic speaker framing smooth at fractional positions | `test_transcript_logic::testDynamicSpeakerFramingInterpolatesFractionalPlaybackPosition` | — |
| Vulkan text path renders subtitles/labels (no Qt/Painter in direct path) | `test_vulkan_text_generation`, `test_vulkan_subtitle_render` | text renderer diagnostics |
| Audio time-stretch gates playback explicitly (TIME I7, sched I10) | `test_audio_time_stretch`, `test_audio_time_stretch_cache`, `test_playback_policy` | `audio.time_stretch_readiness_state` |
| Skip-aware transform timing uses active kept ranges (TIME I6) | `test_transform_skip_aware_timing` | — |
| Active cut path is the transcript source of truth (TRANSCRIPT_LOGIC F1 — fixed) | `test_transcript_logic::testSpeechFilterUsesActiveTranscriptCut` | — |
| "All words skipped" → silence, not passthrough (TRANSCRIPT_LOGIC F4 — fixed) | `test_transcript_logic::testAllSkippedWordsYieldNoSpeechRanges` | — |
| Prepend/postpend state single-owner (TRANSCRIPT_LOGIC F2 — open) | **ADD** | — |
| Bounded continuity reads; no full derivation on UI path (todo.md) | **ADD** | RSS stability check |
| Export at 60 fps from 30 fps range → 2× samples (render.md Export) | **ADD** | render stage table |
| Preview/export overlay layout parity (TIME I4) | `test_opengl_vulkan_render_exactness` (until OpenGL removal), `test_vulkan_direct_render_parity` | — |
| Current visible window not evicted before presentation (sync Resource Ownership) | **ADD** | `visible_decode_retention_policy` |

> Rows marked **ADD** are required new coverage for Phase 4. Any invariant you touch that lacks a
> row gets one. "Untested synchronization behavior is temporary and must be called out
> explicitly."

### A5. Forbidden moves (the Codex anti-pattern list — never do these)
1. Present an older/batch/nearest frame as the *completed* current visible request.
2. Silently upload a CPU image or present via OpenGL on the visible direct-Vulkan path.
3. Widen the stale tolerance or retention window — by code OR by quietly changing a tunable
   default — to make starvation disappear (D7: tunable ≠ inflatable).
4. Clear the swapchain to black without a counted, named reason.
5. Compare a media source frame to a timeline frame, or let any subsystem advance its own clock,
   or cancel a source-frame visible request because the playhead advanced by timeline cadence.
6. Do heavy work (JSON writes, full track derivation, CPU image materialization, full transcript
   rescan) on the playback hot path or writes inside `refresh()`.
7. Split a file to "save tokens," or combine a structural split with a behavior change.
8. Land a fix without running the relevant `ctest` label, or commit a hopeful message in place of
   a verified result.
9. Remove or bypass a required diagnostic field to make a metric look better.
10. Move an ownership responsibility without first updating the synchronization contract to name
    the new owner, threads, mutation boundaries, failure behavior, and tests.
11. Treat a `ctest` **skip** (e.g. the live probe's SKIP 77) as a pass when claiming a gate.

### A6. Preserved evidence — "Vulkan Preview Black Output" (from committed TODO.md @ 414b765)

> Rescued verbatim-in-substance before the TODO.md rewrite deleted it. This is the only captured
> on-box evidence of the failure, and it contradicts the v1 mechanism claim — black output with
> zero clear-fallback draws.

Current state (as recorded on the 3060 box):
- Direct Vulkan preview initializes and presents.
- `/profile` reports valid Vulkan swapchain and repeated texture draws.
- Original-media hardware decode is the primary direct Vulkan path. It must work without proxies.
- CUDA/Vulkan handoff reports success when a hardware frame is selected.
- The black-frame failure case is treated as **visible-frame starvation** unless diagnostics show
  handoff or render failure.

Observed profile evidence:
```
qvulkanwindow_valid: true
handoff_successes > 0
texture_draw_count > 0
checker_draw_count: 0          (hardware-direct mode)
clear_fallback_draw_count: 0
last_handoff_mode: hardware_direct
last_external_image_size: 1920x1080
```

Likely causes to resolve, in order (the original debugging ladder — Phase 0/2 follow it):
1. Verify visible decode scheduling before suspecting Vulkan rendering (low `exact_hit_rate`,
   high `missing_frame_rate`/`current_frame_failure_rate`, visible null callbacks ⇒ no usable
   current frame). 60 fps source in a 30 fps timeline is a normal case; source-frame requests
   must not be cancelled by timeline-cadence playhead advances. `decodeThroughFrame(...)` must
   not satisfy a visible request with a different older frame.
2. Verify the graphics pipeline independently of decode:
   `JCUT_VULKAN_PREVIEW_FORCE_CHECKER=1 ./build/jcut`. Black checker ⇒ fix
   VulkanPipeline/descriptors/render pass/viewport/swapchain. Visible checker ⇒ continue to
   handoff/conversion debugging.
3. Keep proxy mode honest, but never as the fix for direct Vulkan.
4. If scheduling is healthy and black remains: add an env-gated readback/checksum of the
   post-conversion Vulkan RGBA image; report luma min/max in `/profile`. Readback luma ≈ 0 while
   FFmpeg source luma is nonzero ⇒ the CUDA NV12 copy/conversion path produces black.
5. If readback confirms black conversion: replace the ad-hoc CUDA/Vulkan sync (Vulkan external
   memory imported into CUDA + CPU-side `cuStreamSynchronize` + Vulkan barriers) with proper
   CUDA/Vulkan **external semaphore** synchronization. Do not fall back to OpenGL. Do not use
   QImage for original-media Vulkan mode.
6. Keep the memory fixes: one Vulkan preview decode worker; small source lookahead; GPU-aware
   cache trimming; hardware-frame intermediate retention fix in `DecoderContext`.
7. Clean up diagnostics after fixing: remove/hide the checker env var if not needed long-term;
   keep useful `/profile` handoff/draw counters; remove temporary readback unless behind an
   explicit debug env var.

---

*End of plan. The specification was always right — and now it is also accurate. Make the software
obey it, make CI keep it that way, and make it run on the machine in front of you.*
