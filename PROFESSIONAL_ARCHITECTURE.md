# PanelVid2TikTok — Editor Architecture

> **Status**: Living document reflecting the actual implementation as of 2026-04.
> Future improvement notes are marked with `🔮`.

---

## Overview

A Qt6-based non-linear video editor optimised for vertical-format (TikTok/Shorts/Reels) content creation from longer "panel" source videos.
The editor is a single-binary C++ application built with CMake/Ninja.
Fixed output resolution is **1080×1920 @ 30 fps**.

---

## System Layers

```
┌──────────────────────────────────── UI Layer ─────────────────────────────────────────┐
│   ExplorerPane          TimelineWidget        PreviewWindow      InspectorPane         │
│   (file browser,        (multi-track          (OpenGL playback   (Inspector tabs per   │
│    proxy tools)          QWidget-based)        + compositing)     clip type)           │
└──────────────────────────────────────────────────────────────────────────────────────-┘
                │                  │                  │                │
                └──────────────────┴──────────────────┴────────────────┘
                                          │
┌──────────────────────────────── Application Layer ────────────────────────────────────┐
│   EditorWindow (editor.h / editor_*.cpp)                                              │
│   ├─ ProjectManager / ProjectState   (project CRUD, multi-project switching)          │
│   ├─ History (undo-redo via JSON snapshots, editor_history.json)                      │
│   ├─ ControlServer  (local HTTP/REST API on 127.0.0.1:<port>)                         │
│   └─ PlaybackController  (frame clock, sample-accurate A/V sync)                     │
└───────────────────────────────────────────────────────────────────────────────────────┘
                                          │
┌──────────────────────────────── Pipeline Layer ───────────────────────────────────────┐
│   AsyncDecoder          TimelineCache            MemoryBudget                         │
│   (thread-pool          (predictive per-clip      (CPU/GPU budget                     │
│    FFmpeg decode)        LRU cache)                tracking & eviction)               │
│                                                                                       │
│   PlaybackFramePipeline (sample-accurate playback pipeline, ties decoder↔audio)      │
└───────────────────────────────────────────────────────────────────────────────────────┘
                                          │
┌──────────────────────────────── Engine Layer ─────────────────────────────────────────┐
│   DecoderContext / FFmpeg   GlFrameTextureShared   AudioEngine (RtAudio)              │
│   (libavcodec, libavformat  (OpenGL texture        (multi-track mix, speech-          │
│    libswscale, CUDA/VAAPI)   upload/eviction)       filter, transcript overlay)       │
│                                                                                       │
│   Render Pipeline (render_*.cpp)                                                      │
│   (GPU-composited or CPU-fallback offline export)                                     │
└───────────────────────────────────────────────────────────────────────────────────────┘
```

---

## Key Components

### 1. Data Model (`editor_shared.h`)

The central data structure is `TimelineClip`, owned by the `EditorWindow` and serialised as JSON.

```cpp
struct TimelineClip {
    // Identity & placement
    QString id, filePath, proxyPath, label;
    ClipMediaType mediaType;        // Video | Audio | Image | Title
    MediaSourceKind sourceKind;     // File | ImageSequence
    int64_t startFrame, durationFrames, sourceInFrame;
    int trackIndex;

    // Per-clip grading (flat + keyframes)
    qreal brightness, contrast, saturation, opacity;
    QVector<GradingKeyframe> gradingKeyframes;   // step/linear interp

    // Per-clip transform (flat + keyframes)
    qreal baseTranslationX/Y, baseRotation, baseScaleX/Y;
    QVector<TransformKeyframe> transformKeyframes;

    // Title overlay
    QVector<TitleKeyframe> titleKeyframes;

    // Transcript overlay
    TranscriptOverlaySettings transcriptOverlay;

    // Audio
    int fadeSamples;     // crossfade with previous clip
    qreal playbackRate;
    bool videoEnabled, audioEnabled, locked;

    // Alpha mask feathering
    qreal maskFeather, maskFeatherGamma;
};
```

**Tracks** (`TimelineTrack`) carry name, height, and enable/disable flags.
The entire project state is serialised to `editor_state.json` and history snapshots to `editor_history.json` for undo-redo.

---

### 2. Async Decoder (`async_decoder.h/cpp`, `decoder_context.h/cpp`)

**✅ Fully implemented.**

| Feature | Status |
|---------|--------|
| Per-file lane sharding (hash- & shard-based) | ✅ |
| Priority queue (128-request cap, deadline expiry) | ✅ |
| Hardware acceleration: CUDA → VAAPI → software | ✅ |
| Software ProRes single-threaded workaround (crash fix) | ✅ |
| Image sequence batching + WebP LRU frame cache (384 MB) | ✅ |
| VP9 alpha path via libvpx-vp9 | ✅ |
| `DecodeRequest` generation/cancel-before-frame | ✅ |

**Threading model:**
- 2–6 worker threads (capped at `idealThreadCount`).
- Each `LaneState` holds up to 4 `DecoderContext` objects; LRU eviction when exceeded.
- Callbacks are always `QueuedConnection`-invoked back to the main thread.

