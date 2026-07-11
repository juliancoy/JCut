# Large-File Refactor Prompt Playbook

Use these prompts in order. Start each successive prompt only after the prior
prompt has completed its scoped implementation and verification, or has
documented a concrete blocker.

Do not ask one Codex session to implement every phase at once. Each prompt is
intentionally bounded so that ownership, behavior, and tests can be verified
before moving to another subsystem.

For every prompt below, all builds and build verification must use
`./build.sh` from the repository root. Do not invoke CMake, Ninja, Make, or
another underlying build tool directly unless `./build.sh` cannot perform a
required operation; if that occurs, stop and report the limitation rather than
silently bypassing the repository policy. Every handoff must include the exact
`./build.sh` command and options used. Preserve the user's current uncommitted
changes, especially the grading histogram and direct-render parity work noted
in `REFACTOR_PLAN.md`.

## Prompt 1: Speaker and Track Domain

```text
Implement Phase 1 of REFACTOR_PLAN.md using OWNERSHIPMATRIX.md as the
authoritative ownership guide and FILESIZE_REDUCTION_STRATEGY.md as the size
policy.

Scope this session to the speaker and track domain in tracks.cpp and
speakers_tab.cpp, including only the supporting files necessary to complete
their ownership-preserving extraction.

Requirements:

1. Read REFACTOR_PLAN.md, OWNERSHIPMATRIX.md, and
   FILESIZE_REDUCTION_STRATEGY.md completely before changing code. Read and
   obey all applicable AGENTS.md instructions. Do not create a Git branch.
2. Inspect the current worktree first. Preserve user changes and do not modify
   unrelated files.
3. Before every extraction batch, complete the pre-move ownership audit from
   OWNERSHIPMATRIX.md:
   - inventory the source symbols and state being moved;
   - inspect every listed existing candidate owner;
   - classify the work as Keep, Move, Consolidate, Create, or Delegate;
   - identify tests that characterize current behavior;
   - record the completed audit row in OWNERSHIPMATRIX.md.
4. Prefer existing modules over new files. Do not create a destination marked
   Create until repository searches confirm there is no suitable existing
   owner.
5. Audit and consolidate the known duplicate speaker-domain helpers. Add
   equivalence tests before merging implementations, including legacy JSON
   formats, missing values, frame rounding, clip trim/speed mapping, and UI
   fallback behavior as applicable.
6. Keep extractions behavior-preserving. Do not mix moves with unrelated
   cleanup, public API redesign, formatting sweeps, or feature work.
7. Work in small compile-safe batches. After every batch:
   - update the correct CMake target when necessary;
   - build the narrowest relevant target;
   - run focused tests;
   - resolve all introduced warnings and failures before continuing.
8. Keep persistence, assignment rules, artifact parsing, decoding, tracking,
   and click policy in the canonical services identified by
   OWNERSHIPMATRIX.md. Speaker UI files may orchestrate those services but must
   not duplicate their implementations.
9. Continue until both tracks.cpp and speakers_tab.cpp are below the 1,500-line
   hard cap, preferably 800-1,200 lines, unless a concrete technical blocker
   prevents safe completion.
10. At the end, run the full relevant test suite, git diff --check,
    python countlines.py, and scripts/scan_redundant_code.sh. Investigate and
    resolve failures introduced by this work.
11. Update REFACTOR_PLAN.md and OWNERSHIPMATRIX.md when repository evidence
    changes a planned boundary or when an item is completed.
12. Finish with a concise handoff containing:
    - source-to-destination symbol groups;
    - existing owners reused;
    - duplicate implementations removed;
    - final line counts;
    - build/test commands and results;
    - remaining risks, blockers, or intentionally deferred work.

Do not stop after analysis or a proposed plan. Implement and verify the entire
scoped phase. If blocked, exhaust safe in-scope alternatives and document the
exact blocker and the smallest next action required.
```

### Exit gate before Prompt 2

- `tracks.cpp` and `speakers_tab.cpp` are each below 1,500 lines, or the
  implementation reports a concrete blocker.
- Speaker/section selection, assignment/deassignment, face-box interaction,
  transcript save/reload, face-detection generation, and avatar behavior have
  focused verification.
- Canonical assignment and timing helpers have no knowingly active duplicate.
- The recorded build and focused tests pass.

## Prompt 2: Audio Engine

