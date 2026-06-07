# Direct Vulkan Preview Modularization Plan

## Goal

Replace the former monolithic `direct_vulkan_preview_backend.cpp` implementation with smaller files that have clear ownership. The first pass is complete: the `.cpp` implementation file has been removed, and the existing public adapter declarations remain in `direct_vulkan_preview_backend.h`.

## Proposed Files

### `direct_vulkan_preview_config.h/.cpp`

Owns environment/config helpers and small string/format utilities.

- `envFlagEnabled`
- `envFlagDisabled`
- `vulkanPreviewDebugChromeEnabled`
- `vulkanPreviewOptimalPresentEnabled`
- `vulkanPreviewReadbackMirrorEnabled`
- `vulkanPreviewDirectSwapchainVisible`
- `vulkanPreviewVisiblePathLabel`
- `vulkanPreviewCanvasMarginPx`
- `pixelFormatName`
- `vulkanFormatName`

### `direct_vulkan_preview_overlay_rendering.h/.cpp`

Owns CPU-side overlay image generation and overlay texture keys.

- `setFontPixelSizeRobust`
- `playbackStatusOverlayTextureKey`
- `renderPlaybackStatusOverlay`
- Any future CPU-rasterized status/debug overlay helpers

### `direct_vulkan_preview_geometry.h/.cpp`

Owns preview geometry, clip mapping, colors, and Vulkan clear-rect helpers.

- `clearRectFromQRect`
- `scissorFromQRect`
- `selectionOutlineColor`
- `mvpForVulkanClipTransform`
- `clipColor`
- `clipColorForStatus`
- `facedetectionsOverlayColor`
- `normalizedBoxToSwapchainRect`
- `faceDetectionBoxToSwapchainRect`
- `clearRect`
- `clearBoxOutline`
- `targetBoxOverlayColor`
- `targetBoxRectForComposite`

### `direct_vulkan_preview_interaction.h/.cpp`

Owns mouse, hit-test, transform, and overlay interaction helpers that do not require `QVulkanWindow` methods.

- `applyVideoPreviewWheelZoom`
- `applyAudioPreviewWheelZoom`
- `audioSeekSampleAtSurfacePosition`
- `clipSupportsTranscriptOverlay`
- `clipForId`
- `clipWithTransientTranscriptOverride`
- `transformWithTransientOverride`
- `clearVulkanDragOverrides`
- `transcriptOverlayBoundsForClip`
- `VulkanInteractionOverlayInfo`
- `VulkanInteractionOverlayInfos`
- `collectVulkanInteractionInfos`
- `clipIdAtPositionForVulkan`
- `mapScreenPointToNormalizedClipForVulkan`
- `mapNormalizedClipPointToScreenForVulkan`
- `faceDetectionScreenPathForVulkan`
- `dispatchFaceDetectionsBoxAtPosition`
- `dispatchFaceDetectionsFocusClearAtPosition`
- `updateHoveredFaceDetectionsBox`
- `clipIdIsTitleForVulkan`
- `currentTransformForVulkanClip`

### `direct_vulkan_preview_host_widget.h/.cpp`

Owns the Qt widget wrapper around the native Vulkan window.

- `DirectVulkanPreviewHostWidget`
- `createDirectVulkanPreviewHostWidget`
- `createDirectVulkanPreviewWindowContainer`

### `direct_vulkan_preview_window.h/.cpp`

Owns `QVulkanWindow` subclass behavior and event routing.

- `DirectVulkanPreviewWindow`
- `createDirectVulkanPreviewWindow`
- Vulkan instance/device extension forwarding
- Window resize/raise/hide/title/visibility/cursor wrappers
- `directVulkanPreviewWindowSetInteractionCallbacks`
- Mouse, wheel, hover, context menu, key, expose event handling

### `direct_vulkan_preview_renderer.h/.cpp`

Owns the `QVulkanWindowRenderer` subclass lifecycle and frame recording.

- `DirectVulkanPreviewRenderer`
- `initResources`
- `releaseResources`
- `startNextFrame`
- device loss handlers
- frame status/render snapshot latching
- clip draw loop
- transcript/speaker/status/debug overlay draw calls

### `direct_vulkan_preview_handoff_resources.h/.cpp`

Owns sampled-image handoff resource lifetime for clip textures.

- `ClipHandoffResources`
- `RetiredClipHandoffResources`
- `ensureClipHandoffResources`
- `pruneClipHandoffResources`
- `advanceRetiredClipHandoffResources`
- `releaseClipHandoffResources`
- `updateClipHandoffResourceStats`

This may be either a helper owned by `DirectVulkanPreviewRenderer` or a small class, for example `DirectVulkanPreviewHandoffResourcePool`.

### `direct_vulkan_preview_readback.h/.cpp`

Owns readback slot allocation and image extraction.

- `ReadbackSlot`
- `findMemoryType`
- `destroyReadbackSlots`
- `ensureReadbackSlot`
- `imageFromReadback`
- `consumeReadbackSlot`
- `consumeDecoderReadbackSlot`
- `recordSwapchainReadback`
- `recordImageReadback`

This can become a small class, for example `DirectVulkanPreviewReadbackManager`, to avoid exposing renderer internals.

