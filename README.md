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
