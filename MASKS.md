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

The Masks tab discovers every compatible mask sidecar beside the selected media. This includes SAM2/SAM3 and BiRefNet output, plus other AI-generated sibling directories whose names identify mask, matte, segmentation, or alpha output. A `jcut_alpha.json` manifest also marks a generic directory as a sidecar. Discovery is deterministic and each canonical directory has a stable identity.

Every discovered sidecar is materialized as its own `MaskMatte` child. The directory remains serialized as `maskFramesDir` for backward compatibility; UI code uses the stable sidecar identity and descriptive name instead of treating the path as display identity.

## Timeline hierarchy and compositing

Each generated mask is shown on a compact child track immediately below its source clip's track. The child track retains independent visibility and grading-preview state. Its position in the timeline hierarchy is presentation only and does not determine compositing order.

Every visual clip has an explicit Z-level. Mask children initially receive consecutive levels above their source, and users may edit each level independently in the Masks tab. Changing a Z-level never moves a child track. Older projects without explicit levels are assigned deterministic values during migration.

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
- Each disk sidecar owns a separate matte child and mask parameter set.
- Timeline hierarchy and Z-level are independent concerns.
- Matte normalization never overwrites `gradingKeyframes`.
- A sidecar switch changes matte samples, not grading ownership.
- Legacy masked-area grading is migrated once and cannot be applied twice.
- Preview and export use the same standard matte grading representation.

## Verification

Focused coverage includes sidecar metadata/discovery, legacy grade migration, matte normalization, grading serialization, CPU mask compositing, Vulkan status handoff, and preview/export parity.