```text
Implement Phase 2 of REFACTOR_PLAN.md using OWNERSHIPMATRIX.md as the
authoritative ownership guide and FILESIZE_REDUCTION_STRATEGY.md as the size
policy.

Scope this session to audio_engine.cpp and the audio modules required to split
device/transport, scheduling, decoding, time-stretch orchestration, caching,
and mixing into their canonical owners.

Requirements:

1. Read REFACTOR_PLAN.md, OWNERSHIPMATRIX.md, and
   FILESIZE_REDUCTION_STRATEGY.md completely. Review the previous phase's
   worktree and handoff. Read and obey applicable AGENTS.md instructions. Do
   not create a Git branch.
2. Preserve user changes and avoid unrelated files.
3. Complete and record a pre-move ownership audit for every extraction batch.
   Inspect audio_time_stretch.*, audio_time_stretch_cache.*,
   audio_speech_harmonic_isolator.*, audio_mix_readiness.h,
   audio_preview_support.*, render_audio.*, waveform_service.*, and direct
   preview audio code before creating or extending modules.
4. Existing algorithm, cache, isolation, preview, export, and waveform owners
   remain authoritative. Move or delegate behavior to them instead of
   creating parallel implementations.
5. Keep AudioEngine's public API stable unless a narrow internal API is
   demonstrably required. Prefer private/internal declarations and avoid
   broadening audio_engine.h.
6. Preserve mutex ownership, lock ordering, worker lifetime, condition-variable
   predicates, and real-time callback constraints. Do not add allocation,
   blocking I/O, logging, or unbounded work to the audio callback.
7. Keep each batch behavior-preserving and compile-safe. After every batch,
   update CMake, build the narrowest target, and run focused tests before
   continuing.
8. Add or strengthen characterization tests for time/sample mapping, readiness,
   range blending, crossfades, cache lookup/failure, seeking, playback-rate
   changes, and time-stretch job state before consolidating implementations.
9. Continue until audio_engine.cpp is below 1,500 lines, preferably 800-1,200,
   and no extracted destination exceeds the hard cap.
10. At completion, run focused audio tests, an export/playback audio comparison
    where supported, the full relevant suite, git diff --check,
    python countlines.py, and scripts/scan_redundant_code.sh. Run a thread
    sanitizer build if the repository supports it; otherwise state that it was
    unavailable.
11. Update the planning and ownership documents with completed audit rows and
    evidence-based boundary changes.
12. Finish with source-to-destination groups, reused owners, duplicates
    removed, threading invariants preserved, final line counts, exact test
    results, and remaining risks.

Do not stop after analysis. Implement and verify the entire scoped phase unless
a concrete blocker prevents safe completion.
```

### Exit gate before Prompt 3

- `audio_engine.cpp` is below 1,500 lines or a concrete blocker is recorded.
- Playback, seek, rate change, cache failure, time stretching, and mixing have
  focused coverage.
- No parallel implementation was introduced beside existing stretch/cache,
  preview, export, isolation, or waveform owners.
- Threading and callback constraints have been explicitly verified.

## Prompt 3: Offscreen Vulkan Renderer

