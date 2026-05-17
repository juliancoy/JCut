# Qt Deprecation Plan

## Goal

Reduce and eventually remove Qt from the performance-critical runtime and editor shell without destabilizing the rendering pipeline.

This is not a single-step port. The codebase currently uses Qt for:

- UI widgets and windowing
- event loop, timers, and signal/slot messaging
- IPC wrappers
- strings, containers, and JSON
- image/text rendering primitives
- frame and overlay interchange types

The high-risk area is not scheduling or messaging. The high-risk area is that render/compositor paths still use `QImage` and Qt text APIs as core data-path types.

## Principles

- Remove Qt from runtime boundaries before removing it from the app shell.
- Prioritize zero-copy render paths over UI framework replacement.
- Replace abstractions, not call sites, where possible.
- Keep fallback compatibility paths during migration.
- Do not mix “performance refactor” and “UI rewrite” into one step.

## Current Risk Areas

### 1. Renderer contracts are still Qt-shaped

Examples:

- `render_internal.h`: `QImage renderFrame(...)`
- `facestream_runtime.h`: `renderFrameWithVulkan(...)`, `readLastRenderedVulkanFrameImage(...)`
- `offscreen_vulkan_renderer.cpp`: layer composition still accepts CPU image layers and CPU text overlays

Impact:

- Qt image types leak into runtime interfaces
- zero-copy is optional instead of primary
- preview/export helpers can silently force readback

### 2. Overlay rendering is CPU/Qt based

Examples:

- transcript overlays via `QPainter` / `QTextDocument`
- title overlays via `QPainter`
- preview image annotation paths

Impact:

- Vulkan mode still has CPU islands
- UI toolkit and compositor remain coupled

### 3. Qt core types are pervasive

Examples:

- `QString`, `QVector`, `QHash`, `QJsonObject`
- Qt object ownership and lifecycle assumptions

Impact:

- raises migration cost across nearly every subsystem
- makes “just replace the UI” misleading

## Migration Stages

## Stage 0: Inventory And Guardrails

Goal:

Measure and constrain Qt usage before major refactors.

Tasks:

- Add a dependency inventory for major Qt categories:
  - UI/widgets
  - eventing/messaging
  - IPC
  - image/text/rendering
  - containers/core types
- Mark performance-critical paths where Qt usage is forbidden.
- Add runtime logging for readback/materialization in Vulkan mode.
- Define a strict meaning of “zero-copy” and enforce it in summaries/tests.

Exit criteria:

- Known list of Qt dependencies by subsystem
- Zero-copy contract documented and testable

## Stage 1: Separate Runtime Services From Qt UI

Goal:

Make scheduling and messaging replaceable.

Tasks:

- Introduce interfaces for:
  - task scheduler
  - timers
  - main-thread dispatch
  - message bus / observer events
  - IPC transport
- Wrap existing Qt implementations behind these interfaces.
- Remove direct widget/signal ownership assumptions from runtime code.

Notes:

- This stage is moderate effort.
- None of this is conceptually hard, but it is spread across many call sites.

Exit criteria:

- Runtime code can operate against non-Qt scheduler/messaging interfaces
- Qt remains only as one backend implementation

## Stage 2: Remove Qt Image Types From Renderer Boundaries

Goal:

Make GPU-native frame interchange primary.

Tasks:

- Replace `QImage`-first renderer contracts with GPU-native frame/layer contracts:
  - decoded frame handle
  - Vulkan external image
  - explicit CPU fallback image
- Keep CPU readback as an opt-in compatibility/output path.
- Ensure Vulkan compositor accepts:
  - hardware decoder frames
  - GPU textures
  - imported Vulkan images
- Eliminate implicit “materialize to `QImage`” steps in render paths.

Notes:

- This is the most important stage for performance.
- It is also the biggest architectural change in the rendering stack.

Exit criteria:

