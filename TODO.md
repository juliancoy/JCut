# TODO

## Vulkan Preview Black Output

Current state:
- Direct Vulkan preview initializes and presents.
- `/profile` reports valid Vulkan swapchain and repeated texture draws.
- Decode is ready in non-proxy mode and CUDA handoff reports success.
- The visible preview remains black, so the failure is after decode readiness and before/inside visible sampled output.

Observed profile evidence:
- `qvulkanwindow_valid: true`
- `handoff_successes > 0`
- `texture_draw_count > 0`
- `checker_draw_count: 0` in hardware-direct mode
- `clear_fallback_draw_count: 0`
- `last_handoff_mode: hardware_direct`
- `last_external_image_size: 1920x1080`

Likely causes to resolve, in order:

1. Verify the graphics pipeline independently of CUDA/proxy decode.
   - Run with `JCUT_VULKAN_PREVIEW_FORCE_CHECKER=1 ./build/jcut`.
   - Make sure the actual JCut window is visible, then screenshot or visually confirm checker output.
   - If checker is black, fix `VulkanPipeline`, descriptor layout, render pass, viewport/scissor, or swapchain presentation.
   - If checker is visible, continue to handoff/conversion debugging.

2. Fix `Use Proxy` state propagation.
   - `renderUseProxies=true` in the external project state did not show as `render_use_proxy_media: true` in `/profile` during testing.
   - Confirm whether project load is overwriting the checkbox/state or whether the test hit a stale/other instance.
   - Add profile fields for `renderUseProxies` from editor state and the preview-side `m_useProxyMedia` separately.
   - Add profile field for resolved decode path per active clip so we can see whether it is using `.proxy` image sequence or original media.

3. Validate proxy upload path separately from hardware-direct.
   - With `Use Proxy` active and resolved decode path pointing to the `.proxy` directory, `/profile` should show:
     - `render_use_proxy_media: true`
     - `cpu_decode_status_clips: 1`
     - `last_handoff_mode: cpu_upload`
   - If it still shows `hardware_direct`, proxy resolution or preview registration is wrong.
   - If it shows `cpu_upload` but remains black, the common graphics pipeline is wrong.

4. Validate CUDA hardware-direct image contents.
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
