# Mask Architecture

## Terminology

Use these terms consistently in code, UI, tests, logs, and documentation:

- **source parent**: the ordinary media `TimelineClip` that owns the media and its
  timeline-to-source mapping;
- **Mask Matte child**: a visual-only `TimelineClip` with
  `clipRole = mask_matte` and `linkedSourceClipId` set to the source parent's ID;
- **mask sidecar**: the on-disk artifact that supplies matte samples and coverage
  metadata.

After the relationship is established, **parent** and **child** are acceptable
shorthand. `MaskMatte` is the serialized enum spelling; user-facing text is
**Mask Matte**. Do not call a Mask Matte a "Mask Z Marker." Z-level is only a
compositing property of the child, not its identity or relationship.

## Ownership

JCut represents a masked foreground as one parent-child aggregate with three
explicit concerns:

1. The source parent owns decoded video, audio, timing, source-frame mapping,
   render-sync markers, and source transforms.
2. A mask sidecar owns matte samples and coverage metadata.
3. The Mask Matte child owns its sidecar association and visual treatment:
   visibility, Z-level, mask processing, opacity, shadow, effects, grading, and
   correction polygons, including the keyframes for those properties.

The source parent and Mask Matte child may share a decoded video frame, but they
never share evaluated visual effects. Preview diagnostics expose this distinction
through four explicit owner IDs:

| Concern | Owner |
| --- | --- |
| Media | source parent |
| Timing | source parent |
| Effects | Mask Matte child |
| Matte | Mask Matte child |

Preview and export must fail closed if a Mask Matte layer does not resolve to
that parent/parent/child/child mapping.

The following parent fields are authoritative and are always derived for a child;
copies stored on the child are serialization or UI caches only:

- media path/type, decode/cache identity, source FPS, and source duration;
- timeline start (including subframe position), source-in, duration, and playback
  rate;
- render-sync markers and the resulting timeline-to-source mapping;
- base transform, transform keyframes, and evaluated transform.

All reads must resolve these values through the parent. All mutations must update
the parent aggregate and normalize any cached child fields at the same model
boundary. A Mask Matte cannot be moved, trimmed, retimed, marker-edited, or
transformed independently. Transform detachment is not part of this model; adding
it later requires an explicit new relationship state rather than silently clearing
a lock flag.

`linkedSourceClipId` on the child is the canonical relationship. Generated-track
fields such as `parentClipId` and `childClipId` are derived presentation indices,
not a second ownership graph. Model reconciliation rebuilds or repairs those
indices from the clip relationship and leaves at most one track binding for each
child.

## Sidecars

`MaskSidecar` is the discovery descriptor. It contains:

- a stable SHA-256-derived ID based on the canonical sidecar directory;
- a user-facing name;
- the canonical directory;
- source type;
- frame count and first/last frame coverage.

The Masks tab discovers every compatible mask sidecar beside the selected media. This includes SAM2/SAM3 and BiRefNet output, plus other AI-generated sibling directories whose names identify mask, matte, segmentation, or alpha output. A `jcut_alpha.json` manifest also marks a generic directory as a sidecar. Discovery is deterministic and each canonical directory has a stable identity.

Every discovered sidecar may be materialized as its own Mask Matte child. The directory remains serialized as `maskFramesDir` for backward compatibility; UI code uses the stable sidecar identity and descriptive name instead of treating the path as display identity.

Sidecar reconciliation is parent-scoped and idempotent. The identity key is the
pair `(linkedSourceClipId, generatedFromMaskId)`; the canonical directory is only
a legacy fallback when an older project has no stable sidecar ID. Re-running
discovery or generation updates the matching child and must not append another
child or track binding for the same association.

Only an ordinary video `Media` clip may be a source parent. A generated effect,
speaker-title clip, image, or other unsupported role/type cannot acquire Mask
Matte children. If malformed persisted state contains more than one child for the
same parent and stable sidecar identity, normalization retains the first child in
persisted timeline order (including all of its child-owned visual state) and
removes the later duplicates.

