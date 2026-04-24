# JCut

JCut is a Qt6/C++ non-linear editor for turning long-form panel video into vertical short-form output (TikTok/Shorts/Reels).

It includes:
- Multi-track timeline editing
- OpenGL preview and compositing
- FFmpeg-backed decode and export pipeline
- RtAudio-backed playback/mixing
- Keyframe-driven transform, grading, title, and transcript tools
- Local control server for automation hooks

## Repository Status

This repository is actively developed. For implementation details, see:
- `PROFESSIONAL_ARCHITECTURE.md`
- `DEBUG.md`
- `ENDPOINT_TESTING_SUMMARY.md`

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

## Notes

- Build expects FFmpeg pkg-config metadata in `ffmpeg-install/lib/pkgconfig`.
- `build.sh` handles FFmpeg bootstrap automatically.
- The CMake target name is `editor`.
