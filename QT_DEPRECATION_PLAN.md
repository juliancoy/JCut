# Qt Deprecation Plan

## Goal

Reduce Qt usage in the runtime and rendering stack without destabilizing preview, export, or the editor shell.

This is not a single-step "remove Qt" rewrite. The codebase still uses Qt for:

- UI widgets and windowing
- event loop, timers, and signal/slot messaging
- IPC wrappers
- strings, containers, and JSON
- image interchange and some preview compatibility paths
- parts of title/transcript data models

The important update is that the codebase is no longer at the "everything render-related is Qt-first" stage. Some of the hardest groundwork is already in progress.

## Current Snapshot

As of the current tree:

- renderer output is partially dual-path, not purely `QImage`-first
- overlay rendering now has an explicit backend abstraction
- the non-Qt overlay backend already uses `fontconfig` + `freetype2`
- Vulkan facedetections/offscreen paths already emit zero-copy and fallback telemetry
- targeted tests already exist for Vulkan subtitle/render parity work

That means the plan should focus on finishing boundary cleanup, not re-describing already-completed groundwork as future work.

## What Is Already True

### 1. Renderer output has started moving away from a pure `QImage` contract

Relevant files:

- [render_internal.h](/home/julian/Documents/JCut/render_internal.h:205)
- [facedetections_runtime.h](/home/julian/Documents/JCut/facedetections_runtime.h:24)
- [facedetections_runtime.cpp](/home/julian/Documents/JCut/facedetections_runtime.cpp:106)

Current status:

- `render_detail::OffscreenRenderFrame` carries both `QImage cpuImage` and `OffscreenVulkanFrame vulkanFrame`
- `OffscreenRenderer::renderFrameToOutput(...)` already exists beside the legacy `QImage renderFrame(...)` convenience API
- facedetections runtime has `VulkanRenderResult` and `renderFrameToVulkan(...)`-style entry points

Implication:

- the architecture has already started the right migration
- the remaining job is to make the GPU-first path the default contract and demote `QImage` to compatibility/output only

### 2. Overlay generation is no longer purely Qt text rendering

Relevant files:

- [cpu_overlay_render_backend.h](/home/julian/Documents/JCut/cpu_overlay_render_backend.h:12)
- [cpu_overlay_render_backend.cpp](/home/julian/Documents/JCut/cpu_overlay_render_backend.cpp:1)
- [titles.cpp](/home/julian/Documents/JCut/titles.cpp:106)
- [render_decode.cpp](/home/julian/Documents/JCut/render_decode.cpp:237)

Current status:

- overlay generation now goes through `OverlayRenderBackend`
- title/transcript overlay generation can return a neutral `OverlayImage` byte buffer
- the backend implementation already uses FreeType/fontconfig rather than `QTextDocument`
- `QImage` still appears at leaf compatibility boundaries via explicit overlay-image views for painter-based composition

Implication:

- the main remaining problem is not "invent a non-Qt overlay renderer from scratch"
- the real task is to push `OverlayImage`/GPU upload paths deeper and stop converting back to `QImage` unless a legacy caller explicitly needs it

### 3. Zero-copy and fallback observability already exist

Relevant files:

- [vulkan_facedetections_offscreen_main.cpp](/home/julian/Documents/JCut/vulkan_facedetections_offscreen_main.cpp:4200)
- [vulkan_facedetections_offscreen_main.cpp](/home/julian/Documents/JCut/vulkan_facedetections_offscreen_main.cpp:4336)

Current status:

- facedetections summary output already records hardware/direct handoff, CPU fallback, `qimage_materialized`, and zero-copy satisfaction flags
- strictness knobs already exist such as `require_zero_copy` and `require_hardware_vulkan_frame_path`

Implication:

- Stage 0 should build on this instrumentation instead of starting from scratch

### 4. The build and test graph already reflects the migration

Relevant files:

- [CMakeLists.txt](/home/julian/Documents/JCut/CMakeLists.txt:99)
- [tests/CMakeLists.txt](/home/julian/Documents/JCut/tests/CMakeLists.txt:41)

