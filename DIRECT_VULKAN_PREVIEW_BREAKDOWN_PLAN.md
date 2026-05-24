# Direct Vulkan Preview Breakdown Plan

## Goal

Reduce size and duplication in the current Vulkan preview stack without changing user-visible behavior.

The primary target is `direct_vulkan_preview_presenter.cpp`, which is currently the strongest breakdown candidate among the largest translation units because it combines multiple classes, owns several distinct responsibilities, and duplicates logic that already exists elsewhere in the repo.

## Scope

In scope:

1. `direct_vulkan_preview_presenter.cpp`
2. `direct_vulkan_preview_presenter.h`
3. `direct_vulkan_preview_audio.cpp`
4. `opengl_preview_window_overlay.cpp`
5. `vulkan_preview_surface.cpp`
6. Build file updates required to register new source files

Out of scope for the first pass:

1. Major redesign of `PreviewInteractionState`
2. OpenGL removal
3. `vulkan_facedetections_offscreen_main.cpp` decomposition
4. `speakers_tab.cpp` decomposition beyond documenting follow-up opportunities

## Why This File First

`direct_vulkan_preview_presenter.cpp` is the best first split because it has both structural and behavioral duplication.

Structural issues:

1. It contains the direct renderer implementation.
2. It contains the host widget implementation.
3. It contains the Vulkan window implementation.
4. It contains the presenter assembly/orchestration layer.

Behavioral duplication already confirmed:

1. Speaker hover profile parsing, caching, image lookup, and fallback avatar generation are duplicated across:
   - `direct_vulkan_preview_presenter.cpp`
   - `direct_vulkan_preview_audio.cpp`
   - `opengl_preview_window_overlay.cpp`
2. Audio preview pan-to-playhead behavior has drifted between:
   - `vulkan_preview_surface.cpp`
   - `opengl_preview_window_core.cpp`

This makes the presenter split high-yield: it reduces file size and removes established-path duplication at the same time.

## Current Findings

### Confirmed Redundancy

1. Speaker hover profile helpers are effectively copy-pasted between the Vulkan presenter, Vulkan audio helper path, and OpenGL overlay path.
2. Vulkan preview surface still carries an inline `syncAudioPreviewPanToPlayhead` path instead of using the shared `resolveAudioPreviewViewport(...)` helper already used by the OpenGL preview path.

### Large But Not First-Priority

1. `vulkan_facedetections_offscreen_main.cpp` is large, but most of the size appears to be unique harness/runtime/checkpoint/preflight logic rather than duplication of an established shared path.
2. `offscreen_vulkan_renderer.cpp` has some reusable helpers, but most of its weight still belongs to the offscreen export path.
3. `speakers_tab.cpp` is still too large, but it is already partially split into adjacent files and is currently dirty in the worktree, which makes it a worse first target.

## Target End State

The direct Vulkan preview stack should be organized around clear file boundaries:

1. Shared speaker-hover/profile utilities live in one reusable module.
2. Renderer-specific Vulkan draw/readback code lives separately from widget/window orchestration.
3. Presenter assembly remains thin and owns lifecycle, composition, and fallback/error wiring.
4. Shared preview behavior uses existing helper paths instead of backend-specific copies when those helpers already exist.

## Step-by-Step Plan

## Step 0: Preflight

Source files:

1. `CMakeLists.txt`
2. `direct_vulkan_preview_presenter.cpp`
3. `direct_vulkan_preview_presenter.h`

Destination files:

1. `CMakeLists.txt`

Actions:

1. Add placeholders for the destination files below before moving logic.
2. Build after each extraction step.
3. Preserve existing behavior and logging during the split; do not combine cleanup with behavior changes unless explicitly called out below.

## Step 1: Extract Shared Speaker Hover/Profile Utilities

Source files:

1. `direct_vulkan_preview_presenter.cpp`
2. `direct_vulkan_preview_audio.cpp`
3. `opengl_preview_window_overlay.cpp`

Destination files:

1. `preview_speaker_profiles.h`
2. `preview_speaker_profiles.cpp`

Actions:

1. Move shared `HoverSpeakerProfile` data structures into the new module.
2. Move transcript-derived speaker summary extraction into the new module.
3. Move speaker profile cache population and lookup into the new module.
4. Move hover image loading/scaling cache into the new module.
5. Move fallback avatar generation into the new module.
6. Update all three call sites to consume the shared helpers.

Acceptance criteria:

1. No duplicated speaker-hover/profile helper implementations remain in those three files.
2. Hover cards and speaker avatars still render identically in Vulkan and OpenGL preview paths.

## Step 2: Split Presenter Assembly From Renderer/Window Types

Source files:

1. `direct_vulkan_preview_presenter.cpp`
2. `direct_vulkan_preview_presenter.h`

Destination files:

1. `direct_vulkan_preview_renderer.cpp`
2. `direct_vulkan_preview_host_widget.cpp`
3. `direct_vulkan_preview_window.cpp`
4. `direct_vulkan_preview_presenter.cpp`

Actions:

1. Move `DirectVulkanPreviewRenderer` into `direct_vulkan_preview_renderer.cpp`.
2. Move `DirectVulkanPreviewHostWidget` into `direct_vulkan_preview_host_widget.cpp`.
3. Move `DirectVulkanPreviewWindow` into `direct_vulkan_preview_window.cpp`.
4. Keep `DirectVulkanPreviewPresenter` construction, wiring, lifecycle, and public API in `direct_vulkan_preview_presenter.cpp`.
5. Introduce private declarations or internal headers only as needed; do not expose renderer-only implementation details through broad public headers.

Acceptance criteria:

1. `direct_vulkan_preview_presenter.cpp` becomes orchestration-focused instead of owning all preview implementation details.
2. Class boundaries in the file layout match class boundaries in the code.

## Step 3: Extract Renderer-Local Readback Helpers

Source files:

1. `direct_vulkan_preview_renderer.cpp`

Destination files:

1. `direct_vulkan_preview_readback.cpp`
2. `direct_vulkan_preview_readback.h`

Actions:

1. Move readback slot allocation and teardown helpers out of the main renderer file.
2. Move image materialization helpers out of the main renderer file.
3. Keep command recording and frame orchestration in the renderer file.

Acceptance criteria:

1. Renderer file size drops materially.
2. Readback-specific code is isolated enough that it can later be audited for explicit preview/debug-only CPU fallback behavior.

## Step 4: Normalize Shared Audio Preview Pan Behavior

Source files:

1. `vulkan_preview_surface.cpp`
2. `audio_preview_support.cpp`
3. `audio_preview_support.h`

Destination files:

1. `audio_preview_support.cpp`
2. `audio_preview_support.h`

Actions:

1. Replace the Vulkan-specific inline pan-to-playhead math with the shared `resolveAudioPreviewViewport(...)` path already used by OpenGL.
2. Keep any truly backend-specific state updates at the call site only.

Acceptance criteria:

1. No duplicated `syncAudioPreviewPanToPlayhead` algorithm remains between Vulkan and OpenGL preview paths.
2. Audio view pan behavior is consistent across backends.

## Step 5: Reassess Remaining Large Files After The Presenter Split

Source files:

1. `speakers_tab.cpp`
2. `offscreen_vulkan_renderer.cpp`
3. `vulkan_facedetections_offscreen_main.cpp`

Destination files:

1. Follow-up plan doc updates only in this step

Actions:

1. Recount line totals after the presenter split and helper extraction work lands.
2. Re-evaluate whether `speakers_tab.cpp` or `offscreen_vulkan_renderer.cpp` is the next highest-yield target.
3. Leave `vulkan_facedetections_offscreen_main.cpp` for a later pass unless duplication findings change.

Acceptance criteria:

1. The next split decision is made from the post-extraction baseline rather than the current one.

## File Breakdown Recommendation

Recommended first-pass destination files:

1. `preview_speaker_profiles.h`
2. `preview_speaker_profiles.cpp`
3. `direct_vulkan_preview_renderer.cpp`
4. `direct_vulkan_preview_host_widget.cpp`
5. `direct_vulkan_preview_window.cpp`
6. `direct_vulkan_preview_readback.h`
7. `direct_vulkan_preview_readback.cpp`

## Risks

1. The presenter file likely contains internal helper coupling that will tempt broad header exposure. Resist that; prefer narrow internal declarations.
2. Hover-profile extraction touches both Vulkan and OpenGL preview paths, so behavior drift must be checked in both.
3. Readback helper extraction can accidentally blur the line between normal direct presentation and explicit CPU fallback paths if responsibilities are not kept tight.
4. `speakers_tab.cpp` is already being edited locally, so it should not be part of the first implementation slice.

## Testing Expectations

Minimum validation after each step:

1. Build the editor target cleanly.
2. Open a project with preview enabled.
3. Verify direct Vulkan preview initializes without new failure reasons.
4. Verify transcript overlays still render in preview.
5. Verify speaker hover cards still show avatar, name, organization, and summary.
6. Verify audio preview pan-to-playhead behavior remains stable.
7. Verify OpenGL fallback preview still compiles and shows the same speaker hover content.

## Non-Goals

1. Replacing the direct Vulkan presenter architecture
2. Folding offscreen render/export code into the interactive presenter
3. Doing a large shared-preview rewrite in the same patch series
4. Solving every oversized file in one pass

## Recommendation

Implement the work in this order:

1. Shared speaker-hover/profile extraction
2. Presenter class-per-file split
3. Renderer readback helper extraction
4. Vulkan/OpenGL audio-pan behavior unification

That order removes real duplication first, then simplifies the largest preview translation unit, then cleans the most localized renderer internals, and only after that reopens the question of which remaining large file should be broken down next.
