# Masks, SAM Sidecars, and Virtual Mask Clips

This document describes the intended JCut mask workflow from SAM generation to
timeline compositing.

## Summary

SAM masks are sidecar artifacts attached to a source media clip. A mask sidecar
does not replace the source video and does not create a second independently
timed media item. When a mask needs to participate in timeline stacking, JCut
represents it as a generated `mask_matte` virtual clip that stays locked to the
source clip.

The professional model is:

1. The source clip owns the decoded video, timing, transform, and mask controls.
2. The SAM sidecar supplies per-frame matte images through `maskFramesDir`.
3. The virtual mask clip exposes the masked foreground in the timeline stack.
4. Independent mask grading applies only to pixels where the mask is present.

## Creating SAM Mask Sidecars

SAM processing analyzes a source image or video and writes mask artifacts beside
the project/media workflow instead of baking them into the original clip.

For video, the sidecar is frame based:

- Each source video frame has a corresponding mask frame.
- The mask frame is sampled in source-video time.
- The mask is aligned to the source frame size or preprocessed into the size
  expected by the renderer.
- The sidecar path is stored on the source clip as `maskFramesDir`.

The source clip becomes mask-capable when:

- `maskEnabled = true`
- `maskFramesDir` points to a valid mask frame directory
- the mask frames match the source clip's frame order and timing

The important invariant is that the mask sidecar follows the source clip's media
time. If the source clip is trimmed, moved, or played at a different rate, the
mask frame lookup must use the same source-time mapping as the video frame
lookup.

## Source Clip Ownership

The source clip is the single source of truth for the mask:

- `maskFramesDir`
- feather, blur, erode, dilate, invert, and opacity
- mask-only diagnostic state
- mask grading controls
- mask grading curves
- transform and transform keyframes
- source-in, duration, playback rate, and timeline position

Selecting either the source clip or its virtual mask clip should open the same
Mask tab and Transform tab state. Edits are written to the source clip, then
propagated to the virtual mask clip by normalization.

This prevents the common bad state where the source and virtual mask clip appear
to have separate mask settings, or where the virtual clip is accidentally left in
`Show mask only` diagnostic mode.

## Virtual Mask Clips

A virtual mask clip is a generated timeline clip with:

- `clipRole = mask_matte`
- `linkedSourceClipId` pointing to the source clip
- `generatedFromMaskId` recording the mask sidecar identity/path
- `syncLockedToSource = true`
- `sourceTransformLocked = true`
- no audio

The virtual clip renders the source image clipped by the SAM mask. It exists so
the masked foreground can be placed above other video/image/effect layers in Z
order.

The virtual clip is not independent media:

- It must not have independent retiming relative to the source video.
- It must not have an independent transform while source-transform locking is
  enabled.
- It must not drift away from the source clip's trim, source-in, playback rate,
  or duration.
- It should remain locked to reduce accidental edits.

If the source clip moves or changes duration, the virtual mask clip should move
and change with it.

## Timeline Stack

The intended stack for a foreground cutout is:

1. Lower track: the original source clip or another background plate.
2. Middle track: inserted background video, image, or generated effect.
3. Upper track: the virtual `mask_matte` clip.

The upper virtual mask clip draws only the masked foreground portion of the
source media. Transparent pixels outside the mask reveal the lower tracks.

The source clip may also keep `maskForegroundLayerEnabled = true` so preview and
export can build the synchronized foreground layer from the source clip. The
virtual clip is the timeline representation of that same foreground layer.

## Mask Preview Modes

There are three different views that must not be confused:

- Normal composite: source video is drawn normally, and the masked foreground can
  be drawn as a foreground layer or virtual clip.
- Mask-only diagnostic: the mask itself is shown as grayscale alpha for
  inspection.
- Mask grading: source video pixels inside the mask are color adjusted, while
  pixels outside the mask remain transparent or unchanged depending on the draw
  pass.

`Show mask only` is a diagnostic view. It should not be used for the generated
foreground clip in normal editing, because it shows the matte image rather than
the actual video pixels.

## Independent Mask Grading

Mask grading is independent from the clip's normal full-frame grade, but it is
not independent from the source clip's mask ownership.

The mask grade controls are:

- enable/disable mask grade
- brightness
- contrast
- saturation
- RGB and luma curves

These controls apply only where the SAM mask is present. The expected result is
not a white silhouette. The expected result is the original video foreground,
cut out by the mask, with the mask grade applied to that foreground.

Correct rendering behavior:

- Inside the mask: draw source video pixels with mask grade applied.
- Feathered edge: blend according to grayscale mask alpha.
- Outside the mask: contribute transparent pixels for the virtual foreground
  layer, allowing lower tracks to show through.
- Mask-only mode: show grayscale mask values only for debugging.

Mask feathering must preserve grayscale alpha. A hard black/white threshold is
only appropriate for a deliberately binary matte, not for normal feathered SAM
masks.

## Normalization Rules

Whenever mask clips are loaded, created, or edited, JCut should normalize the
source/virtual pair:

- the source clip keeps `maskForegroundLayerEnabled = true`
- the source clip keeps `maskShowOnly = false` during normal virtual-clip use
- the virtual clip keeps `maskForegroundLayerEnabled = false`
- the virtual clip keeps `maskShowOnly = false`
- the virtual clip copies the source mask parameters
- the virtual clip copies the source transform and transform keyframes
- the virtual clip copies the source timing fields

Normalization makes old project files safer and keeps current projects from
accumulating contradictory mask state.

## Failure Modes

If the preview shows a pure white body/silhouette, check for these states first:

- `Show mask only` is enabled on the source or virtual clip.
- The renderer is drawing the matte texture instead of the source video clipped
  by the matte.
- Mask grade is being applied in a separate overlay pass instead of being applied
  to the masked foreground video pixels.
- Feathering or preprocessing has thresholded the mask to black/white instead of
  preserving grayscale alpha.

If the foreground does not reveal lower tracks:

- Confirm the virtual mask clip is above the lower clip in track/Z order.
- Confirm the draw pass outside the mask outputs transparent pixels.
- Confirm the virtual clip is not being drawn as a normal opaque copy of the
  source media.
- Confirm the source/virtual pair has been normalized after edits.

If the virtual clip drifts from the source:

- Check `linkedSourceClipId`.
- Check `syncLockedToSource`.
- Check `sourceTransformLocked`.
- Re-run or trigger mask-matte normalization.

## Implementation Contract

The construction contract lives in `makeSamMaskMatteClip`:

- duplicate the source clip's media identity and timing
- set `clipRole = mask_matte`
- link back to the source clip
- copy `maskFramesDir`
- disable audio
- lock the clip to the source
- keep `maskShowOnly = false`

The repair contract lives in `normalizeSamMaskMatteClips`:

- source clip remains the owner
- virtual clip is brought back into sync
- mask and transform parameters are shared
- old or invalid virtual-clip state is corrected

Preview and export must use the same compositing rule: the virtual mask layer is
source video clipped by grayscale mask alpha, with optional mask grading applied
to the mask-present portion only.
