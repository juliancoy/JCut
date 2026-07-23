# Effects, Masks, and Generated Timeline Clips

## Goals

- Treat every usable mask sidecar as a first-class Mask Matte child, not only as
  hidden per-clip metadata.
- Keep each Mask Matte exactly locked to its source parent in media identity,
  timeline position, trim, rate, source-in, duration, render-sync mapping, and
  transforms.
- Allow media to be placed between the source parent and its Mask Matte child so
  the mask can reveal, isolate, or foreground over other video and image layers.
- Extend effects into procedural video-synth clips that can occupy their own timeline tracks.
- Support image/video driven synthesis, including multiplying one image into a background and translating repeated copies in alternating motion patterns.
- Improve speaker title workflows so transcript speaker introductions can produce lower-third style graphics for a short time, then disappear.

## Timeline Model

Generated clips should be normal `TimelineClip` entries with additional semantic metadata:

- `clipRole = media`: ordinary user media.
- `clipRole = mask_matte`: a generated Mask Matte child of a source parent.
- `clipRole = effect_synth`: a procedural synthesizer clip that renders repeated, transformed, or generated imagery.
- `clipRole = speaker_title`: a generated lower-third/title clip derived from transcript speaker changes.

Generated clips that depend on media keep a stable `linkedSourceClipId` pointing
at that source. For a Mask Matte, this field is the canonical parent-child
relationship. `generatedFromMaskId` records the stable sidecar identity; a mask
frames directory is only a backward-compatible fallback for older projects.
`syncLockedToSource` identifies a strict follower to editing code, but the lock
flag is not a second source of truth and does not make cached child timing
authoritative.

The important rule is that a Mask Matte is not a second copy of decoded media with
independent timing. It is a visual expression of the parent's resolved source
frame. Other generated roles may have independent timing where their own contract
allows it.

## Mask Matte Clips

Each source parent with a rotoscope sidecar can have a Mask Matte child:

- Media identity, source FPS/duration, timeline start/subframe, source-in, playback
  rate, duration, render-sync markers, and transforms are derived from the parent.
  Serialized copies on the child are normalization caches, not editable state.
- Transform detachment is unsupported. A future detached mode requires an explicit
  relationship state and preview/export semantics.
- `maskEnabled` is child-owned and initially true; the child also owns
  `generatedFromMaskId`, `maskFramesDir`, and its mask processing and visual
  settings.
- Mask Matte children render the parent's resolved source image clipped by the
  sidecar. `maskShowOnly` remains a child-owned diagnostic view of the matte.
- Effect presets are child-owned clip state. The generated child track is only a
  presentation binding and is not an effect owner. `Difference Matte` and
  `Temporal Echo` are disabled for Mask Matte authoring because they require
  independently decoded history frames; persisted legacy values are retained but
  render inactive and fail closed in preview and export.
- `maskForegroundLayerEnabled` remains useful for the existing immediate foreground pass, but the Mask Matte child is the long-term timeline representation.

This enables stacks such as:

1. Original plate clip.
2. Inserted background image/video/effect synth.
3. Synchronized Mask Matte foreground child.

The complete ownership, reconciliation, orphan, split, delete/cut, and clipboard
contract is defined in `MASKS.md`. In particular, structural operations preserve
the complete parent-child closure and paste remaps both parent and child IDs.

## Effect Synth Clips

Effects should be able to render without pretending to be ordinary footage. The first concrete procedural pattern is an alternating motion background:

- Source image or video is tiled across the output.
- Rows translate horizontally.
- Alternating rows can move in opposite directions.
- `effectRows`, `effectSpeed`, `effectScale`, and `effectAlternateDirection` control density, motion, size, and row direction.
- A future blend mode field should support `normal`, `multiply`, `screen`, and `add`; multiply is the requested first non-normal target.

Effect synth clips should eventually support:

- Image/video input as a texture source.
- Procedural patterns with deterministic frame-time evaluation.
- Track-level compositing and blend modes.
- Keyframeable effect parameters.
- Export parity with Direct Vulkan preview.

## Speaker Titles

Speaker title generation should use transcript speaker transitions:

- Detect when the active transcript speaker changes.
- Create a short `speaker_title` generated clip at that introduction point.
- Use the speaker display name, title, and organization from the Speakers tab profile data.
- Render as a lower-third graphic near the bottom of the frame.
- Animate in, hold briefly, and animate out.

Existing transcript overlay speaker-title logic is useful for live overlay rendering. The longer-term direction is to also generate timeline title clips so the editor can trim, restyle, disable, or move individual introductions.

## Implementation Notes

- Persist clip role and source-link metadata before building complex UI. That keeps project files forward-compatible.
- Use `makeMaskMatteClip` as the construction contract for Mask Matte children:
  visual-only, linked to the source parent, parent-derived media/timing/mapping/
  transform, and rendered as the resolved parent image clipped by the sidecar.
- Route all Mask Matte creation, reconciliation, split, delete/cut, copy/paste,
  relink, and orphan handling through one aggregate model service. UI and project
  adapters must not mutate relationship fields independently.
- Use the timeline context menu's Generated Clips commands to create/update Mask
  Matte children and create alternating motion background synth clips.
- Use the timeline context menu's Transcript command to create/update transcript-derived speaker title clips.
- Keep generated clip timing deterministic and based on the source clip, not wall clock.
- Do not duplicate media caches unnecessarily; generated clips should share decode identity where possible.
- Direct Vulkan preview and export must use the same effect geometry and blend semantics.
- Transcript-generated titles should be idempotent: regenerating should update or replace previously generated speaker-title clips instead of accumulating duplicates.

## Phases

1. Done: persist generated clip roles, source links, and sync-lock metadata.
2. Done: add the alternating motion background preset to the existing effect preset system.
3. Done: add a command that creates synchronized Mask Matte children from clips with `maskFramesDir`.
4. Done: add effect synth clip creation UI and track placement rules.
5. Done: generate speaker lower-thirds from transcript speaker introductions.
6. Remaining: add blend modes, starting with multiply, to Direct Vulkan preview and export.
7. Remaining: add project-level batch regeneration/update commands for all SAM clips, effect synth clips, and speaker titles.