🔮 **Future**: GPU zero-copy path (CUDA NV12 → GL texture via `FrameHandle::createHardwareFrame`) is scaffolded but gated behind `EDITOR_HAS_CUDA`. Completing the interop would eliminate the GPU→CPU memcpy during preview.

---

### 3. Frame Handle (`frame_handle.h/cpp`)

**✅ Implemented.**

RAII wrapper with `QExplicitlySharedDataPointer` for thread-safe reference counting.
Supports CPU frames (`QImage`) and GPU frames (CUDA hardware surface).  
`memoryUsage()` is used by `MemoryBudget` for pressure tracking.

---

### 4. Timeline Cache (`timeline_cache.h/cpp`)

**✅ Fully implemented.**

- Per-clip LRU with configurable memory budget.
- Predictive prefetch based on playback direction and speed.
- Lookahead window: 30 frames default.
- Eviction pressure callbacks feed `MemoryBudget`.
- Cache-hit statistics exposed as `cacheHitRate()`.

---

### 5. Memory Budget (`memory_budget.h/cpp`)

**✅ Implemented.**

- Separate CPU and GPU budget tracking via `std::atomic`.
- Priority-aware allocation (`Low` / `Normal` / `High` / `Critical`).
- `trimRequested` signal drives cache eviction callbacks.

🔮 **Future**: GPU budget is tracked conceptually but actual VRAM consumption tracking relies on estimated frame sizes, not direct driver queries. Hooking into `GL_NVX_gpu_memory_info` or similar would improve accuracy.

---

### 6. Preview Window (`preview.h`, `preview_window_*.cpp`)

**✅ Fully implemented.**

`PreviewWindow` is a `QOpenGLWidget`. The rendering stack:
1. `getActiveClips()` filters the clip list to the current timeline frame.
2. Per-clip frames are fetched from `TimelineCache` (or requested via `AsyncDecoder`).
3. `renderFrameLayerGL()` uploads each `FrameHandle` → `GLuint` texture (via `GlFrameTextureShared`).
4. An OpenGL shader composites layers with per-clip transform matrices, grading, opacity, and blend mode.
5. `PlaybackFramePipeline` drives sample-accurate advancement during live playback.
6. Transcript overlay is rendered via `QPainter` on top of the GL output.

Features: zoom/pan, interactive clip move/resize handles, audio badge, empty-state placeholder.

**GPU path:** OpenGL with `QOpenGLShaderProgram` (GLSL). Not using Qt RHI (QRhi) despite what the old doc said — actual implementation uses raw OpenGL.

🔮 **Future**: Migrate to Qt RHI for Vulkan/Metal portability. The `GPUCompositor` class (`gpu_compositor.h`) exists as a QRhi-based skeleton but is not wired into `PreviewWindow`.

---

### 7. Playback Frame Pipeline (`playback_frame_pipeline.h/cpp`)

**✅ Implemented.**

Bridges `AsyncDecoder` and `AudioEngine` for A/V synchronised playback.
Handles sub-frame sample offset timing and `playbackRate` scaling.

---

### 8. Audio Engine (`audio_engine.h`)

**✅ Implemented** (header is comprehensive, implementation is `audio_engine.h` inline / separate TU).

- Built on **RtAudio** (bundled in `rtaudio/`).
- Multi-track mixing with per-clip fade-in/fade-out (`fadeSamples`).
- Speech-filter pass (silence non-speech segments using transcript data).
- Transcript-driven overlay timing.
- Mute/volume control.

---

### 9. Offline Render Pipeline (`render.h`, `render_*.cpp`)

**✅ Fully implemented.**

| Stage | File | Notes |
|-------|------|-------|
| Decode | `render_decode.cpp` | Parallel decode per clip |
| GPU composite | `render_gpu.cpp` | OpenGL offscreen FBO |
| CPU fallback | `render_cpu_fallback.cpp` | `QPainter`-based |
| Audio mix | `render_audio.cpp` | FFmpeg AAC/Opus output |
| Codec selection | `render_codecs.cpp` | H.264/HEVC/ProRes/etc. |
| Export | `render_export.cpp` | Muxing, progress callback |
| Stats | `render_stats.cpp` | Per-stage timing table |

`RenderProgress` provides granular per-stage timing (`renderDecodeStageMs`, `renderCompositeStageMs`, etc.) for profiling.

🔮 **Future**: Hardware encoder support (NVENC/VAAPI encode) — scaffolded with `usingHardwareEncode` flag in `RenderResult`, not yet wired.

---

### 10. Inspector Tabs (Multi-tab Keyframe Editor)

**✅ Implemented.**

| Tab | File | Keyframe Types |
|-----|------|---------------|
| Video Transform | `video_keyframe_tab.cpp` | `TransformKeyframe` |
| Grading | `grading_tab.cpp` | `GradingKeyframe` (brightness, contrast, sat, opacity, shadows/midtones/highlights) |
| Titles | `titles_tab.cpp` | `TitleKeyframe` (text, position, font, opacity) |
| Transcript | `transcript_tab.cpp` | WhisperX JSON word-level editing |
| Effects | `effects_tab.cpp` | Mask feather |
| Output | `output_tab.cpp` | Export settings |
| Profile | `profile_tab.cpp` | Render stats display |