### `direct_vulkan_preview_backend.h`

Public adapter declarations for presenter/window wiring. Keep this header as the boundary consumed by `direct_vulkan_preview_presenter.cpp`; do not add implementation logic here.

## Extraction Order

1. Move pure helpers first: config, overlay rendering, geometry.
2. Move interaction helpers next, keeping signatures unchanged.
3. Split `DirectVulkanPreviewHostWidget` out.
4. Split `DirectVulkanPreviewWindow` out after interaction helpers are stable.
5. Extract readback and handoff resource managers from the renderer.
6. Move `DirectVulkanPreviewRenderer` last, once its dependencies have narrow headers.
7. Remove `direct_vulkan_preview_backend.cpp` once adapter implementations have moved to focused modules.

## Dependency Rules

- `direct_vulkan_preview_config` should depend only on Qt/Core and Vulkan format types where needed.
- `direct_vulkan_preview_geometry` may depend on `preview_interaction_state.h`, `preview_view_transform.h`, `vulkan_pipeline.h`, and Vulkan headers.
- `direct_vulkan_preview_interaction` may depend on `preview_interaction_state.h`, `preview_view_transform.h`, transcript overlay helpers, and `audio_preview_support.h`.
- `direct_vulkan_preview_window` may depend on interaction helpers but should not depend on renderer internals.
- `direct_vulkan_preview_renderer` may depend on geometry, overlay rendering, readback, handoff resources, `VulkanResources`, `VulkanPipeline`, and `DirectVulkanPreviewWindow`.
- Avoid circular dependencies by passing callbacks/state structs rather than including presenter headers in low-level modules.

## Build System Updates

The project uses a top-level `file(GLOB *.cpp)`, so new module `.cpp` files are picked up automatically after CMake reconfigure. If the build system switches away from globbing, add these module files explicitly to `editor_core`.

Suggested first batch:

- `direct_vulkan_preview_config.cpp`
- `direct_vulkan_preview_overlay_rendering.cpp`
- `direct_vulkan_preview_geometry.cpp`
- `direct_vulkan_preview_interaction.cpp`

Suggested second batch:

- `direct_vulkan_preview_host_widget.cpp`
- `direct_vulkan_preview_window.cpp`
- `direct_vulkan_preview_handoff_resources.cpp`
- `direct_vulkan_preview_readback.cpp`
- `direct_vulkan_preview_renderer.cpp`

## Safety Checks

After each extraction batch:

- Build with `cmake --build build -j2`.
- Start JCut and verify the Direct Vulkan preview opens.
- Check `/pipeline?debug=1` for `backend: "vulkan"`, `qvulkanwindow_valid: true`, and expected draw counters.
- Verify mouse selection, resize handles, preview zoom/pan, face-stream clicks, right-click face-stream focus clear, audio preview seek, and transcript overlay interaction.
- Confirm no behavior changes in screenshot/readback diagnostics.

## Target End State

Approximate file sizes after modularization:

- `direct_vulkan_preview_backend.cpp`: removed
- `direct_vulkan_preview_window.cpp`: 600-900 lines
- `direct_vulkan_preview_renderer.cpp`: 900-1300 lines
- `direct_vulkan_preview_interaction.cpp`: 500-800 lines
- `direct_vulkan_preview_geometry.cpp`: 250-450 lines
- `direct_vulkan_preview_readback.cpp`: 300-500 lines
- `direct_vulkan_preview_handoff_resources.cpp`: 200-400 lines
- config/overlay/host files: each under 300 lines

The main success criterion is that new preview work has an obvious home and no future feature recreates `direct_vulkan_preview_backend.cpp`.

## Implemented Split

The first modularization pass has been applied:

- `direct_vulkan_preview_backend.cpp`: removed. Public adapter declarations remain in `direct_vulkan_preview_backend.h`; implementations now live in focused modules.
- `direct_vulkan_preview_config.h/.cpp`: environment flags and format naming.
- `direct_vulkan_preview_overlay_rendering.h/.cpp`: playback status overlay keying/rasterization.
- `direct_vulkan_preview_interaction.h/.cpp`: zoom, seek mapping, clip hit testing, face-stream click/hover paths, transcript bounds, and current transform helpers.
- `direct_vulkan_preview_geometry.h/.cpp`: Vulkan clear/scissor helpers, colors, MVP mapping, face/target overlay rectangles, and grading LUT upload bytes.
- `direct_vulkan_preview_host_widget.cpp`: fallback host widget and host-widget factory.
- `direct_vulkan_preview_window.cpp`: Direct Vulkan window, renderer lifecycle, readback, handoff resources, frame recording, and public window adapter functions.
- `vulkan_clear_helpers.h/.cpp`: shared Vulkan clear-rect/outline helpers used by Direct Vulkan preview and the Vulkan text renderer, eliminating duplicate implementations.

`direct_vulkan_preview_window.cpp` intentionally remains the integration file for the tightly coupled `QVulkanWindow` and `QVulkanWindowRenderer` classes after this pass. A later pass can split it into `direct_vulkan_preview_renderer.cpp`, `direct_vulkan_preview_readback.cpp`, and `direct_vulkan_preview_handoff_resources.cpp` once a narrow window-owner interface is introduced.
