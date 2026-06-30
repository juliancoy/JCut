# Effects, Masks, and Generated Timeline Clips

## Goals

- Treat every SAM result with a usable mask as a first-class generated clip, not only as hidden per-clip metadata.
- Keep generated mask clips exactly locked to their source clip in trim, rate, source-in, duration, and timeline position.
- Allow media to be placed between the original clip and its mask/matte companion so the mask can reveal, isolate, or foreground over other video and image layers.
- Extend effects into procedural video-synth clips that can occupy their own timeline tracks.
- Support image/video driven synthesis, including multiplying one image into a background and translating repeated copies in alternating motion patterns.
- Improve speaker title workflows so transcript speaker introductions can produce lower-third style graphics for a short time, then disappear.

## Timeline Model

Generated clips should be normal `TimelineClip` entries with additional semantic metadata:

- `clipRole = media`: ordinary user media.
- `clipRole = mask_matte`: a generated SAM mask/matte companion for another clip.
- `clipRole = effect_synth`: a procedural synthesizer clip that renders repeated, transformed, or generated imagery.
- `clipRole = speaker_title`: a generated lower-third/title clip derived from transcript speaker changes.

Generated clips must keep a stable `linkedSourceClipId` pointing at the source clip. For SAM masks, `generatedFromMaskId` records the mask artifact identity or mask frames directory. `syncLockedToSource` means timeline edits should treat the generated clip as a temporal follower of the source clip.

The important rule is that generated clips are not a second copy of decoded media with independent timing. They are a synchronized expression of the same source timing, with their own rendering role.

## SAM Mask Clips

Each clip with a SAM mask should be expressible as a paired mask/matte clip:

- Same `filePath`, media type, source FPS, source duration, source-in, playback rate, start frame, and duration as the original.
- Same transform timing unless the user explicitly detaches it.
- `maskEnabled = true` and `maskFramesDir` copied from the source.
- `maskShowOnly` or equivalent matte rendering enabled when the clip is intended to act as a mask layer.
- `maskForegroundLayerEnabled` remains useful for the existing immediate foreground pass, but the generated mask clip is the long-term timeline representation.

This enables stacks such as:

1. Original plate clip.
2. Inserted background image/video/effect synth.
3. Synchronized SAM foreground/matte clip.

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
- Use `makeSamMaskMatteClip` as the construction contract for generated SAM matte clips: same media/timing as source, visual-only, linked back to the source clip, and `maskShowOnly` enabled.
- Use the timeline context menu's Generated Clips commands to create/update SAM mask mattes and create alternating motion background synth clips.
- Use the timeline context menu's Transcript command to create/update transcript-derived speaker title clips.
- Keep generated clip timing deterministic and based on the source clip, not wall clock.
- Do not duplicate media caches unnecessarily; generated clips should share decode identity where possible.
- Direct Vulkan preview and export must use the same effect geometry and blend semantics.
- Transcript-generated titles should be idempotent: regenerating should update or replace previously generated speaker-title clips instead of accumulating duplicates.

## Phases

1. Done: persist generated clip roles, source links, and sync-lock metadata.
2. Done: add the alternating motion background preset to the existing effect preset system.
3. Done: add a command that creates synchronized SAM mask/matte clips from clips with `maskFramesDir`.
4. Done: add effect synth clip creation UI and track placement rules.
5. Done: generate speaker lower-thirds from transcript speaker introductions.
6. Remaining: add blend modes, starting with multiply, to Direct Vulkan preview and export.
7. Remaining: add project-level batch regeneration/update commands for all SAM clips, effect synth clips, and speaker titles.
