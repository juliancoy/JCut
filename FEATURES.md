# JCut Features

## Overview

JCut is a desktop, timeline-based video editor focused on fast iterative editing with deep per-clip controls, transcript tooling, and integrated render/export.

## Core Editor Features

- Multi-track timeline editing with clip selection, split, trim, nudge, and razor/select tool modes
- Real-time preview with per-clip transforms, grading/opacity/effects, title overlays, and transcript overlays
- Async FFmpeg decoding with timeline caching and configurable playback prefetch behavior
- Clip proxy workflow and playback-source awareness
- Speech-filter controls tied to transcript timing
- Offline rendering to video formats and image sequence formats
- Project management with multiple projects and persistent editor state/history
- Local REST-style control server for health/state/profile/diagnostic automation

## Inspector Tabs (Breakdown by Tab)

### Grade
- Per-clip color controls: brightness, contrast, saturation
- Lift/Gamma/Gain-style RGB controls for shadows/midtones/highlights
- Keyframe table for grading values + interpolation
- Histogram + channel curve workflow
- Auto-scroll/follow-current controls for keyframe navigation

### Opacity
- Per-clip opacity control and keyframing
- Keyframe table with interpolation mode
- Fade-in/fade-out generation from playhead with duration setting
- Auto-scroll/follow-current controls

### Effects
- Alpha-mask feathering enable/disable
- Feather radius and gamma controls
- Visual-only applicability checks (e.g., alpha-capable clips)

### Corrections
- Polygon-based correction masks over preview
- Draw/close/cancel polygon workflow
- Polygon list with start/end frame range and enabled state
- Vertex table for precise XY editing
- Delete-last and clear-all mask actions

### Titles
- Title text keyframes with timing and interpolation
- Position, font family/size, bold/italic, and opacity controls
- Text color, drop-shadow controls, background window controls, window frame controls
- Add/remove keyframes, center-horizontally, center-vertically helpers
- Auto-scroll keyframe navigation

### Sync
- Render sync marker table (clip/frame/count/action)
- Global "clear all sync points" action

### Keyframes
- Transform keyframes: translation, rotation, scale, interpolation
- Mirror horizontal/vertical toggles and lock-scale behavior
- Clip-relative keyframe frame space option
- Skip-aware timing option
- Add/remove keyframe and flip-horizontal actions
- Table-driven keyframe editing with follow-current controls

### Transcript
- Transcript table with source/render timing, speaker, text, and edit flags
- Editable cut label and cut-version management (new/delete/switch)
- Overlay settings: enable/window, line/char limits, position/size, font styling
- Speaker filtering and outside-cut line visibility controls
- Speech filter controls (prepend/postpend/fade samples)
- Row-level transcript edit workflows with table context actions

### Speakers
- Speaker identity table for active transcript cut
- Editable speaker name and on-screen location (X/Y)

### Properties
- Clip playback rate and playback-source/proxy info
- Track-level controls: name, height, visual mode, audio enable
- Track crossfade duration + apply action for consecutive clips
- Audio clip information surface

### Clips
- Tabular clip inventory (name, track, type, start, duration, file)
- Inline edits for selected fields (e.g., track assignment)
- Context actions including selection and clip deletion

### History
- Timeline/project history list with index and summary
- Double-click restore to prior snapshot

### Tracks
- Track table with visual/audio enabled state indicators
- Quick per-track visibility and audio control surface

### Preview
- Preview-only controls (non-destructive to output)
- Hide content outside output window toggle
- Zoom controls with reset
- Playback buffering controls (cache fallback + lead prefetch knobs)

### Output
- Output dimensions and export frame range
- Render format selection (video formats and image sequences)
- Render toggles (proxies, sequence-to-video path where applicable)
- Background color selection for output
- Decoder/cache pipeline tuning controls for render path
- Autosave interval and backup retention controls
- Render trigger and range summary

### System
- System/profile summary table
- Decode benchmark trigger
- Decoder restart action
- H.264/H.265 CPU threading mode selector

### Projects
- Project listing and selection
- New project, Save As, and rename workflows

## Non-Tab Features

- Explorer pane file browsing and timeline ingest
- Keyboard shortcut workflow (playback, split, nudge, undo/redo, tool switching)
- Timeline/export range integration with preview/audio/render pipelines
- Diagnostics and profiling surfaces through UI and control-server endpoints
