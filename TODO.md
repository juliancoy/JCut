# TODO

## Vulkan Preview Black Output

Current state:
- Direct Vulkan preview initializes and presents.
- `/profile` reports valid Vulkan swapchain and repeated texture draws.
- Original-media hardware decode is the primary direct Vulkan path. It must work without proxies.
- CUDA/Vulkan handoff reports success when a hardware frame is selected.
- The black-frame failure case is now treated as visible-frame starvation unless diagnostics show handoff or render failure.

Observed profile evidence:
- `qvulkanwindow_valid: true`
- `handoff_successes > 0`
- `texture_draw_count > 0`
- `checker_draw_count: 0` in hardware-direct mode
- `clear_fallback_draw_count: 0`
- `last_handoff_mode: hardware_direct`
- `last_external_image_size: 1920x1080`

Likely causes to resolve, in order:

1. Verify visible decode scheduling before suspecting Vulkan rendering.
   - Low `exact_hit_rate`, high `missing_frame_rate`, high `current_frame_failure_rate`, or many visible null callbacks means the presenter has no usable current frame.
   - A 60 fps source inside a 30 fps timeline is a normal case. Source-frame requests must not be cancelled merely because the playhead advanced by timeline-frame cadence.
   - `decodeThroughFrame(...)` must not satisfy a visible request with a different older frame. An exact miss is a failure to diagnose, not a successful current-frame completion.

2. Verify the graphics pipeline independently of hardware decode only after the scheduler is delivering current frames.
   - Run with `JCUT_VULKAN_PREVIEW_FORCE_CHECKER=1 ./build/jcut`.
   - Make sure the actual JCut window is visible, then screenshot or visually confirm checker output.
   - If checker is black, fix `VulkanPipeline`, descriptor layout, render pass, viewport/scissor, or swapchain presentation.
   - If checker is visible, continue to handoff/conversion debugging.

3. Keep proxy mode honest, but do not use it as the fix for direct Vulkan.
   - If `Use Proxy` is disabled, direct Vulkan should decode original media through hardware/GPU payloads.
   - If `Use Proxy` is enabled, the preview must respect that state and resolve the proxy decode path.
   - Proxy mode is useful for workflow and comparison, not a required workaround for original-media Vulkan playback.

4. Validate CUDA hardware-direct image contents if scheduling is healthy and black output remains.
   - Add a temporary readback/checksum of the post-conversion Vulkan RGBA image, gated by an env var, not enabled by default.
   - Report luma/min/max in `/profile`.
   - If readback luma is near zero while source FFmpeg luma is nonzero, the CUDA NV12 copy/conversion path is producing black.

5. Replace ad-hoc CUDA/Vulkan synchronization if readback confirms black conversion.
   - Current path imports Vulkan external memory into CUDA and uses CPU-side `cuStreamSynchronize`, then Vulkan barriers.
   - If still stale/black, implement proper CUDA/Vulkan external semaphore synchronization.
   - Do not fall back to OpenGL.
   - Do not use QImage for original-media Vulkan mode.

6. Keep the memory fixes.
   - Keep one Vulkan preview decode worker.
   - Keep source lookahead small.
   - Keep GPU-aware cache trimming.
   - Keep hardware-frame intermediate retention fix in `DecoderContext`.

7. Clean up diagnostics after fixing.
   - Remove or hide `JCUT_VULKAN_PREVIEW_FORCE_CHECKER` if not needed long-term.
   - Keep useful `/profile` counters that expose handoff/draw state.
   - Remove any temporary readback path unless retained behind an explicit debug env var.
