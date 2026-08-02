# Ownership and Refactor Completion Plan

This document is the implementation checklist for the ownership audit. A phase
is complete only when its acceptance checks pass; line movement alone is not
completion.

Baseline on 2026-08-02: `./build.sh` passed on `main` at `763e954`.

## Phase 1 — Enforceable ownership metadata

- [ ] Add a machine-readable ownership manifest covering first-party code.
- [ ] Enrich the generated structure index with `declared_owner`, `layer`,
  `build_targets`, and per-file ownership violations.
- [ ] Add regression tests for ownership resolution and violations.
- [ ] Make missing or ambiguous production ownership fail validation.

Acceptance: the structure generator reports complete declared ownership and can
identify target duplication from an existing build graph.

## Phase 2 — One build owner per implementation

- [ ] Eliminate `EDITOR_ALL_CPP` and the root-level source glob.
- [ ] Give every production `.cpp` one owning library or executable.
- [ ] Link consumers to owning libraries rather than recompiling sources.
- [ ] Keep executable targets limited to their entry point and genuinely
  executable-specific adapters.

Acceptance: no production `.cpp` appears in more than one non-test target in
`build/build.ninja`, apart from an explicitly documented empty exception list.

## Phase 3 — Canonical document ownership

- [ ] Declare `EditorDocumentCore` as the canonical persisted editor model.
- [ ] Restrict `TimelineClip`/`TimelineTrack` to a renderer and legacy-Qt
  projection boundary.
- [ ] Route durable save/load/history through the canonical model.
- [ ] Remove duplicated mutation policy from projection code.

Acceptance: canonical mutations and persistence are owned by
`EditorDocumentCore`/`EditorRuntime`; projections cannot become an independent
source of truth.

## Phase 4 — Thin executable and command dispatch

- [ ] Reduce executable translation units to startup and delegation.
- [ ] Split ImGui shell, platform host, panels, and workflow controllers out of
  `jcut_imgui_main.cpp`.
- [ ] Extract command-family handlers from `EditorRuntime::execute()` while
  retaining one document/history owner.

Acceptance: entry points contain process setup only, and no individual command
dispatcher or UI panel function remains a multi-thousand-line owner.

## Phase 5 — UI and domain ownership

- [ ] Move speaker assignment mutation out of `tracks.cpp` UI code and into the
  speaker assignment/document service.
- [ ] Move inspector tab construction into tab-specific implementation files.
- [ ] Leave `InspectorPane` responsible only for pane/tab registration and
  wiring.

Acceptance: UI code issues domain operations through narrow services, and the
inspector facade no longer constructs feature-complete tabs itself.

## Phase 6 — Renderer and audio internals

- [ ] Split offscreen Vulkan initialization, frame composition, conversion,
  text preparation, and preview publication behind one renderer lifecycle.
- [ ] Split direct Vulkan window interaction from renderer recording.
- [ ] Split audio device control, decode/cache work, mixing, and profiling while
  retaining one audio-device owner.
- [ ] Keep standalone renderers responsible for backend mechanics and consume
  shared render plans/policies.

Acceptance: resource lifetime remains singular, focused parity tests pass, and
the formerly oversized implementation owners are split by responsibility.

## Final verification

- [ ] Regenerate and validate the ownership/structure artifact.
- [ ] Run focused ownership, domain, speaker, inspector, render, Vulkan, audio,
  and ImGui tests.
- [ ] Run `./build.sh` successfully.
- [ ] Record exact verification commands and remaining physical-only acceptance
  gates, if any.