Current status:

- `fontconfig` and `freetype2` are first-class dependencies
- `test_vulkan_subtitle_render` exists
- `test_opengl_vulkan_render_exactness` includes the overlay backend path

Implication:

- this work should now be driven by boundary enforcement and regression coverage, not just exploratory refactors

## Remaining High-Risk Areas

### 1. Public runtime/render interfaces still leak Qt types

Examples:

- `QImage` remains part of renderer and runtime compatibility APIs
- `QString`, `QHash`, `QVector`, `QJsonObject`, and `QSize` are still used throughout render-facing interfaces

Relevant files:

- [render_internal.h](/home/julian/Documents/JCut/render_internal.h:222)
- [facedetections_runtime.h](/home/julian/Documents/JCut/facedetections_runtime.h:45)
- [preview_surface.h](/home/julian/Documents/JCut/preview_surface.h:134)

Impact:

- even when the internal render path is GPU-native, boundary contracts still pin adjacent systems to Qt
- replacing the editor shell alone would not remove these dependencies

### 2. Vulkan composition still keeps CPU image compatibility paths alive

Examples:

- prepared image caches still use `QImage`
- some layer preparation and compatibility paths can still materialize CPU images
- readback helpers remain widely available and easy to call

Relevant files:

- [offscreen_vulkan_renderer.cpp](/home/julian/Documents/JCut/offscreen_vulkan_renderer.cpp:3155)
- [facedetections_runtime.h](/home/julian/Documents/JCut/facedetections_runtime.h:80)
- [render_internal.h](/home/julian/Documents/JCut/render_internal.h:214)

Impact:

- zero-copy is available, but not yet the default discipline
- performance regressions can still creep in through convenience code paths

### 3. Overlay composition still falls back through Qt image helpers in legacy paths

Examples:

- painter-based leaf compatibility paths still expose overlay bytes to Qt as `QImage` views
- some preview/export leaf consumers still terminate in Qt image composition

Relevant files:

- [render_decode.cpp](/home/julian/Documents/JCut/render_decode.cpp:237)
- [titles.h](/home/julian/Documents/JCut/titles.h:57)
- [cpu_overlay_render_backend.h](/home/julian/Documents/JCut/cpu_overlay_render_backend.h:29)

Impact:

- overlay generation itself is less coupled to Qt than before
- legacy composition APIs still preserve Qt at the boundary

### 4. QtCore remains pervasive across non-UI subsystems

Examples:

- strings, JSON, containers, ownership, and event assumptions remain Qt-shaped

Relevant files:

- [editor.h](/home/julian/Documents/JCut/editor.h:346)
- [editor_editor_pane.cpp](/home/julian/Documents/JCut/editor_editor_pane.cpp:122)
- [editor_inspector_bindings.cpp](/home/julian/Documents/JCut/editor_inspector_bindings.cpp:530)
- [transcript_engine.cpp](/home/julian/Documents/JCut/transcript_engine.cpp:96)

Impact:

- full de-Qt remains a large cross-cutting effort
- this should still come after render/runtime boundaries are stabilized

## Revised Priorities

### Priority 1: Finish the renderer boundary transition

Primary files:

- [render_internal.h](/home/julian/Documents/JCut/render_internal.h)
- [facedetections_runtime.h](/home/julian/Documents/JCut/facedetections_runtime.h)
- [facedetections_runtime.cpp](/home/julian/Documents/JCut/facedetections_runtime.cpp)

Tasks:

- make `OffscreenRenderFrame`/GPU-native output the primary contract
- keep `QImage renderFrame(...)` only as a compatibility wrapper
- make readback explicit at the call site rather than implicit in the default API
- reduce render-facing public interfaces that expose `QImage` directly

Exit criteria:

- new render/runtime call sites do not need `QImage`
- readback is opt-in and easy to audit

### Priority 2: Eliminate silent CPU fallback in Vulkan mode

