# OpenGL Deprecation and Removal Plan

## Goal

Make Vulkan the normal preview/render path and keep OpenGL only as a temporary compatibility fallback until Vulkan has full behavior parity, test coverage, and operational reliability.

Backend choice should affect performance and hardware compatibility, not application behavior.

## Intermediate Compatibility State

During the migration, both backends must continue to work with acceptable performance:

- OpenGL remains a working compatibility backend until Vulkan parity gates pass.
- Shared preview state/controller code must be used by both backends.
- The migration must not introduce a heavy translation layer in the OpenGL path.
- Vulkan consumes shared state through a direct `QVulkanWindow` presenter; offscreen `QImage` bridge paths are not valid Vulkan preview paths.
- Each extraction slice must keep `editor` building and preserve the FaceStream wrapper smoke path.

This intermediate state is temporary. It exists to avoid breaking mature preview behavior while Vulkan becomes the normal interactive presenter.

## Current State

OpenGL is still necessary because the existing `PreviewWindow` path owns mature preview behavior:

- Timeline preview presentation
- Selection and transform interaction
- Overlay drawing and hit testing
- Transcript overlay interaction
- Speaker track overlays
- Correction drawing
- Zoom and pan behavior
- Audio/waveform preview UI behavior

Vulkan currently owns a direct swapchain presenter and FaceStream wrapper paths, but Vulkan is not yet the full interactive timeline composition implementation.

The obsolete `QVulkanWindow` scaffold was removed because it performed an offscreen Vulkan render, read back to `QImage`, then uploaded again to a Vulkan swapchain. The replacement direct presenter owns the swapchain and records commands directly into it.

## Target Architecture

```text
PreviewSurface
  App-facing preview API and lifecycle

PreviewController / InteractionModel
  Shared selection, hit testing, overlay state, zoom/pan, transcript interaction

RenderBackend
  Vulkan renderer as default
  OpenGL renderer as fallback during transition

FaceStreamGenerator
  Shared Generate FaceStream service used by the program and test wrappers

PreviewPresenter
  Vulkan presenter when available
  OpenGL presenter only while fallback remains supported
```

The OpenGL path must stop being the owner of interaction behavior before it can be removed.

## Deprecation Principles

- Do not remove OpenGL until Vulkan has full user-visible behavior parity.
- Do not maintain two independent interaction implementations.
- Do not add new features only to OpenGL.
- New preview/render features should target shared controller/state code plus Vulkan first.
- OpenGL bug fixes are allowed only when they protect users during the transition.
- Any CPU readback path must be explicit in logs/stats and justified as preview/export/debug only.

## Phase 1: Stabilize Vulkan As The Primary Backend

Status: in progress.

Requirements:

- Vulkan is the default preview backend when available.
- Backend factory reports clear requested/effective backend decisions.
- Vulkan preview presentation uses the direct swapchain presenter; FaceStream verification uses Vulkan image handles without `QImage` materialization by default.
- FaceStream test wrapper uses the same native Generate FaceStream path as the program.
- Obsolete offscreen-readback `QVulkanWindow` scaffold remains removed.
- Vulkan render stats distinguish decode, texture, composite, readback, and detector stages.
- FaceStream preview/debug readback is explicitly opt-in and throttled with `--preview-stride`; normal detector throughput should be profiled with preview output disabled.
- The offscreen FaceStream harness supports live detector tuning through `--params-file` for threshold, stride, candidate caps, ROI, area, and aspect filters.

Exit gates:

- `editor` builds cleanly.
- `jcut_vulkan_boxstream_offscreen` builds and runs on the reference video.
- Vulkan preview fallback decisions are visible in logs.
- Preview/readback timing is visible in the offscreen summary when preview output is enabled.
- No stale `QVulkanWindow` scaffold references remain.

## Phase 2: Extract Shared Preview Interaction

Status: started.

Move interaction and overlay behavior out of the OpenGL widget into backend-neutral code.

Extract or centralize:

- Preview state model
- Current frame/sample state
- Clip hit testing
- Selection bounds and handles
- Transform/correction interaction
- Transcript overlay interaction
- Speaker overlay visibility and hover state
- Zoom and pan model
- Overlay draw command generation

Initial extraction completed:

