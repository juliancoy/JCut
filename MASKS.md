# Mask Architecture

## Ownership

JCut represents a masked foreground as three explicit concerns:

1. The source media clip owns decoded video, audio, timing, and source transforms.
2. A mask sidecar owns matte samples and coverage metadata.
3. The `MaskMatte` virtual clip owns compositing parameters and all grading, including grading keyframes.

The source and matte may share a decoded video frame, but they never share evaluated visual effects. Preview diagnostics expose this distinction as `media_owner_clip_id` and `effects_owner_clip_id`.

## Sidecars

`MaskSidecar` is the discovery descriptor. It contains:

- a stable SHA-256-derived ID based on the canonical sidecar directory;
- a user-facing name;
- the canonical directory;
- source type;
- frame count and first/last frame coverage.

The Masks tab discovers all `<media>_sam3_*_binary_masks` directories beside the selected media and displays their coverage. The selected directory remains serialized as `maskFramesDir` for backward compatibility; UI code does not use the path as display identity.

## Masks tab

The Masks tab owns only:

- sidecar selection and enablement;
- dilation and erosion;
- feather radius, falloff profile, and power;
- blur and inversion;
- matte-only display, opacity, and drop shadow.

It contains no grading controls or grading callbacks.

## Grading

Select the `MaskMatte` virtual clip and use the normal Grade tab. The matte supports the same grading model as any other visual clip:

- brightness, contrast, and saturation;
- lift, gamma, and gain;
- RGB and luminance curves;
- arbitrary grading keyframes and interpolation.

Preview and export evaluate the matte clip's standard grading state. The linked source supplies decoded media only.

## Project migration

Mask architecture version 2 replaces masked-area grading fields with ordinary grading on the virtual matte.

On loading a project with `maskArchitectureVersion < 2`:

1. legacy `maskGrade*` values and curves are copied to a frame-zero `GradingKeyframe` on the linked `MaskMatte`;
2. the matte base brightness, contrast, and saturation are updated;
3. legacy enable flags are cleared to prevent double application;
4. normal matte synchronization runs;
5. subsequent saves write `maskArchitectureVersion: 2` and no longer serialize `maskGrade*` fields.

Legacy fields remain readable solely so old projects can migrate. They are not part of the current UI or active rendering path.

## Preview handoff

Locked mask mattes avoid redundant decoding by reusing their linked source frame. The handoff copies only media payload state. It then evaluates and attaches the matte's own grade, curve LUT, feathering, and effect state.

The REST profile endpoint reports both owners and the effective grading values in `preview.decode_status_details`. For a matte, `media_owner_clip_id` should be the linked source and `effects_owner_clip_id` should be the matte ID.

## Invariants

- Source and matte grades are independent.
- Matte normalization never overwrites `gradingKeyframes`.
- A sidecar switch changes matte samples, not grading ownership.
- Legacy masked-area grading is migrated once and cannot be applied twice.
- Preview and export use the same standard matte grading representation.

## Verification

Focused coverage includes sidecar metadata/discovery, legacy grade migration, matte normalization, grading serialization, CPU mask compositing, Vulkan status handoff, and preview/export parity.
