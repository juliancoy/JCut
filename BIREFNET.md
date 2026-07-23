# BiRefNet continuous-alpha rotoscoping

Right-click a video clip in the timeline or Clips table and choose
**Rotoscope → Run BiRefNet…**. The neighboring **Run SAM 3…** action remains
available for prompt-based selection.

The BiRefNet preflight includes **Preview Frame**. It runs the pinned model on the
source frame under the playhead (or the clip midpoint when the playhead is outside
the clip), then displays the source, grayscale alpha, and a checkerboard composite.
This lets users reject an unsuitable automatic selection before committing to the
full clip. **Alpha tolerance** removes low-confidence foreground leakage while
preserving a continuous soft matte: `0%` is exact model output, while higher values
make the selection progressively stricter. Select **Generate Alpha** when the
preview and settings are acceptable.
Completion creates or reuses a locked **Mask Matte** child. This gives the
timeline an immediately usable masked foreground layer while retaining its source
parent below it; move effects or replacement backgrounds between those layers.
The alpha sidecar and all mask treatment belong to that child; the source parent
continues to own media, timing, and transforms. Z-level controls compositing
order; it is not a marker or a relationship type.

During full generation, the progress window refreshes a three-panel
**Source | Alpha Matte | Composite** strip after every rendered frame. The log
below it reports the current frame and total frame count. Users can stop the run
without applying an incomplete matte; launching it again resumes from completed
alpha frames. The Mask Matte child is materialized only after the full job
succeeds.

BiRefNet is most suitable when the intended subject is the visually dominant
foreground. Unlike SAM, it does not accept a text or point prompt. It writes one
8-bit grayscale alpha PNG per decoded source frame and JCut immediately uses that
directory as the child's mask sidecar. Intermediate gray values are preserved by
the Vulkan mask pipeline. The shared Masks controls support invert, erode, dilate,
blur/feather, continuous-alpha falloff, opacity, mask-only inspection, and soft
drop shadow in both Vulkan preview and export.

The default model is `ZhengPeng7/BiRefNet-matting` at a pinned Hugging Face
revision. CUDA FP16 is the recommended configuration. CPU and FP32 modes are
available for compatibility, but are substantially slower.

Artifacts are written beside the source video:

```text
<video-stem>_birefnet_alpha_masks/
  frame_000001.png
  frame_000002.png
  ...
  jcut_alpha.json
```

Runs are resumable. Existing non-empty alpha frames are skipped, while missing
frames are generated. Changing the model, device precision, or alpha tolerance
starts a clean run so a sidecar can never contain frames produced with mixed
settings. The dedicated Docker runtime and model caches are separate
from SAM, so installing or updating BiRefNet cannot change SAM's dependencies.

## Memory failures

CUDA allocation failures and host `MemoryError` failures are reported explicitly.
The runner writes `jcut_error.json` beside the incomplete alpha frames with the
failure category, processing phase, frame number, device, and available CUDA
memory diagnostics. JCut copies those fields into the job manifest and presents
an actionable error in the progress window. Completed frames remain intact and a
retry with unchanged settings resumes from the next missing frame.

Exit code `137` is reported as a possible memory-pressure kill rather than a
definite OOM because an external `SIGKILL` has several possible causes. JCut does
not silently fall back to a different device, precision, or model.

The standalone launcher is also available:

```bash
./birefnet.sh input.mp4 --output-dir ./input_birefnet_alpha_masks
```

BiRefNet processes frames independently. It improves edge detail and produces a
true continuous matte, but it does not provide temporal memory; difficult footage
may still benefit from a later temporal-stabilization pass.