```text
Implement the offscreen-renderer portion of Phase 3 in REFACTOR_PLAN.md using
OWNERSHIPMATRIX.md as the authoritative ownership guide and
FILESIZE_REDUCTION_STRATEGY.md as the size policy.

Scope this session to offscreen_vulkan_renderer_backend.cpp and only the
supporting Vulkan modules necessary to extract device lifecycle, backend
resources, composition, readback, NV12 conversion, and CUDA interop.

Requirements:

1. Read all three refactor documents, the previous phase handoff, and all
   applicable AGENTS.md instructions. Do not create a Git branch. Preserve
   unrelated worktree changes.
2. Complete and record a pre-move ownership audit for every extraction. Inspect
   offscreen_vulkan_renderer_helpers.*, vulkan_resources.*,
   render_vulkan_shared.*, vulkan_detector_frame_handoff.*,
   vulkan_text_renderer.*, and renderer public interfaces first.
3. Reuse shared Vulkan primitives and detector handoff implementations. Do not
   create backend-local copies of memory helpers, resource abstractions,
   synchronization logic, conversions, or text rendering.
4. Establish a narrow private boundary for OffscreenVulkanRendererPrivate.
   Keep the public facade and high-level render orchestration in the original
   backend file.
5. Treat the first pass as move-only. Preserve Vulkan object ownership,
   creation/destruction order, queue-family assumptions, layouts, barriers,
   synchronization, staging lifetime, and error behavior. Do not redesign GPU
   resource ownership during extraction.
6. Work in small compile-safe batches. Build and run validation after each
   batch before proceeding.
7. Add characterization or parity checks where coverage is missing. Verify
   CPU/Vulkan pixel parity, headless export, BGRA, YUV420P, NV12, CUDA transfer
   when available, resize/reinitialize, device loss, repeated startup/shutdown,
   and validation-layer output.
8. Continue until offscreen_vulkan_renderer_backend.cpp and every new file are
   below 1,500 lines, preferably 800-1,200.
9. At completion, run the applicable parity/throughput scripts, focused and
   full relevant tests, git diff --check, python countlines.py, and
   scripts/scan_redundant_code.sh. Clearly distinguish tests skipped because
   required GPU/CUDA hardware is unavailable.
10. Update the planning and ownership documents, then report moved symbol
    groups, shared owners reused, duplicates removed, final line counts,
    validation output, parity evidence, and remaining hardware-dependent risk.

Do not stop after analysis. Implement and verify this entire renderer scope
unless a concrete blocker prevents safe completion.
```

### Exit gate before Prompt 4

- The offscreen backend and its extracted files meet the 1,500-line cap.
- Vulkan validation reports no newly introduced errors.
- Available rendering/readback parity paths pass.
- Hardware-dependent CUDA or format paths are either verified or explicitly
  identified as unverified—not silently assumed correct.

## Prompt 4: Direct Vulkan Preview

```text
Implement the direct-preview portion of Phase 3 in REFACTOR_PLAN.md using
OWNERSHIPMATRIX.md as the authoritative ownership guide and
FILESIZE_REDUCTION_STRATEGY.md as the size policy.

Scope this session to direct_vulkan_preview_window.cpp and the direct-preview
modules required to establish canonical ownership for lifecycle, resources,
readback, frame submission, and the public window facade.

Requirements:

1. Read the three refactor documents, prior handoffs, and applicable AGENTS.md
   instructions. Do not create a Git branch. Preserve unrelated changes.
2. Complete and record the ownership audit for each extraction. Inspect and
   reuse the existing audio, geometry, interaction, presenter, transcript, and
   overlay-rendering modules before creating anything.
3. Move any remaining instances of those existing responsibilities into their
   canonical modules rather than creating alternate implementations.
4. Create a narrow direct_vulkan_preview_internal.h only for declarations
   genuinely shared among the extracted implementation files. Do not expose
   renderer internals publicly.
5. Preserve renderer/window lifetime, swapchain state, resource retirement,
   frame handoff, readback synchronization, device-loss behavior, cursor/input
   behavior, and update scheduling.
6. Keep batches move-only, compile-safe, and independently tested. Update the
   correct CMake targets as files are added.
7. Verify launch/show/hide/resize, frame updates, readback, audio integration,
   transcript/overlay drawing, face-box interaction, device loss where
   testable, and repeated destruction/recreation.
8. Continue until direct_vulkan_preview_window.cpp and all extracted files are
   below 1,500 lines, preferably 800-1,200.
9. Run focused tests, available Vulkan validation/parity checks, the full
   relevant suite, git diff --check, python countlines.py, and the redundancy
   scanner. Update planning/ownership documents and provide a complete handoff.

Do not stop at a plan. Implement and verify the entire direct-preview scope
unless concretely blocked.
```

### Exit gate before Prompt 5

- The direct-preview window and its extracted files meet the size cap.
- Existing direct-preview modules are canonical and have no newly introduced
  parallel logic.
- Lifecycle, readback, resize, interaction, and presentation checks pass.

## Prompt 5: Vulkan Text Renderer

