# Ownership and Refactor Completion Plan

This document is the implementation checklist for the ownership audit. A phase
is complete only when its acceptance checks pass; line movement alone is not
completion.

Baseline on 2026-08-02: `./build.sh` passed on `main` at `763e954`.

## Phase 1 — Enforceable ownership metadata

- [x] Add a machine-readable ownership manifest covering first-party code.
- [x] Enrich the generated structure index with `declared_owner`, `layer`,
  `build_targets`, and per-file ownership violations.
- [x] Add regression tests for ownership resolution and violations.
- [x] Make missing or ambiguous production ownership fail validation.

Acceptance: the structure generator reports complete declared ownership and can
identify target duplication from an existing build graph.

## Phase 2 — One build owner per implementation

- [x] Eliminate `EDITOR_ALL_CPP` and the root-level source glob.
- [x] Give every production `.cpp` one owning library or executable.
- [x] Link consumers to owning libraries rather than recompiling sources.
- [x] Keep executable targets limited to their entry point and genuinely
  executable-specific adapters.

Acceptance: no production `.cpp` appears in more than one non-test target in
`build/build.ninja`, apart from an explicitly documented empty exception list.

## Phase 3 — Canonical document ownership

- [x] Declare `EditorDocumentCore` as the canonical persisted editor model.
- [x] Restrict `TimelineClip`/`TimelineTrack` to a renderer and legacy-Qt
  projection boundary.
- [x] Route durable save/load/history through the canonical model.
- [x] Route persistence normalization through the canonical model instead of
  maintaining an independent projection policy.

Acceptance: canonical mutations and persistence are owned by
`EditorDocumentCore`/`EditorRuntime`; projections cannot become an independent
source of truth.

## Phase 4 — Thin executable and command dispatch

- [x] Reduce executable translation units to startup and delegation.
- [x] Split ImGui shell, platform host, panels, and workflow controllers out of
  `jcut_imgui_main.cpp`.
- [x] Extract command-family handlers from `EditorRuntime::execute()` while
  retaining one document/history owner.

Acceptance: entry points contain process setup only, and no individual command
dispatcher or UI panel function remains a multi-thousand-line owner.

## Phase 5 — UI and domain ownership

- [x] Move speaker assignment mutation out of `tracks.cpp` UI code and into the
  speaker assignment/document service.
- [x] Move inspector tab construction into tab-specific implementation files.
- [x] Leave `InspectorPane` responsible only for pane/tab registration and
  wiring.

Acceptance: UI code issues domain operations through narrow services, and the
inspector facade no longer constructs feature-complete tabs itself.

## Phase 6 — Renderer and audio internals

- [x] Split offscreen Vulkan initialization, frame composition, conversion,
  text preparation, and preview publication behind one renderer lifecycle.
- [x] Split direct Vulkan window interaction from renderer recording.
- [x] Split audio device control, decode/cache work, mixing, and profiling while
  retaining one audio-device owner.
- [x] Keep standalone renderers responsible for backend mechanics and consume
  shared render plans/policies.

Acceptance: resource lifetime remains singular, focused parity tests pass, and
the formerly oversized implementation owners are split by responsibility.

## Final verification

- [x] Regenerate and validate the ownership/structure artifact.
- [x] Run focused ownership, domain, speaker, inspector, render, Vulkan, audio,
  and ImGui tests.
- [x] Run `./build.sh` successfully.
- [x] Record exact verification commands and remaining physical-only acceptance
  gates, if any.

## Artifact review procedure

Review `build/code-structure/code_structure.json` as the canonical machine
artifact and `build/code-structure/code_structure.md` as its review view:

1. Regenerate it from the current worktree and reject stale output.
2. Require zero parse errors and zero ownership violations before using the
   refactor queue.
3. Review `declared_owner`, `layer`, `declared_target`, and `build_targets`
   together. A production `.cpp` with zero or multiple production targets is a
   build-ownership defect.
4. Start refactor review with the largest definition, not merely the largest
   file. Confirm each proposed seam preserves one state/resource lifetime
   owner and moves a coherent responsibility.
5. Spot-check dependency direction for each selected seam, then rerun focused
   behavioral tests after moving code.

## Completion record — 2026-08-02

The generated artifact contains 792 files, 338,651 lines, and 23,817 symbols,
with zero parse errors and zero ownership violations. It also tolerates tracked
files deleted in a dirty worktree, which is covered by a regression test.

Verification commands completed successfully:

```text
python3 -m unittest tests.test_generate_code_structure
python3 scripts/generate_code_structure.py --check-ownership
./build.sh --with-tests
./build.sh
QT_QPA_PLATFORM=offscreen ./build/tests/test_editor_runtime
QT_QPA_PLATFORM=offscreen ./build/tests/test_facedetections_artifacts
QT_QPA_PLATFORM=offscreen ./build/tests/test_effects_tab
QT_QPA_PLATFORM=offscreen ./build/tests/test_mask_tab
QT_QPA_PLATFORM=offscreen ./build/tests/test_transcript_logic
./build/tests/test_audio_mix_policy
./build/tests/test_audio_time_stretch
./build/tests/test_audio_time_stretch_cache
QT_QPA_PLATFORM=offscreen ./build/tests/test_imgui_project_history
QT_QPA_PLATFORM=offscreen ./build/tests/test_direct_vulkan_handoff_pipeline_contract
QT_QPA_PLATFORM=offscreen ./build/tests/test_vulkan_direct_render_parity
QT_QPA_PLATFORM=offscreen ./build/tests/test_imgui_qt_render_parity
QT_QPA_PLATFORM=offscreen ./build/tests/test_vulkan_preview_offline_export
QT_QPA_PLATFORM=offscreen ./build/tests/test_vulkan_subtitle_render
```

The hardware-backed tests exercised Vulkan, CUDA external memory, and NVENC on
an NVIDIA GeForce RTX 3060. The ImGui/Qt transcript parity case skipped because
its temporary transcript fixture contained no readable sections; the dedicated
Vulkan subtitle pixel suite passed all cases. No physical-only acceptance gate
applies to this ownership refactor. Interactive UI ergonomics remain a normal
manual smoke-test concern rather than an ownership acceptance condition.
