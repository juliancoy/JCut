# Tabs Functionality Map

This file documents the current inspector tab functionality implemented in code.

Primary wiring:
- Tab creation/order: `inspector_pane.cpp` (`InspectorPane` constructor)
- Tab logic controllers: `editor_tabs.cpp` (`EditorWindow::setupTabs()` + per-tab classes)

Current tabs (in UI order):

## 1) Grade
Purpose:
- Visual color grading for selected visual clip.

Key functionality:
- Edit mode switch: `Levels` vs `Curves`.
- Levels controls: brightness, contrast, saturation.
- Curves-style controls: shadows/midtones/highlights RGB lift/gamma/gain, channel selection, histogram widget.
- Keyframe workflow: auto-scroll, follow current keyframe, preview toggle, key-at-playhead, keyframe table.

## 2) Opacity
Purpose:
- Per-clip opacity editing with keyframes.

Key functionality:
- Base opacity value control.
- Keyframe workflow: auto-scroll, follow current keyframe, key-at-playhead, fade in/out, fade duration.
- Opacity keyframe table with interpolation.

## 3) Effects
Purpose:
- Clip effects controls currently focused on alpha mask feathering.

Key functionality:
- Enable/disable mask feathering.
- Feather radius and feather gamma controls.
- Guidance text clarifying alpha-channel-only applicability.

## 4) Corrections
Purpose:
- Polygon-based correction/erase workflow for selected visual clip.

Key functionality:
- Enable/disable corrections.
- Polygon ranges table (on/start/end/points).
- Vertex table for selected polygon.
- Draw controls: draw mode, draw polygon, close polygon, cancel draft, delete last, clear all.
- Tab-switch behavior: leaving Corrections disables preview correction draw mode.

## 5) Titles
Purpose:
- Title overlay/keyframed text editing.

Key functionality:
- Title text edit.
- Position (X/Y), font family/size.
- Style: bold, italic, color, opacity.
- Shadow controls: enable, color, opacity, offset.
- Window/background controls: enable, color, opacity, padding.
- Frame controls: enable, color, opacity, width, gap.
- Keyframe actions: add at playhead, remove selected.
- Quick alignment: center horizontal/vertical.
- Auto-scroll and keyframe table.
- Tab-switch behavior: preview title interaction is enabled only in Titles tab.

## 6) Sync
Purpose:
- Render sync marker inspection/editing.

Key functionality:
- Sync table with clip/frame/count/action columns.
- Clear-all-sync-points action.
- Editor handlers support selection, item changes, and context-menu operations.

## 7) Keyframes
Purpose:
- Transform keyframes for selected visual clip.

Key functionality:
- Translation X/Y, rotation, scale X/Y.
- Interpolation mode.
- Mirror horizontal/vertical.
- Lock scale option.
- Frame-space options: clip-relative frames and skip-aware timing.
- Add/remove keyframe, flip horizontal.
- Auto-scroll/follow-current checkboxes + keyframe table.

## 8) Transcript
Purpose:
- Transcript cut/version editing, speech filtering, transcript playback follow, and overlay config.

Key functionality:
- Editable cut title.
- Cut version selector + create/delete cut actions.
- Overlay controls: enable, window visibility, max lines/chars, follow current word, position/size, font/style, unified edit colors.
- Speaker filtering and “show lines not in active cut”.
- Speech Filter controls: enable, prepend/postpend ms, fade length, boundary crossfade.
- Playback controls for transcript flow: clock source and audio warp mode.
- Transcript table columns: source start, source end, speaker, text, edits.
- Context menus and edit operations (insert, delete, skip/unskip, etc.) via `TranscriptTab`.
- Delete-current-transcription action is context-aware and disabled when not valid.

## 9) Speakers
Purpose:
- Speaker profile editing for active transcript cut.

Key functionality:
- Speakers table: speaker id, name, X, Y.
- Supports naming speakers and setting speaker on-screen positions.

## 10) Properties
Purpose:
- General selected-clip properties plus track/audio inspector controls.

Key functionality:
- Selected clip labels + proxy/original/playback source information.
- Clip playback speed control.
- Track-level editing: name, height, visual mode, audio enabled.
- Track crossfade duration + “Crossfade Consecutive Clips” action.
- Audio inspector details for selected audio clip.

## 11) Clips
Purpose:
- Timeline clip list and quick clip management.

Key functionality:
- Clips table: name, track, type, start, duration, file.
- Context menu actions include select clip and delete.
- Track reassignment/edit handling integrated in `ClipsTab`.

## 12) History
Purpose:
- Snapshot history navigation.

Key functionality:
- History table (index + summary).
- Restore-to-snapshot behavior on row activation handled in `HistoryTab`.

## 13) Tracks
Purpose:
- Track-level visibility/audio status overview and edits.

Key functionality:
- Tracks table: track, visual, audio.
- Editor refresh/update handlers support per-track state changes.

## 14) Preview
Purpose:
- Preview-only controls (does not change final render output settings).

Key functionality:
- Hide content outside output window.
- Zoom controls: zoom factor + reset + usage guidance.
- Playback buffering controls: cache fallback, lead prefetch enable/count, playback window ahead, visible queue reserve.

## 15) Output
Purpose:
- Render/export configuration and decoder/cache pipeline tuning.

Key functionality:
- Export dimensions, start/end frame, output format.
- Render options: use proxies, optionally create video from image sequence.
- Autosave interval and backup count.
- Render background color.
- Decoder/cache controls: cache fallback, lead prefetch toggles and queue parameters, decoder lane count, render pipeline mode.
- Deterministic pipeline toggle + reset defaults.
- Render action button.

## 16) System
Purpose:
- Runtime/system diagnostics and decoder policy controls.

Key functionality:
- H.264/H.265 CPU threading mode selection.
- Profile summary table.
- Decode benchmark action.
- Restart all decoders action.

## 17) Projects
Purpose:
- Project list and project lifecycle actions.

Key functionality:
- Project list view.
- Actions: New, Save As, Rename.
- Project selection/switch behavior managed in `ProjectsTab`.

## Refresh and Routing Notes
- Global inspector refresh routing is in `EditorWindow::setupInspectorRefreshRouting()`.
- That refresh fan-out updates both class-backed tabs (Transcript, Projects, Output, etc.) and direct editor-managed tables (Sync, Tracks, Properties).