```text
Implement the Vulkan text-renderer portion of Phase 3 in REFACTOR_PLAN.md using
OWNERSHIPMATRIX.md as the authoritative ownership guide and
FILESIZE_REDUCTION_STRATEGY.md as the size policy.

Scope this session to vulkan_text_renderer.cpp and the private text-rendering
modules needed to separate fonts, pipelines, atlases, speaker labels,
transcripts, and titles. Preserve the ownership seams introduced by commit
073eb8b.

Requirements:

1. Read all refactor documents, prior handoffs, recent relevant Git history,
   and applicable AGENTS.md instructions. Do not create a Git branch. Preserve
   unrelated and uncommitted user changes.
2. Use ./build.sh from the repository root for every build and build
   verification. Do not invoke CMake, Ninja, Make, or other underlying build
   tools directly. Report every exact ./build.sh command and option used.
3. Complete and record the ownership audit for every extraction. Inspect
   title_mesh_extrusion.*, overlay_text_style.*,
   transcript_overlay_cache_key.*, direct_vulkan_preview_transcript.*,
   cpu_overlay_render_backend.*, and existing renderer interfaces first.
4. Keep title mesh geometry in title_mesh_extrusion.* and shared
   title/transcript styling in overlay_text_style.*. Keep serialization in
   clip_serialization.cpp and public transcript cache-key policy in
   transcript_overlay_cache_key.*. Do not duplicate these responsibilities in
   renderer modules.
5. Extract FreeType/font work, Vulkan pipeline work, atlas construction/upload,
   speaker layout, transcript layout, and title layout along the boundaries in
   OWNERSHIPMATRIX.md. Keep renderer lifetime and top-level dispatch in
   vulkan_text_renderer.cpp.
6. Use a narrow private internal boundary. Do not expose FreeType handles,
   Vulkan implementation state, atlas internals, or renderer caches through
   public UI/domain headers merely to enable the split.
7. Treat the first pass as behavior-preserving. Preserve font fallback,
   wrapping, glyph metrics, atlas/cache keys, descriptor and buffer lifetime,
   upload synchronization, draw ordering, flat title rendering, 3D title
   transforms, extrusion, transcript styling, and speaker labels.
8. Work in small compile-safe batches. After each batch, run ./build.sh with
   the appropriate supported options and the focused tests; fix introduced
   failures before continuing.
9. Continue until vulkan_text_renderer.cpp and every extracted file are below
   1,500 lines, preferably 800-1,200.
10. Run test_vulkan_text_generation, available direct/offscreen render parity
    coverage, focused/full relevant tests, git diff --check,
    python countlines.py, and scripts/scan_redundant_code.sh. Do not overwrite
    the user's uncommitted test_vulkan_direct_render_parity.cpp changes.
11. Update REFACTOR_PLAN.md and OWNERSHIPMATRIX.md with completed audit rows and
    evidence-based boundary changes.
12. Finish with source-to-destination symbol groups, canonical owners reused,
    duplicates removed, final line counts, exact ./build.sh commands/results,
    text/render parity evidence, and remaining hardware-dependent risks.

Do not stop after analysis. Implement and verify the entire Vulkan text scope
unless a concrete blocker prevents safe completion.
```

### Exit gate before Prompt 6

- `vulkan_text_renderer.cpp` and its extracted files meet the size cap.
- Title mesh geometry and overlay styling remain in their established owners.
- Text-generation and available rendering-parity tests pass.
- Font, atlas, pipeline, transcript, speaker, and title responsibilities have
  one canonical owner each.

## Prompt 6: ImGui Application Shell

```text
Implement Phase 4 of REFACTOR_PLAN.md using OWNERSHIPMATRIX.md as the
authoritative ownership guide and FILESIZE_REDUCTION_STRATEGY.md as the size
policy.

Scope this session to jcut_imgui_main.cpp and the private ImGui shell modules
needed to extract platform setup, preferences, runtime-control adapters,
preview/export workers, and UI panels.

Requirements:

1. Read all refactor documents, prior handoffs, and applicable AGENTS.md
   instructions. Do not create a Git branch. Preserve unrelated changes.
2. Complete and record the ownership audit for every extraction. Search
   existing platform, runtime-control, document serialization, preview,
   rendering, and editor modules before creating destinations.
3. Keep protocol handling in existing control-server modules and document
   serialization in existing document JSON/I/O modules. New ImGui files own
   shell adapters and presentation, not duplicate services.
4. Keep ShellState and ShellLayout behind a narrow private internal header.
   Avoid introducing application-global state or public shell APIs.
5. Preserve startup/shutdown order, worker cancellation/join behavior, Vulkan
   and ImGui lifetime, preference compatibility, dirty-document behavior,
   command dispatch, and runtime-control semantics.
6. Extract one responsibility/panel at a time, updating CMake and running the
   narrowest build and focused smoke tests after every batch.
7. Verify launch/shutdown, preferences and font persistence, media import,
   document save/reload, play/seek/edit, preview refresh, export/cancel,
   runtime-control screenshots/playhead, and all extracted panels.
8. Continue until jcut_imgui_main.cpp and all extracted files are below 1,500
   lines, with the main file preferably below 800 lines.
9. Run focused and full relevant tests, git diff --check,
   python countlines.py, and scripts/scan_redundant_code.sh. Update the
   planning and ownership documents and provide the required complete handoff.

Do not stop after analysis. Implement and verify this entire shell scope unless
a concrete blocker prevents completion.
```

