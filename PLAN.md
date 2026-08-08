Biggest errant couplings

Editor window mega-header
[editor.h (line 1)](/home/julian/Documents/JCut/editor.h:1) is still the broadest app-level umbrella: 915 lines, 737 symbols, 19 include consumers, and 31 repository headers in fan-out. Internal implementation fragments should move to narrower `EditorWindow` state/command interfaces instead of depending on the full application window.

Preview snapshot split landed; the next cut is smaller render overrides
[preview_interaction_state.h (line 1)](/home/julian/Documents/JCut/preview_interaction_state.h:1) is now 189 lines and 151 symbols, with 13 include consumers and 8 fan-out edges. Renderer-facing fields now live in `PreviewRenderSnapshot`, and drag-only state moved into `PreviewInteractionUiState`. The next bounded step is to move the remaining interaction/transient overrides out of the renderer snapshot and keep preview render helpers on that narrower contract.

Global debug configuration
[debug_controls.h (line 1)](/home/julian/Documents/JCut/debug_controls.h:1) has 42 include consumers across 10 owners, and its implementation is 975 lines. It is still acting as a global configuration bus spanning UI, audio, decode, render, control-server, and face detection. Split it into typed domain snapshots and keep UI/control-server code as adapters.

Render internals are still too monolithic
[direct_vulkan_preview_window_internal.h (line 1)](/home/julian/Documents/JCut/direct_vulkan_preview_window_internal.h:1) is 1,698 lines and 232 symbols with 37 fan-out edges. [direct_vulkan_preview_renderer_recording.cpp (line 1)](/home/julian/Documents/JCut/direct_vulkan_preview_renderer_recording.cpp:1) is 2,039 lines with 34 fan-out edges, [offscreen_vulkan_renderer_backend.cpp (line 1)](/home/julian/Documents/JCut/offscreen_vulkan_renderer_backend.cpp:1) is 1,582 lines / 209 symbols with 35 fan-out edges, and [vulkan_preview_surface.cpp (line 1)](/home/julian/Documents/JCut/vulkan_preview_surface.cpp:1) is 3,678 lines / 123 symbols with 53 fan-out edges. After the snapshot split, the next productive boundary is a narrow preview-window services interface so recording, resource ownership, and presentation bookkeeping stop reaching through the full internal header.

The earlier shared-header issue is now secondary
[editor_shared.h (line 1)](/home/julian/Documents/JCut/editor_shared.h:1) still has 53 include consumers, and [editor_shared_core.h (line 1)](/home/julian/Documents/JCut/editor_shared_core.h:1) still has 29. That is still broad, but it is no longer the strongest structural signal after the narrow type headers were split out.

Secondary cleanup candidates
The fresh graph now reports 48 dead or unreachable implementations and 2 duplicate-responsibility clusters: `null_preview_surface.cpp` / `vulkan_preview_surface.cpp`, and `timeline_layout.cpp` / `timeline_widget_layout.cpp`. Those are lower priority than the app/render umbrellas, but they are still worth pruning once the main boundaries are smaller.

The current viewer is available at [index.html](/home/julian/Documents/JCut/build/code-structure/index.html).
