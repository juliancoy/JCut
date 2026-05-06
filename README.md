# JCut

JCut is a Qt6/C++20 non-linear editor for turning long-form panel video into vertical short-form output (TikTok/Shorts/Reels), with timeline editing, GPU preview compositing, transcript tooling, and offline render/export.

## Docs

- [Architecture](ARCHITECTURE.md)
- [Features](FEATURES.md)
- [Tabs](TABS.md)
- [Transcript Logic](TRANSCRIPT_LOGIC.md)
- Mermaid sources:
  - [System Architecture](docs/diagrams/system-architecture.mmd)
  - [Inspector Tabs](docs/diagrams/inspector-tabs.mmd)
  - [Playback Data Flow](docs/diagrams/playback-data-flow.mmd)

## Prerequisites

- Linux (primary tested environment)
- CMake 3.24+
- C++20 compiler
- Qt6 development packages:
  - Core
  - Gui
  - Widgets
  - Network
  - Concurrent
  - OpenGLWidgets
  - Test
- OpenGL development headers
- `pkg-config`
- `git` (submodules are required)

## Clone

```bash
git clone https://github.com/juliancoy/JCut.git
cd JCut
git submodule update --init --recursive
```

## Build

The canonical build entrypoint is `build.sh`. It bootstraps pinned FFmpeg libs into `ffmpeg-install/` and builds the editor.

```bash
./build.sh
```

Useful build flags:

```bash
./build.sh --asan                 # AddressSanitizer debug build
./build.sh --with-tests           # Build tests
./build.sh --ffmpeg-enable-nvidia # Try NVIDIA-enabled FFmpeg profile
./build.sh --run                  # Build and run editor
```

## Run

```bash
./build/editor
```

If your shell environment injects conflicting snap libraries, use:

```bash
./run_editor.sh
```

## Tests

If built with `--with-tests`, run:

```bash
ctest --test-dir build --output-on-failure
```

## Vulkan FaceStream Harness

`jcut_vulkan_boxstream_offscreen` runs the native Vulkan FaceStream detector outside the editor. By default it scans the full video at full frame rate; use `--max-frames` and `--stride` for bounded tests.

```bash
./build/jcut_vulkan_boxstream_offscreen input.mp4 \
  --full-video \
  --params-file /tmp/jcut_runtime_params.json \
  --out-dir /tmp/jcut_facestream
```

Runtime tuning can be changed while the process is running by editing the JSON file passed with `--params-file`:

```json
{
  "threshold": 0.62,
  "stride": 1,
  "max_detections": 128,
  "max_faces_per_frame": 4,
  "roi_x1": 0.18,
  "roi_y1": 0.42,
  "roi_x2": 0.92,
  "roi_y2": 0.98,
  "min_face_area_ratio": 0.002,
  "max_face_area_ratio": 0.09,
  "min_aspect": 0.6,
  "max_aspect": 1.7
}
```

Preview output is useful for inspection but slower because it requires GPU readback. Use `--preview-stride N` to throttle preview updates; keep `--no-preview-window --no-preview-files` for throughput profiling.

## Project Structure

- `editor*.cpp/.h`: main editor window and UI orchestration
- `timeline_*`: timeline widget, model, layout, input, cache
- `preview_*`: OpenGL preview rendering and interaction
- `render_*`: offline export and render pipeline
- `decoder_*`, `async_decoder*`: media decoding pipeline
- `control_server*`: local HTTP control server
- `rtaudio/`: audio backend submodule
- `ffmpeg/`: FFmpeg source submodule
- `tests/`, `Testing/`: test sources and test artifacts