### Exit gate before Prompt 7

- The ImGui main file is below 1,500 lines and contains only startup, event
  loop, top-level orchestration, and shutdown.
- Worker cancellation, persistence, document operations, preview, export, and
  runtime control pass focused checks.

## Prompt 7: Face-Detection Offscreen Runner

```text
Implement the face-detection runner portion of Phase 5 in REFACTOR_PLAN.md
using OWNERSHIPMATRIX.md as the authoritative ownership guide and
FILESIZE_REDUCTION_STRATEGY.md as the size policy.

Scope this session to vulkan_facedetections_offscreen_runner.cpp and existing
offscreen face-detection modules required to reduce it to pipeline
orchestration.

Requirements:

1. Read the three refactor documents,
   vulkan_facedetections_offscreen_modularization.md, previous handoffs, and
   applicable AGENTS.md instructions. Reconcile conflicts in the planning
   documents before editing. Do not create a Git branch.
2. Preserve unrelated changes and complete/record a pre-move ownership audit
   for every extraction.
3. Inspect and reuse the existing options, Vulkan harness, filters, tuning,
   resume state, checkpoint writer, artifact I/O, preview I/O, progress, and
   benchmark modules. Do not create replacements for their responsibilities.
4. Keep detection and tracking algorithms in their existing domain services.
   Runner-local code may adapt data and sequence stages but must not duplicate
   those algorithms.
5. Create a decode/frame-iteration module only after proving that no existing
   decoder service is an appropriate owner.
6. Preserve CLI arguments, validation messages, exit codes, cancellation,
   progress output, checkpoint compatibility, resume behavior, artifact
   schemas, and deterministic ordering.
7. Work in small compile-safe batches and verify CLI and artifact behavior
   after each batch.
8. Continue until the runner and every extracted file are below 1,500 lines,
   preferably leaving the runner below 800 lines.
9. Verify invalid/valid CLI invocations, clean processing, cancellation, resume
   after interruption, empty input, checkpoint compatibility, and artifact
   JSON equivalence. Run focused/full relevant tests, git diff --check,
   python countlines.py, and the redundancy scanner.
10. Update planning/ownership records and report exact behavior/parity evidence
    and any hardware-dependent gaps.

Do not stop after analysis. Implement and verify the full runner scope unless
concretely blocked.
```

### Exit gate before Prompt 8

- The runner is below the size cap and primarily sequences existing modules.
- CLI compatibility, cancellation/resume, and artifact equivalence pass.
- Existing modules own options, harness, filtering, tuning, persistence, and
  progress without parallel implementations.

## Prompt 8: Keyframes and Speaker Framing