Primary files:

- [offscreen_vulkan_renderer.cpp](/home/julian/Documents/JCut/offscreen_vulkan_renderer.cpp)
- [vulkan_facedetections_offscreen_main.cpp](/home/julian/Documents/JCut/vulkan_facedetections_offscreen_main.cpp)

Tasks:

- keep current telemetry
- add stricter assertions or test coverage around "no unexpected materialization"
- fail loudly in strict Vulkan modes when a path would fall back through CPU image materialization
- surface per-frame fallback reasons in preview/export diagnostics where useful

Exit criteria:

- strict Vulkan mode cannot silently succeed after a CPU bounce
- regressions are visible in tests or run summaries

### Priority 3: Finish the overlay boundary cleanup

Primary files:

- [cpu_overlay_render_backend.h](/home/julian/Documents/JCut/cpu_overlay_render_backend.h)
- [render_decode.cpp](/home/julian/Documents/JCut/render_decode.cpp)
- [titles.h](/home/julian/Documents/JCut/titles.h)
- [offscreen_vulkan_renderer.cpp](/home/julian/Documents/JCut/offscreen_vulkan_renderer.cpp)

Tasks:

- push `OverlayImage` as the main overlay interchange type
- confine `QImage` conversion helpers to legacy callers
- remove `QPainter` composition from paths that can consume raw RGBA overlay buffers directly
- keep title/transcript layout/rendering behind the backend interface

Exit criteria:

- Vulkan preview/export overlay flow does not require `QPainter`
- `QImage` overlay helpers are compatibility shims, not primary APIs

### Priority 4: Extract preview/runtime shell boundaries

Primary files:

- [preview_surface.h](/home/julian/Documents/JCut/preview_surface.h)
- [editor_pane.cpp](/home/julian/Documents/JCut/editor_pane.cpp:163)
- [editor_editor_pane.cpp](/home/julian/Documents/JCut/editor_editor_pane.cpp:122)
- [direct_vulkan_preview_presenter.cpp](/home/julian/Documents/JCut/direct_vulkan_preview_presenter.cpp)

Tasks:

- separate preview runtime state from widget ownership
- isolate interaction callbacks from QWidget lifecycle
- keep existing Qt preview shells behind an abstract runtime boundary

Exit criteria:

- preview runtime services can be driven without hard QWidget coupling

### Priority 5: De-Qt IPC and core utility boundaries

Primary files:

- [vulkan_facedetections_offscreen_main.cpp](/home/julian/Documents/JCut/vulkan_facedetections_offscreen_main.cpp)
- runtime/editor boundary modules that currently exchange `QString`/`QJsonObject`/`QVector`

Tasks:

- abstract `QLocalSocket`/`QLocalServer` usage
- convert subsystem boundaries before converting internal implementation details
- decide where neutral types are actually worth the churn versus where QtCore can remain tolerated

Exit criteria:

- core runtime/render modules compile with sharply reduced QtCore surface area

## Migration Stages

## Stage 0: Inventory, Metrics, and Guardrails

Goal:

Measure the remaining Qt-dependent boundaries and lock in strict definitions for fallback behavior.

Tasks:

- inventory remaining Qt use by category:
  - QtWidgets/UI shell
  - eventing/scheduling
  - IPC
  - image/render interchange
  - text/layout/rendering
  - core/container types
- document which render paths are allowed to read back and which are not
- reuse existing facedetections metrics as the baseline for zero-copy/fallback reporting
- add tests or assertions for "unexpected `QImage` materialization"

Exit criteria:

- remaining Qt surface is categorized by subsystem
- zero-copy and compatibility-mode behavior are explicitly defined

## Stage 1: GPU-First Renderer Boundary

Goal:

Make GPU/native frame interchange the default render contract.

Tasks:

- promote `renderFrameToOutput(...)`-style APIs over legacy `QImage` APIs
- trim `QImage` from render/runtime boundaries wherever a GPU frame or neutral buffer is enough
- make CPU readback an explicit compatibility/output request