An unavailable sidecar is not an orphaned child. Missing, unmounted, or temporarily
unreadable sidecar files must leave the persisted child and all child-owned visual
state intact. The child renders fail-closed (no masked foreground), reports the
sidecar as unavailable, and resumes using the same association if the artifact
returns. Reconciliation must never substitute a different sidecar merely because
its name or coverage looks similar.

## Timeline hierarchy and compositing

Each Mask Matte is shown on a compact child track immediately below its source
parent's track. The child track retains independent visibility and grading-preview
state. Its position in the timeline hierarchy is presentation only and does not
determine compositing order.

Child-track visibility is authoritative in both preview and export. A hidden Mask
Matte child neither composites nor keeps its source parent active as a decode
provider. A hidden source parent may still decode for any visible child, but that
provider-only source status is never drawn as a full-frame layer.

Every visual clip has an explicit Z-level. Mask Matte children initially receive
consecutive levels above their parent, and users may edit each level independently
in the Masks tab. Changing a Z-level never moves a child track. Older projects
without explicit levels are assigned deterministic values during migration.

## Aggregate lifecycle

Parent-child edits are atomic model operations. UI widgets, importers, project
loaders, undo/redo, and scripting bridges must call the same operations rather
than modifying one clip and relying on a later repair pass.

### Create and reconcile

- Creating a child requires an existing source parent and a stable sidecar
  association.
- There is at most one automatically materialized child for a given parent and
  sidecar identity. Repeating the operation updates that child in place.
- Reconciliation normalizes parent-derived caches and track bindings, but never
  overwrites child-owned visual state.
- A Mask Matte whose `linkedSourceClipId` does not resolve is a true orphan. Project
  load/model reconciliation removes the orphan and its generated-track binding;
  it must not guess a parent by file path, track position, name, or adjacency.
- A missing sidecar is handled as unavailable, not as an orphan, and therefore does
  not cause child deletion.

### Delete, cut, and disable

- Deleting an unlocked source parent cascades to all of its Mask Matte children
  and their generated-track bindings in the same undoable operation.
- Cutting an unlocked source parent to the timeline clipboard applies the same
  cascade at the original location; the clipboard payload contains the complete
  aggregate.
- Direct timeline delete or cut of a locked Mask Matte child is disallowed. A
  discovered sidecar association is durable project state: disable its child in
  the Masks tab (or hide its generated child track) when it should not composite.
  Re-enabling the same association restores the same child-owned treatment.
- Individual association deletion is intentionally not inferred from a missing
  file or a timeline-row delete. Permanent removal would require an explicit,
  persisted sidecar-suppression record so discovery cannot silently recreate it;
  that state is not part of the current project schema.

### Split

Splitting a source parent at a valid edit point splits the complete aggregate at
the same timeline position. The left parent retains its ID and existing children.
The right parent receives a new ID, and every right-side child receives a new ID
whose `linkedSourceClipId` is remapped to the new parent. Both halves retain the
same stable sidecar association; source-in and source-frame selection remain
derived from their respective parent halves.

Child-owned keyframes and effects follow the ordinary clip-split rule: values at
the boundary are preserved, left-side keys remain with the left child, and
right-side keys are copied/rebased to the right child's local domain. A split must
not leave a truncated child without a corresponding right-side child.

### Copy and paste

The timeline clipboard preserves relationship closure. Copying a source parent
includes all Mask Matte children even if their compact tracks were not selected.
Selecting a child for timeline copy expands the payload to its parent and sibling
children; copying only mask settings is a separate parameter-copy operation, not
a timeline-clip copy.

Paste allocates new IDs for every parent and child in the payload, then remaps all
`linkedSourceClipId` values to the new parent IDs before insertion. It must never
link pasted children back to clips at the copied location. Track bindings are
recreated from the remapped clip graph.

## Masks tab

The Masks tab owns only:

- sidecar selection and enablement;
- dilation and erosion;
- feather radius, falloff profile, and power;
- blur and inversion;
- matte-only display, opacity, and drop shadow.