```text
Implement the keyframe and speaker-framing portion of Phase 5 in
REFACTOR_PLAN.md using OWNERSHIPMATRIX.md as the authoritative ownership guide
and FILESIZE_REDUCTION_STRATEGY.md as the size policy.

Scope this session to editor_shared_keyframes.cpp and the keyframe, cache,
continuity-artifact, timing, and speaker-section modules required to establish
canonical ownership.

Requirements:

1. Read all refactor documents, prior handoffs, and applicable AGENTS.md
   instructions. Do not create a Git branch. Preserve unrelated changes.
2. Complete and record the ownership audit for each extraction. Inspect
   editor_transform_keyframe_ops.*, editor_title_opacity_keyframe_ops.*,
   editor_shared_keyframes_cache.*, facedetections_continuity_artifacts.*,
   editor_shared_timing.*, and the canonical speaker-section assignment helper
   created or selected in Prompt 1.
3. Command/mutation logic remains in existing editor ops modules. Artifact
   parsing remains in artifact modules. Timing and section JSON parsing use
   their canonical owners; do not reintroduce local copies.
4. Add table-driven characterization tests before moving evaluation logic.
   Cover empty tracks, a single keyframe, duplicate frames, fractional
   positions, before/after bounds, interpolation modes, source lock, grading,
   opacity, titles, scale constraints, rotation wraparound, and speaker
   continuity fallbacks.
5. Separate generic transform/grading/opacity/title evaluation from
   speaker-framing continuity and retargeting. Generic keyframe modules must
   not depend on face-detection artifacts after the split.
6. Preserve cache keys, warm-up behavior, thread/GUI constraints, smoothing,
   fallback semantics, and render results. Treat extraction and behavior
   redesign as separate work.
7. Work in small compile-safe batches with focused tests after every move.
8. Continue until editor_shared_keyframes.cpp and every destination are below
   1,500 lines, preferably 800-1,200.
9. Run focused keyframe/render parity tests, the full relevant suite,
   git diff --check, python countlines.py, and the redundancy scanner. Update
   planning/ownership records and provide a complete handoff.

Do not stop after analysis. Implement and verify the entire scoped phase unless
a concrete blocker prevents safe completion.
```

### Exit gate before Prompt 9

- Generic keyframes and speaker framing have separate dependency boundaries.
- Evaluation and render parity tests pass.
- Section parsing, timing, caches, artifacts, and mutation commands each have
  one canonical owner.

## Prompt 9: Remaining P1/P2 Sweep

```text
Complete Phase 6 of REFACTOR_PLAN.md using OWNERSHIPMATRIX.md as the
authoritative ownership guide and FILESIZE_REDUCTION_STRATEGY.md as the size
policy.

This is a planning-and-implementation sweep over the files that still exceed
the 1,500-line cap after Prompts 1-8. Work one cohesive domain at a time; do not
perform a repository-wide mechanical split.

Requirements:

1. Read all refactor documents and prior handoffs, inspect the current
   worktree, run python countlines.py, and produce the current ordered list of
   oversized files. Read and obey applicable AGENTS.md instructions. Do not
   create a Git branch.
2. Reconcile the current list with the P1/P2 backlog in REFACTOR_PLAN.md.
   Remove completed targets, add newly oversized targets, and prioritize by
   size, churn, coupling, and regression risk.
3. Select the next cohesive domain and complete the ownership audit before
   editing. If the remaining scope spans multiple high-risk domains, finish
   the safest bounded domain and leave explicit successive prompts for the
   others rather than combining them into one unreviewable change.
4. Prefer existing owners, consolidate tested duplicates, keep orchestration
   thin, and create new modules only where OWNERSHIPMATRIX.md and repository
   evidence show an ownership gap.
5. Keep extractions behavior-preserving and compile-safe. Build and run focused
   tests after every batch; never defer a growing failure set until the end.
6. Continue through the selected domain until all its files meet the cap.
   Repeat for another domain only when context and verification remain clear.
7. For each completed domain, run focused and full relevant tests,
   git diff --check, python countlines.py, and
   scripts/scan_redundant_code.sh. Use domain-specific parity or sanitizer
   checks where appropriate.
8. Update REFACTOR_PLAN.md and OWNERSHIPMATRIX.md with the current baseline,
   completed audit rows, ownership changes, remaining oversized files, and
   exact next scope.
9. Finish with completed domains, moved symbol groups, existing owners reused,
   duplicates removed, before/after line counts, test evidence, outstanding
   oversized files, and the exact recommended next prompt if work remains.

Do not merely report the remaining files. Implement and verify at least one
complete cohesive domain unless a concrete blocker prevents any safe progress.
```

## Final Repository Exit Gate

The large-file refactor is complete when:

1. no source or header exceeds the 1,500-line hard cap;
2. high-churn core files are preferably 800-1,200 lines;
3. every responsibility has one canonical owner recorded in
   `OWNERSHIPMATRIX.md`;
4. known duplicate implementations have been removed after equivalence tests;
5. public APIs were not broadened merely to enable file splitting;
6. all builds, focused tests, full relevant tests, parity checks, and supported
   sanitizer/validation runs pass;
7. `git diff --check`, `python countlines.py`, and
   `scripts/scan_redundant_code.sh` pass; and
8. the size guard prevents changed files from growing back beyond the policy.
