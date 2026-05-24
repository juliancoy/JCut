# JCut NCNN Fork

JCut uses a maintained NCNN fork for runtime Vulkan interop work that upstream NCNN does not currently expose as public API.

## Repository

- Upstream: `https://github.com/Tencent/ncnn.git`
- JCut fork: `https://github.com/juliancoy/ncnn.git`
- JCut branch: `jcut/external-vulkan-device`

The parent repository tracks `external/ncnn` as a gitlink and declares it in `.gitmodules` with the fork URL and branch above.

## Required Runtime Patch

Commit: `a84c787 Add JCut external Vulkan device wrapper`

Purpose:

1. Allow NCNN to wrap a JCut-owned `VkDevice` and `VkQueue`.
2. Keep ownership with JCut so NCNN does not destroy the external Vulkan device.
3. Route NCNN compute/transfer queue-family lookups through the wrapped JCut queue family.
4. Preserve the zero-copy SCRFD path used by `VulkanScrfdNcnnFaceDetector::initialize(const VulkanDeviceContext&, ...)`.

Without this patch, JCut cannot call the external-device NCNN constructor used by the SCRFD Vulkan detector.

## Optional Tooling Patch

Commit: `ec7024c Support Caffe raw_data blobs in converter`

Purpose:

1. Adds FP16/raw blob handling to NCNN's Caffe converter.
2. This is not required for JCut runtime FaceDetections generation.
3. Keep it as a separate commit so it can be dropped if the fork should only carry runtime patches.

## Updating From Upstream

From `external/ncnn`:

```bash
git fetch upstream
git switch jcut/external-vulkan-device
git rebase upstream/master
git push --force-with-lease origin jcut/external-vulkan-device
```

Then from the JCut repository root:

```bash
git add external/ncnn .gitmodules docs/NCNN_FORK.md
git commit -m "Update NCNN fork pointer"
```

## Rules

1. Keep JCut-specific NCNN changes as small, reviewable commits on the fork branch.
2. Do not make ad hoc edits directly in `external/ncnn` without committing them to the fork.
3. Keep runtime-required patches separate from converter/tooling patches.
4. Prefer upstreaming the external Vulkan device wrapper if NCNN maintainers are open to it.
