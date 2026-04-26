# Transformations Pathway

This document captures the current transform flow for Subtitle Face Tracking and Face Stabilize.

## Goals

- Keep activation logic simple and explicit.
- Avoid legacy fallback behavior.
- Preserve geometric consistency when targets move.
- Provide deterministic behavior across UI and REST.

## Core Terms

- `tracking` (transcript speaker profile framing object): speaker BoxStream data (`keyframes`) plus explicit `enabled`.
- `Face Stabilize` (clip-level): uses `speakerFramingKeyframes` on a selected clip.
- `FaceBox target` (clip-level): `speakerFramingTargetXNorm`, `speakerFramingTargetYNorm`, `speakerFramingTargetBoxNorm`.
- `baked target` (clip-level solve baseline):
  - `speakerFramingBakedTargetXNorm`
  - `speakerFramingBakedTargetYNorm`
  - `speakerFramingBakedTargetBoxNorm`

## KISS Activation Rules

### Subtitle Face Tracking

Tracking is ON only when both are true:

1. BoxStream exists (`keyframes` non-empty).
2. `enabled == true`.

No implicit ON from refs/mode/legacy fields.

### Face Stabilize

Face Stabilize can be enabled only when:

1. A clip is selected.
2. Selected clip has non-empty `speakerFramingKeyframes`.

No bootstrap/autofill accommodations from transcript BoxStream on toggle.

## End-to-End Pathway

### 1. Generate tracking BoxStream (speaker profile)

Auto-Track writes transcript speaker framing `keyframes`, then sets `enabled=true`.

### 2. Solve clip stabilize keyframes

Runtime mode:

1. Generate BoxStream writes transcript speaker framing `keyframes` only.
2. Clip stores `speakerFramingSpeakerId` binding.
3. Face Stabilize transform is evaluated at runtime from BoxStream sample + FaceBox target.
4. No transform bake/smoothing is applied during Generate BoxStream.

### 3. Render transform composition

Render transform = base clip transform composed with speaker framing transform.

This applies in preview and GPU render paths.

## Geometric Flow (Current)

### Coordinate spaces

- `source_norm`: normalized coordinates in source media space `[0..1]`.
  - Examples: BoxStream `x/y`, `box_size`, `box_left/top/right/bottom`.
- `fitted_px`: pixels in fitted source rect inside output canvas (after aspect-fit).
  - Examples: `fittedWidth`, `fittedHeight`, `fittedMinSide`.
- `output_px`: pixels in final output canvas.
  - Examples: `targetXPx`, `targetYPx`, `targetSideOutputPx`.
- `clip_local_px`: per-key translation delta from fitted center before clip transform compose.
  - Examples: `localX`, `localY`, `translationX`, `translationY`.

### Scale fit correction

Scale solve now uses direct output-space pixels:

1. `targetSideOutputPx = targetBoxNorm * min(outputWidth, outputHeight)`
2. `faceSideOutputPx = box_size * min(fittedWidth, fittedHeight)` (canonical path)
3. `scale = targetSideOutputPx / faceSideOutputPx`

`box_size` is treated as the canonical BoxStream size (normalized to source min-side).
Corner box fields are only a fallback when `box_size` is missing.

Space transition summary:

- `source_norm -> fitted_px`: multiply by fitted dimensions.
- `fitted_px -> output_px`: implicit through solved `scale` and fitted center anchoring.

### Live target retargeting

When FaceBox `X/Y` changes:

1. Compute output-space delta from baked target to current target.
2. Shift existing `speakerFramingKeyframes` translation by that delta.
3. Update baked `X/Y` to new target.

This avoids cumulative drift and keeps repeated edits stable.

## Serialization Contract

Clip state now persists:

- `speakerFramingTargetXNorm/YNorm/BoxNorm`
- `speakerFramingBakedTargetXNorm/YNorm/BoxNorm`

Backward compatibility:

- If baked fields are missing, load defaults baked values from current target values.

## REST/UX Behavior

- Disabled actions return explicit reasons where possible (for example, no selected clip, zero face keyframes).
- Selected clip state is exposed to REST state payload and improved selected-clip border visibility exists in timeline rendering.

## Invariants

1. Tracking enablement is explicit (`enabled` + BoxStream), never inferred from refs.
2. Face Stabilize enablement is clip-keyframe gated.
3. Auto-Track owns keyframe solve.
4. Target retargeting uses baked baseline and is deterministic.
5. No legacy accommodations that silently mutate missing prerequisites.

## Future Work (Optional)

- Add full live `Size` retarget for existing keyframes using BoxStream-aware re-solve.
- Add explicit diagnostics panel for:
  - selected clip id
  - face key count
  - tracking enabled status
  - baked target vs current target deltas
