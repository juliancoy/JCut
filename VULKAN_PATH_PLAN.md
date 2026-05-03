# VULKAN_PATH_PLAN (Goal: Native Vulkan + OpenGL Fallback)

## Primary Goal
Deliver a **native Vulkan preview path** with a **deterministic OpenGL fallback** policy.

Final meaning:
- `requested=vulkan` + `effective=vulkan` => true native Vulkan presentation/composition path.
- If Vulkan init/runtime fails => automatic OpenGL fallback with one clear reason.
- Bridge modes (`vulkan-cpu-present`) are temporary and must be removed before completion.

## Snapshot (as of 2026-05-01)

## Completed
- Backend preference plumbing exists and is persisted (`opengl`, `vulkan`, etc.).
- `PreviewSurface` abstraction added and integrated through core ownership paths.
- Construction-time backend selection is in place via preview surface factory.
- Structured backend decision logs implemented:
  - `[render-backend-selection] ...`
  - `[render-backend-fallback] ...`
- `vulkan_preview_surface.*` scaffold added.
- `vulkan_context.*` added for dedicated Vulkan instance ownership.
- `vulkan_renderer.*` added for native `QVulkanWindowRenderer` state-driven loop scaffold.
- Vulkan parity bridge mode added in `VulkanPreviewSurface`:
  - native Vulkan surface can stay initialized while full timeline/overlay/interaction rendering is driven by the proven preview compositor path
  - enabled by default when native Vulkan is active (`JCUT_VULKAN_PARITY_BRIDGE != 0`)
- Native Vulkan surface attempt (`QVulkanWindow`) wired behind opt-in:
  - `JCUT_VULKAN_NATIVE_SURFACE=1`
- Runtime reporting now reflects actual outcome:
  - native init success => `effective=vulkan`
  - otherwise => `effective=vulkan-cpu-present` with fallback reason.
- Existing bridge fallback path remains stable and benchmark harness still works.

## Not Completed (Critical)
- Native Vulkan path does **not** yet render full preview composition parity (timeline frames/overlays/interactions) independently.
- Current native path is scaffold-level surface init, not full compositor.
- Independent native compositor parity (without bridge) is not complete yet.
- Native renderer is still clear-frame scaffold (no timeline frame composition yet).
- Fallback logging consolidated to factory-level decision logging (single fallback line).

---

## Current Architecture State
- `PreviewSurface` is the shared interface used by editor orchestration.
- `VulkanPreviewSurface` currently does:
  - try native `QVulkanWindow` init (optional via env)
  - run parity bridge path for full feature parity in Vulkan backend mode
  - optionally run native state path via `VulkanRendererState` + `VulkanNativeWindow` when bridge is disabled
  - otherwise fallback to `VulkanPreviewWindow` bridge delegate
- OpenGL/legacy rendering logic still lives in `PreviewWindow` (`QOpenGLWidget` monolith).

---

## Remaining Work (Authoritative Checklist)

## Phase 1: Native Vulkan Surface Ownership (Finish Foundation)
- [x] Add `vulkan_preview_surface.*` scaffold.
- [x] Add native Vulkan surface init attempt (`QVulkanWindow`).
- [x] Remove reliance on hidden legacy delegate for core state/render path.
- [x] Ensure factory emits exactly one backend decision/fallback log line per startup.

### Exit Criteria
- Native path has its own execution path for state-to-render flow.
- No duplicate backend fallback logs.

## Phase 2: Vulkan Compositor Path (Core Rendering)
- [ ] Implement Vulkan render loop for preview frames (swapchain-driven).
- [ ] Port frame upload/composition from OpenGL path to Vulkan resources/pipelines.
- [ ] Connect timeline frame data path directly into Vulkan compositor.
- [ ] Validate resize/recreate/device-lost handling.

### Exit Criteria
- Native Vulkan mode renders visual clip frames without OpenGL rendering dependency.

## Phase 3: Overlay + Interaction Parity
- [x] Port transcript/title/speaker/corrections overlays to Vulkan backend path (parity bridge mode).
- [x] Port hit-testing/drag/resize/edit interaction behavior for overlays (parity bridge mode).
- [x] Maintain correction draw mode + transcript/title interaction modes (parity bridge mode).

### Exit Criteria
- Feature parity with OpenGL preview interactions for agreed workflows.

## Phase 4: Deterministic OpenGL Fallback Finalization
- [ ] Harden fallback for native Vulkan init failure, swapchain failure, and device loss.
- [ ] Ensure fallback keeps UI responsive and preserves editor state.
- [ ] Ensure fallback reason is user/support actionable.

### Exit Criteria
- Vulkan failures never crash or dead-end preview; OpenGL fallback always usable.

## Phase 5: Bridge Retirement
- [ ] Remove `vulkan-cpu-present` bridge mode from normal path.
- [ ] Remove parity bridge mode (`JCUT_VULKAN_PARITY_BRIDGE`) after native compositor reaches parity.
- [ ] Keep only final selection/fallback logging paths.

### Exit Criteria
- `effective=vulkan` means native compositor path only.
- No bridge mode required for production operation.

## Phase 6: Validation + Performance Gates
- [ ] Run parity harness on representative projects and archive artifacts.
- [ ] Run throughput comparisons and document variance windows.
- [ ] Run stress tests (resize storms, scrub bursts, long playback, reopen loops).

### Exit Criteria
- Parity and throughput evidence archived.
- No critical regressions vs OpenGL baseline.

---

## Files Already Added/Modified for This Effort
- `preview_surface.h`
- `preview_surface_factory.h/.cpp`
- `preview_widget_factory.h/.cpp` (compat path still present)
- `vulkan_preview_surface.h/.cpp`
- `vulkan_preview_window.h/.cpp`
- `vulkan_context.h/.cpp`
- `vulkan_renderer.h/.cpp`
- `preview_window_core.cpp` (bridge logging guard)
- `scripts/vulkan_parity_throughput.sh`

## Planned Additional Vulkan-Focused Files (Still To Implement)
- `vulkan_resources.h/.cpp`
- `vulkan_pipeline.h/.cpp`
- `vulkan_overlay.h/.cpp` (or merged with renderer)

---

## Known Issues / Risks To Address Tomorrow
1. Native Vulkan mode currently initializes surface but is not full compositor parity.
2. Native renderer currently presents scaffold frames only; timeline composition is not wired.
3. Swapchain/device-loss fallback hardening is not implemented yet.
4. Ensure no hidden OpenGL dependency remains once compositor migration completes.

---

## Exact Next Steps (Resume Order)
1. Route clip frame upload/composition into Vulkan path; validate one-clip render correctness.
2. Add `vulkan_resources.*` + `vulkan_pipeline.*` for swapchain images, descriptors, and draw pipeline.
3. Implement resize/recreate + device-lost fallback handling.
4. Port overlays incrementally and validate each against screenshot parity.
5. Remove bridge mode after parity and fallback gates pass.