Exit criteria:

- primary renderer/runtime APIs no longer assume a CPU image result

## Stage 2: Overlay Boundary Cleanup

Goal:

Finish the transition already started by `OverlayRenderBackend`.

Tasks:

- treat `OverlayImage` as the main overlay payload
- remove legacy Qt-only composition assumptions from Vulkan-capable paths
- keep font loading, shaping, and rasterization behind the backend interface

Exit criteria:

- overlay generation and Vulkan composition do not require `QPainter`, `QTextDocument`, or `QImage` as primary types

## Stage 3: Preview Runtime Extraction

Goal:

Decouple runtime preview services from the Qt widget shell.

Tasks:

- separate widget ownership from preview/render orchestration
- isolate interaction and presentation contracts
- preserve the current Qt shell as one backend during the transition

Exit criteria:

- preview runtime can be hosted by a non-Qt shell without render-path changes

## Stage 4: IPC and QtCore Reduction

Goal:

Reduce pervasive reliance on QtCore at subsystem boundaries.

Tasks:

- abstract IPC transport
- progressively replace or wrap boundary-facing `QString`, `QVector`, `QHash`, `QJsonObject`
- leave deep internal substitutions for later unless a boundary refactor already touches them

Exit criteria:

- runtime/render subsystems are no longer broadly pinned to QtCore public types

## Stage 5: Editor Shell Replacement

Goal:

Make the UI framework swappable after runtime/render boundaries are stable.

Tasks:

- rehost panels, timeline interactions, preview controls, and settings editors behind stable runtime services
- preserve rendering/runtime behavior beneath the shell swap

Exit criteria:

- the shell can move off QtWidgets without reopening renderer/runtime architecture

## Recommended Order Of Work

1. Finish promoting GPU-first frame contracts over `QImage` wrappers
2. Enforce strict no-silent-fallback behavior in Vulkan paths
3. Finish overlay boundary cleanup around `OverlayImage`
4. Extract preview runtime services from Qt widget ownership
5. Reduce IPC and QtCore boundary dependence
6. Replace the editor shell last

## What Is Easier Now Than Before

- overlay generation no longer depends on inventing a brand-new non-Qt text stack
- zero-copy observability already exists
- renderer output already has a dual-path shape
- regression tests for Vulkan subtitle/render behavior already exist

## What Is Still Hard

- removing `QImage` from public renderer/runtime boundaries without destabilizing callers
- preventing convenience helpers from reintroducing silent CPU bounces
- shrinking broad QtCore type usage without creating a long-lived half-converted API mess
- replacing the shell without touching unstable runtime boundaries

## ImGui Assessment

ImGui remains viable as a future shell, but it is still not the first move.

The reason is the same as before, but the details are sharper now:

- QtWidgets is no longer the dominant render blocker
- the real remaining blockers are boundary types, fallback discipline, and QtCore-heavy subsystem contracts
- shell replacement should wait until preview/runtime services are stable and render APIs are GPU-first by default

## Immediate Next Steps

1. Convert the main renderer/runtime call graph to prefer `OffscreenRenderFrame`/GPU output over `QImage` wrappers.
2. Add tests or assertions that fail when strict Vulkan paths materialize through `QImage`.
3. Change legacy title/transcript helpers so `OverlayImage` is the default return type and `QImage` is a shim.
4. Audit preview/export callers that still rely on `QPainter` overlay composition and move Vulkan-capable paths off that helper.
5. Split preview runtime orchestration from QWidget creation and ownership.

## Summary

The original direction was mostly right, but the current tree is further along than the old draft suggested.

Qt image and boundary types are still the main blockers, but overlay generation is already partially de-Qt'd, renderer output is already partially GPU-first, and zero-copy instrumentation already exists.

The correct plan now is:

1. finish the renderer boundary transition
2. eliminate silent CPU fallback
3. finish overlay boundary cleanup
4. extract preview/runtime services
5. reduce QtCore boundary dependence
6. replace the editor shell last