All keyframe tabs share `KeyframeTabBase` (deferred seek, sync-to-playhead, selection suppression) and `TableTabBase` (selection/skip-refresh helpers).
`keyframe_table_shared.h/cpp` provides `restoreSelectionByFrameRole` and `collectSelectedFrameRoles` used by all tabs.

---

### 11. Timeline Widget (`timeline_widget.h`, `timeline_widget_*.cpp`)

**✅ Implemented** (split across core/input/layout/model/paint).

- Multi-track custom `QWidget`.
- Drag-to-move, trim handles, track sidebar.
- Render sync markers.
- Export range segments.

---

### 12. Explorer Pane (`explorer_pane.h/cpp`)

**✅ Implemented.**

File browser with proxy generation tools (image-sequence proxies, MJPEG/H.264 proxies via FFmpeg subprocess), drag-to-timeline, `ai_rest_snapshot.py` integration hook.

---

### 13. Control Server (`control_server.h/cpp`)

**✅ Implemented.**

Local HTTP server on `127.0.0.1:<random port>` for external tool integration (AI scripts, `editor_harness.py`).
Exposes: fast state snapshot, profiling snapshot, window screenshot.

---

## Threading Model

```
Main Thread (Qt event loop)
├─ UI: Timeline, Inspector, Explorer
├─ PreviewWindow paint (OpenGL)
└─ ControlServer dispatch (QueuedConnection from worker thread)

Decoder Thread Pool  [2–6 workers]
└─ Per-lane: DecoderContext (FFmpeg), frame conversion, cache insert

Audio Thread (RtAudio callback)
└─ Real-time multi-track mix, splice A/V sync

Render Thread (offline export only)
└─ Sequential frame decode → composite → encode
```

---

## Data Flow: Frame Request Lifecycle

```
TimelineWidget seeks to frame N
        │
        ▼
PreviewWindow::requestFramesForCurrentPosition()
        │
        ▼
TimelineCache::requestFrame(clipId, localFrame)
  ├─ Cache HIT  → FrameHandle returned synchronously
  └─ Cache MISS → AsyncDecoder::requestFrame(path, sourceFrame, priority, timeoutMs, cb)
                          │
                          ▼ [worker thread]
                  DecoderContext::decodeFrame(frameNumber)
                    ├─ ImageSequence: loadCachedSequenceFrameImage()
                    └─ Video: seekAndDecode() → avcodec_send_packet → avcodec_receive_frame
                                                  → convertToFrame() → QImage (CPU)
                          │
                          ▼ [QueuedConnection back to main thread]
                  TimelineCache stores FrameHandle
                          │
                          ▼
                  PreviewWindow::scheduleRepaint()
                          │
                          ▼
                  paintGL(): textureForFrame() → glTexImage2D → GLSL composite
```

---

## Technology Stack

| Layer | Libraries / Technology |
|-------|------------------------|
| UI | Qt 6, QOpenGLWidget, QWidget |
| Decode | libavcodec, libavformat, libswscale (FFmpeg) |
| HW Accel | CUDA (NVDec), VAAPI (Linux) |
| Audio | RtAudio (bundled), libavcodec (AAC/Opus encode) |
| Compositing | OpenGL 3.3 + GLSL (preview); OpenGL offscreen FBO (render) |
| Serialisation | QJsonDocument (state + history) |
| Build | CMake 3.x, Ninja, Qt 6 MOC |
| Tests | Qt Test framework (`tests/`) |
| Integration | Python (`ai_rest_snapshot.py`, `editor_harness.py`) |

---

## Known Issues & Future Improvements

| Area | Issue / Improvement | Priority |
|------|---------------------|----------|
| 🔮 GPU Interop | CUDA→GL zero-copy path scaffolded but disabled; full path would remove GPU→CPU memcpy | High |
| 🔮 HW Encode | `RenderResult::usedHardwareEncode` flag exists; NVENC/VAAPI encode not wired | Medium |
| 🔮 Qt RHI | `GPUCompositor` (QRhi-based) skeleton exists but PreviewWindow still uses raw OpenGL | Low |
| 🔮 VRAM tracking | GPU memory budget uses size estimates, not driver queries | Low |
| ⚠️ ProRes threading | ProRes software decoder crashes with multi-threading; fixed with `thread_count=1` | Fixed |
| 🔮 OCIO/LUTs | No OCIO integration; grading is custom CPU path | Future |
| 🔮 Audio scrubbing | No pitch-corrected scrub audio | Future |
| 🔮 Effects plugin API | No plugin system; effects are hardcoded (feather mask only) | Future |

---

## Build

```bash
# Install FFmpeg dev libs (Ubuntu/Debian)
sudo apt install libavcodec-dev libavformat-dev libavutil-dev libswscale-dev

# Build (Release)
./build.sh

# Run
./build/editor

# Tests
./build/test_async_decoder
./build/test_timeline_cache
./build/test_integration
./build/test_memory_budget
./build/test_frame_handle
```
