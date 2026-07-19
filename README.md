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

- Linux (primary tested environment) or macOS (target platform via MoltenVK; `build.sh` macOS support is in progress, see `ambitious_plan.md` Phase 0)
- CMake 3.24+
- C++20 compiler
- Qt6 development packages:
  - Core
  - Gui
  - Widgets
  - Network with SSL/OpenSSL TLS backend support
  - Concurrent
  - OpenGLWidgets
  - Test
- Vulkan SDK (headers and loader; the build requires it via `find_package(Vulkan REQUIRED)`; MoltenVK on macOS)
- OpenGL development headers (present-state: required by the OpenGL compatibility preview backend, which is scheduled for removal)
- `pkg-config`
- `git` (submodules are required)
- `meson` and `ninja` for the vendored Rubber Band audio time-stretch backend

## Clone

```bash
git clone https://github.com/juliancoy/JCut.git
cd JCut
git submodule update --init --recursive
```

## Build

The canonical build entrypoint is `build.sh`. It bootstraps pinned FFmpeg libs into `ffmpeg-install/`, builds the vendored Rubber Band audio time-stretch library into `.deps/rubberband-install/` when Meson is available, and builds the editor.

`build.sh` presently targets Linux. macOS (MoltenVK) is a target platform; `build.sh` macOS support is in progress per `ambitious_plan.md` Phase 0.

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
./build/jcut
```

If your shell environment injects conflicting snap libraries, use:

```bash
./run_editor.sh
```

## Linux distribution

Create a relocatable AppDir and compressed distribution archive with:

```bash
./scripts/package_linux.sh
```

The outputs are written to `dist/`:

- `JCut-<architecture>.AppDir/` is the unpacked application bundle.
- `JCut-<architecture>.tar.gz` is the portable archive for distribution.
- `JCut-<architecture>.AppImage` is also produced when `appimagetool` is installed.

The packaging script invokes `./build.sh`, bundles the Qt runtime and plugins,
copies the compiled Vulkan shaders, collects non-system shared-library
dependencies, and rewrites runtime search paths to be relative to the bundle.
The target system must still provide a compatible Linux kernel, glibc, graphics
driver, Vulkan loader, and audio/display services. For widest compatibility,
produce release artifacts on the oldest Linux distribution supported by JCut.

For packaging-only iteration after a successful build, use:

```bash
./scripts/package_linux.sh --skip-build
```

## Tests

If built with `--with-tests`, run:

```bash
ctest --test-dir build --output-on-failure
```

## Vulkan FaceDetections Harness

`jcut_vulkan_facedetections_offscreen` runs the native Vulkan FaceDetections detector outside the editor. By default it scans the full video at full frame rate; use `--max-frames` and `--stride` for bounded tests.

```bash
./build/jcut_vulkan_facedetections_offscreen input.mp4 \
  --full-video \
  --params-file /tmp/jcut_runtime_params.json \
  --out-dir /tmp/jcut_facedetections
```

For the same detector preflight UI used by the editor, start the standalone runner with `--preflight`:

```bash
./build/jcut_vulkan_facedetections_offscreen input.mp4 --preflight --out-dir /tmp/jcut_facedetections
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
- `preview_*`: preview rendering and interaction. The preview is the direct-Vulkan path (`DirectVulkanPreview*` / `VulkanPreviewSurface`); the OpenGL backend is present-state compatibility scheduled for removal once the parity gates in `OPENGL_DEPRECATION_AND_REMOVAL_PLAN.md` pass
- `render_*`: offline export and render pipeline
- `decoder_*`, `async_decoder*`: media decoding pipeline
- `control_server*`: local HTTP control server
- `rtaudio/`: audio backend submodule
- `ffmpeg/`: FFmpeg source submodule
- `external/rubberband/`: high-quality audio time-stretch submodule
- `tests/`, `Testing/`: test sources and test artifacts