Changing sidecars never writes a directory by itself: `maskFramesDir` and its
derived `generatedFromMaskId` are one atomic association update. When the chosen
sidecar already has a materialized sibling child, the tab selects that child
instead of retargeting the currently selected child's grading and mask treatment.
When the source parent is selected, the tab resolves its current or first
discovered sidecar to the corresponding child before enabling treatment controls.
If that child does not exist yet, the timeline materializes it directly from the
sidecar descriptor; the parent is never used as temporary storage for child-owned
sidecar or treatment fields.

It contains no grading controls or grading callbacks.

## Grading

Select the Mask Matte child and use the normal Grade tab. The child supports the same grading model as any other visual clip:

- brightness, contrast, and saturation;
- lift, gamma, and gain;
- RGB and luminance curves;
- arbitrary grading keyframes and interpolation.

Preview and export evaluate the Mask Matte child's standard grading state. The
source parent supplies decoded media only.

## Project migration

Mask architecture version 2 replaces masked-area grading fields with ordinary grading on the virtual matte.

On loading a project with `maskArchitectureVersion < 2`:

1. legacy `maskGrade*` values and curves are copied to a frame-zero `GradingKeyframe` on the linked Mask Matte child;
2. the matte base brightness, contrast, and saturation are updated;
3. legacy enable flags are cleared to prevent double application;
4. normal matte synchronization runs;
5. subsequent saves write `maskArchitectureVersion: 2` and no longer serialize `maskGrade*` fields.

Legacy fields remain readable solely so old projects can migrate. They are not part of the current UI or active rendering path.

## Preview handoff

Mask Matte children avoid redundant decoding by reusing their source parent's
resolved frame. The parent timeline-to-source mapping, including render-sync
markers, is evaluated once; preview and export apply the sidecar sample for that
exact source frame. The handoff copies only media payload state. It then evaluates
and attaches the child's own grade, curve LUT, feathering, correction polygons,
and effect state. Correction polygons erase the corresponding region from the
matte itself in both preview and export; they never mutate the source parent's
decoded frame.

Effect presets are stored on the Mask Matte child, never on its generated track.
Presets that need independently decoded source-history frames (`Difference Matte`
and `Temporal Echo`) are currently unavailable for Mask Matte children. If an
older project contains either value, the setting remains serialized but preview
and export render it inactive; they must never fall back to an unmasked full-frame
draw. Supporting those presets later requires resolving every auxiliary frame
through the parent and retaining the child's mask on every layer.

The REST profile endpoint reports both owners and the effective grading values in `preview.decode_status_details`. For a matte, `media_owner_clip_id` should be the linked source and `effects_owner_clip_id` should be the matte ID.

## Invariants

- Source-parent and Mask Matte grades are independent.
- Each materialized parent-sidecar association has a separate Mask Matte child and
  mask parameter set.
- Parent timing, media mapping, render-sync markers, decoded frame, and transforms
  are authoritative for every child in preview and export.
- A parent edit and all resulting child normalization form one undoable aggregate
  mutation.
- A child never exists without a resolvable parent; a child may persist while its
  sidecar is unavailable.
- A materialized sidecar association is disabled through its child visibility;
  direct child deletion and missing sidecar files never masquerade as association
  removal.
- Split, delete, cut, copy, and paste preserve relationship closure and remap IDs
  deterministically.
- `linkedSourceClipId` is authoritative; generated-track relationship fields are
  derived and cannot create ownership.
- Timeline hierarchy and Z-level are independent concerns.
- Mask Matte normalization never overwrites `gradingKeyframes`.
- A sidecar switch changes matte samples, not grading ownership.
- Legacy masked-area grading is migrated once and cannot be applied twice.
- Preview and export use the same standard matte grading representation.
- Preview and export apply child correction polygons to the same matte sample.
- Unsupported source-history effects remain persisted but render inactive and
  fail closed in both preview and export.

## Verification

Focused coverage includes sidecar metadata/discovery, legacy grade migration,
Mask Matte normalization, grading serialization, CPU mask compositing, Vulkan
status handoff, lifecycle operations, and preview/export parity.