- Added `PreviewInteractionState` for backend-neutral preview interaction state.
- Moved selected clip id, view mode, preview zoom/pan, correction draw mode, transcript interaction, and title-only interaction flags out of direct OpenGL widget member storage.
- Moved playhead frame/sample/position, playback flag, audio mute/volume, output size, and background color into the same shared state object.
- Moved timeline clip count, clips, tracks, render sync markers, and export ranges into the shared state object.
- Added `PreviewOverlayModel` for backend-neutral overlay metadata and paint order.
- Moved `PreviewOverlayInfo`, `PreviewOverlayKind`, overlay map, and paint order out of OpenGL-specific type ownership.
- Moved transient interaction state into the shared model: drag mode/origin, transcript drag origin, correction draft points, speaker-pick drag state, and last mouse position.
- `PreviewWindow` still consumes this state, but the state type is no longer OpenGL-specific.

Exit gates:

- OpenGL preview uses the shared controller/model.
- Vulkan preview uses the same controller/model.
- No interaction behavior is implemented only inside OpenGL-specific code unless it is truly OpenGL rendering glue.

## Phase 3: Vulkan Interactive Preview Parity

Implement Vulkan presentation that consumes shared preview state and render output without becoming a separate UI implementation.

Requirements:

- Same selection behavior as OpenGL.
- Same transcript interaction behavior as OpenGL.
- Same correction drawing behavior as OpenGL.
- Same speaker overlay behavior as OpenGL.
- Same zoom/pan behavior as OpenGL.
- Same FaceStream overlay preview behavior as Generate FaceStream.
- Render path avoids unnecessary `QImage` readback in normal preview operation.

Exit gates:

- Manual parity checklist passes on representative projects.
- Automated or smoke tests cover backend selection and representative preview state transitions.
- Vulkan failure falls back cleanly to OpenGL with a clear reason.

## Phase 4: OpenGL Deprecation Warning

Once Vulkan parity exists, keep OpenGL as an opt-in fallback for one transition period.

Behavior:

- Vulkan remains default.
- OpenGL can be selected explicitly by environment/config only.
- If OpenGL is selected, log a deprecation warning.
- Any new preview features must not be implemented OpenGL-only.

Example log:

```text
[preview-backend] OpenGL fallback is deprecated and will be removed after Vulkan parity burn-in.
```

Exit gates:

- Vulkan is stable across normal editing sessions.
- Vulkan handles FaceStream preview and generation workflows.
- No high-severity Vulkan-only preview regressions remain open.

## Phase 5: Remove OpenGL Fallback

Remove OpenGL only after the previous gates are satisfied.

Removal scope:

- OpenGL preview widget implementation
- OpenGL-specific preview shaders/resources
- OpenGL fallback factory branches
- OpenGL-only debug toggles that no longer apply
- OpenGL-only tests that do not validate shared behavior

Keep only if still needed:

- Tiny compatibility utilities unrelated to preview
- Tests comparing archived behavior, if useful
- Documentation explaining the migration

Exit gates:

- `editor` builds without OpenGL preview sources.
- Runtime backend selection has no OpenGL branch.
- Vulkan preview and FaceStream workflows pass smoke tests.
- Headless/offscreen workflows still work through Vulkan offscreen or null/software test paths.

## Minimum Test Matrix Before Removal

- Normal editor startup with Vulkan default.
- Project load with existing clips and transcript overlays.
- Timeline scrubbing.
- Clip selection and transform handles.
- Transcript overlay selection/hit testing.
- Correction drawing/editing.
- Speaker track points/boxes overlay.
- Generate FaceStream native hybrid Vulkan path.
- FaceStream offscreen wrapper on reference video.
- Vulkan unavailable/failure behavior before OpenGL removal.
- Headless/offscreen CI smoke path.

## Non-Goals

- Reintroducing the removed `QVulkanWindow` scaffold.
- Maintaining independent Vulkan and OpenGL interaction implementations.
- Calling a path zero-copy when it performs hidden CPU readback.
- Keeping OpenGL indefinitely as a comfort fallback after Vulkan parity is complete.

## Current Recommendation

Keep OpenGL for now, but treat it as deprecated-in-waiting. The next engineering priority is not more OpenGL work; it is extracting shared interaction state and making Vulkan the normal interactive preview implementation.