- Core compositor/render APIs do not require `QImage`
- Zero-copy video layers work independently of the editor UI shell

## Stage 3: Replace Qt Overlay/Text Rendering

Goal:

Remove CPU/Qt overlay islands from Vulkan mode.

Tasks:

- Move transcript rendering to a GPU-friendly text pipeline
- Move title rendering to GPU text/shape rendering
- Replace `QPainter` overlay composition with:
  - glyph atlas rendering, or
  - prebuilt texture generation behind non-Qt interfaces
- Isolate font loading, shaping, and layout behind renderer-agnostic services

Notes:

- This is harder than replacing timers or windows.
- Likely requires HarfBuzz + FreeType or another dedicated text stack.

Exit criteria:

- Vulkan preview/export paths can render titles/transcripts without `QImage`, `QPainter`, or `QTextDocument`

## Stage 4: De-Qt IPC And Core Utility Types

Goal:

Reduce pervasive dependence on Qt core.

Tasks:

- Replace `QLocalSocket`/`QLocalServer` usage with a transport abstraction
- Introduce neutral wrappers or alternate types for:
  - strings
  - vectors/maps
  - JSON
- Convert subsystem boundaries first, internals later

Notes:

- Full removal of `QString`/`QVector`/`QJsonObject` is a large codebase-wide effort.
- This should happen only after runtime and render boundaries are stable.

Exit criteria:

- Core runtime/render modules can compile with minimal or no QtCore reliance

## Stage 5: Replace The Editor Shell

Goal:

Make the UI framework swappable.

Tasks:

- Build a new shell using ImGui or another UI framework
- Reimplement:
  - panels
  - timeline interactions
  - preview controls
  - settings editors
  - diagnostics/profiling views
- Keep runtime/render backends unchanged beneath the shell

Notes:

- This is where ImGui becomes realistic.
- Doing this before Stages 1-3 would create a rewrite with weak payoff.

Exit criteria:

- Editor shell no longer depends on QtWidgets
- Runtime/render path unchanged across shell swap

## Recommended Order Of Work

1. Finish zero-copy compositor work for video layers
2. Introduce runtime service interfaces for scheduling/messaging
3. Remove `QImage` from renderer/runtime boundaries
4. Replace transcript/title overlay rendering
5. Abstract IPC and reduce QtCore usage
6. Replace the editor shell

## What Is Easy To Replace

- timers
- event dispatch
- observer/message bus patterns
- local IPC wrappers
- top-level windows and panels

## What Is Hard To Replace

- `QImage` as a frame interchange type
- `QPainter`-based overlay rendering
- `QTextDocument` text layout/rendering
- broad use of Qt core/container types in public interfaces

## ImGui Assessment

ImGui is viable as a future editor shell, but it should not be the first migration step.

If the goal is:

- performance
- zero-copy rendering
- lower render latency

Then replacing the UI shell first is the wrong priority.

If the goal is:

- simpler editor shell
- easier tooling UI iteration
- less dependence on QtWidgets

Then ImGui makes sense after renderer/runtime boundaries stop depending on Qt image/text primitives.

## Suggested Milestones

### Milestone A

Zero-copy Vulkan compositor for visual video layers, with explicit CPU fallback only.

### Milestone B

Renderer/runtime APIs no longer require `QImage`.

### Milestone C

Transcript/title rendering moved off Qt.

### Milestone D

Runtime scheduling/messaging/IPC can run with non-Qt backends.

### Milestone E

QtWidgets editor shell replaced.

## Non-Goals For The First Pass

- Removing every `QString` immediately
- Replacing every Qt container in one sweep
- Rewriting the full editor and renderer simultaneously
- Sacrificing working preview/export behavior for purity

## Summary

Qt scheduling and messaging infrastructure is replaceable.

Qt image and text primitives are the real blockers.

The correct plan is to de-Qt the runtime and rendering boundaries first, then replace the editor shell last.
