# Waveform Improvement Plan

## Goals
- Replace synthetic timeline waveform drawing with real sample-derived waveform data.
- Use a shared, professional waveform architecture across Timeline and Preview.
- Keep UI interaction smooth (zoom/pan/scrub) at large media durations.
- Support original and post-processed waveform views (normalize/amplify/etc.) without recomputing unnecessarily.

## Current State (Gap Summary)
- Timeline waveform is synthetic placeholder bars (sin-based), pixel-step driven.
- Preview waveform is decoded and cached, but currently bin-based and not a full multi-resolution min/max pyramid.
- Granularity currently controls render spacing in pixels, not analysis resolution in samples.

## Target Architecture

### 1. Canonical Waveform Data Model
- Create a canonical per-media waveform cache keyed by media fingerprint:
  - `absolute_path`
  - `mtime`
  - `file_size`
  - optional content hash (future enhancement)
- Base level stores min/max peaks for fixed sample windows (hop size in samples).
- Build LOD pyramid levels by aggregating adjacent windows:
  - Level 0: finest resolution (e.g., 256 or 512 samples/window)
  - Level N: progressively coarser (power-of-two merge)

### 2. Processing Pipeline
- Decode audio once per media source in worker thread.
- During decode, accumulate peak pairs into Level 0.
- Build higher LOD levels after decode completion.
- Store metadata:
  - sample rate
  - channel count
  - total samples
  - duration
  - generation timestamp

### 3. Rendering Pipeline
- Convert viewport time range to sample range.
- Select best LOD for current zoom and viewport pixel density.
- Render min/max envelope columns (not synthetic bars).
- Keep rendering thread/UI thread read-only over immutable cache snapshots.

### 4. Post-Processed Variants
- Keep `original` waveform cache immutable.
- Build `processed` waveform cache variants for active dynamics settings (amplify/normalize/limiter/compressor).
- Variant key should include:
  - source waveform fingerprint
  - dynamics settings signature
- Reuse original decode results; avoid re-decoding media for each settings change.

### 5. Unified Service Layer
- Introduce a shared `WaveformService` used by both Timeline and Preview.
- Responsibilities:
  - async decode scheduling
  - cache lookup/invalidation
  - LOD selection helper
  - processed variant generation
- Timeline/Preview become thin clients that request:
  - `WaveformSlice` for a sample range + viewport width

## Data Structures
- `WaveformFingerprint`
- `WaveformLevel { window_samples, QVector<float> minVals, QVector<float> maxVals }`
- `WaveformPyramid { levels[], sample_rate, total_samples, channels }`
- `WaveformVariantKey { fingerprint, dynamics_signature }`
- `WaveformSlice { minVals/maxVals aligned to requested columns }`

## API/Interface Plan
- `requestWaveform(path, fingerprint)` -> async handle
- `waveformReady(path, fingerprint)` signal/callback
- `querySlice(path, fingerprint, sample_start, sample_end, pixel_width, variant)` -> `WaveformSlice`
- `invalidate(path|fingerprint)`
- `setDynamicsVariant(settings)` / variant key helpers

## Migration Plan

### Phase 1: Foundation
- Implement `WaveformService` and waveform pyramid generation.
- Keep existing rendering path as fallback.
- Add instrumentation for decode/build timings and memory usage.

### Phase 2: Timeline Migration
- Replace synthetic timeline envelope with real min/max slice rendering.
- Change “granularity” preference from pixel step to sample-window setting.
- Keep a separate display density control only if needed for visual style.

### Phase 3: Preview Migration
- Move preview waveform bins to shared service slices.
- Preserve current speaker tint + wrapped rows behavior.
- Ensure zoom anchor remains sample-accurate.

### Phase 4: Processed Variants
- Add processed waveform pyramid generation from original cache.
- Wire normalize/amplify/etc. toggles to variant switching.
- Avoid recompute when toggles/settings unchanged.

### Phase 5: Hardening
- Add stress tests (long files, many clips, rapid zoom/pan).
- Tune memory budget and LRU eviction policy.
- Add startup prewarm for currently selected/visible clips.

## Preferences/UI Changes
- Replace `Audio Envelope Granularity (px)` with:
  - `Waveform Base Window (samples)` (e.g., 128–4096)
- Optional advanced settings:
  - max waveform cache memory
  - background decode concurrency
  - processed variant cache limit

## Performance Targets
- Timeline redraw should not trigger decode work.
- Zoom/pan should remain UI-thread cheap (slice query + draw only).
- First waveform availability under 500ms for short clips (best effort).
- No audio glitch on close due to waveform tasks (ensure cooperative shutdown).

## Testing Plan

### Unit Tests
- Pyramid build correctness from known synthetic sample buffers.
- LOD selection correctness for varying zoom/pixel widths.
- Slice extraction bounds and edge behavior.
- Variant key stability and cache hit/miss logic.

### Integration Tests
- Timeline waveform visible and stable after project reload.
- Preview waveform changes with dynamics settings.
- Cursor-centered zoom invariance (sample under cursor remains stable).
- Memory eviction behavior under many large media files.

### Regression Tests
- Existing preview geometry tests continue to pass.
- Startup time regression checks with waveform cache cold/warm states.

## Risks and Mitigations
- Risk: memory growth from multi-LOD + processed variants.
  - Mitigation: strict LRU + global memory caps + per-variant limits.
- Risk: decode CPU spikes on large projects.
  - Mitigation: throttled worker pool + prioritize visible/selected clips first.
- Risk: synchronization bugs between async decode and UI reads.
  - Mitigation: immutable snapshots + atomically swapped shared pointers.

## Deliverables
- `WaveformService` implementation and tests.
- Timeline waveform fully sample-derived.
- Preview waveform migrated to shared service.
- Preferences updated to sample-based analysis control.
- Documentation for cache design and troubleshooting.

## Rollout Strategy
- Land behind a feature flag first (`waveform_service_v2`).
- Enable in internal/dev builds, then default-on after validation.
- Keep fallback path one release cycle, then remove legacy code.
