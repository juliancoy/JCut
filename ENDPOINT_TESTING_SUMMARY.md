# Control Server Endpoint Testing Summary

## Overview
This document summarizes the analysis of the control_server.cpp endpoints and the decode pathway.

## Endpoints Available

### Health & Status
- `GET /health` - System health status (PID, port, uptime)
- `GET /version` - Build information (build time, git commit, dirty flag)
- `GET /playhead` - Current playback position

### Editor State
- `GET /state` - Editor state snapshot
- `GET /timeline` - Timeline information (currentFrame, selectedClipId, tracks)
- `GET /tracks` - Track information
- `GET /clips` - Clips with filtering (id, label_contains, file_contains, trackIndex)
- `GET /clip` - Single clip by ID
- `GET /keyframes` - Keyframes for a clip (type: grading/opacity/title/transform)

### Project & History
- `GET /project` - Project information
- `GET /history` - History snapshot

### Performance & Diagnostics
- `GET /profile` - Performance profiling data
- `GET /profile/cached` - Cached profile snapshot
- `GET /diag/perf` - Performance diagnostics (all cache states, debug controls)
- `GET /debug` - Debug controls snapshot

### Decode Benchmarks
- `GET /decode/rates` - Decode FPS benchmark for all media in project
- `GET /decode/seeks` - Seek time benchmark for all media in project

### Hardware Information
- `GET /hardware` - Hardware information (CPU, memory, GPU, OpenGL, CUDA, VA-API)

### Rendering
- `GET /render/status` - GPU rendering status (usedGpu, path, encoder)

### UI Interaction
- `GET /ui` - UI hierarchy
- `GET /windows` - Top-level windows
- `GET /screenshot` - Screenshot (rate-limited)
- `GET /menu` - Active popup menu
- `POST /playhead` - Set playhead position
- `POST /click` - Send synthetic click
- `POST /click-item` - Click widget by ID
- `POST /menu` - Trigger menu action
- `POST /debug` - Set debug controls
- `POST /profile/reset` - Reset profiling stats

## Hardware Endpoint Details

The `/hardware` endpoint (lines 1463-1591 in control_server.cpp) returns:

1. **CPU Info** (via `lscpu`):
   - cpu_model, cpu_cores, cpu_threads, cpu_mhz

2. **Memory Info** (via `free -h`):
   - memory_total, memory_used, memory_free

3. **GPU Info**:
   - NVIDIA GPU (via `nvidia-smi`): nvidia_gpu, nvidia_memory, nvidia_utilization
   - AMD GPU detection (via `lspci`): amd_gpu_detected

4. **OpenGL Info** (via `glxinfo`):
   - opengl_vendor, opengl_renderer, opengl_version

5. **Codec Support**:
   - CUDA version (via `nvcc --version`)
   - VA-API availability (via `vainfo`)

## Decode Pathway Analysis

### 1. Path Selection (editor_shared.cpp:1695-1718)

```
interactivePreviewMediaPathForClip(clip):
  1. Check for proxy path (playbackProxyPathForClip)
  2. If proxy exists and allowed, return proxy path
  3. Otherwise, use original file path
  4. Validate: disallow alpha ProRes .mov files
```

### 2. Decoder Initialization (decoder_context.cpp)

The `DecoderContext` class:
- Opens media file via FFmpeg
- Initializes codec context
- Attempts hardware acceleration

### 3. Hardware Acceleration (decoder_context.cpp:405-480)

Priority order by platform:
- **macOS**: VideoToolbox
- **Windows**: D3D11VA, DXVA2
- **Linux/通用**: CUDA, VAAPI

For VAAPI on Linux:
- Checks render nodes: /dev/dri/renderD128, /dev/dri/renderD129, /dev/dri/renderD130

Shared hardware devices:
- AsyncDecoder pre-creates hardware device contexts
- DecoderContext borrows references to avoid CUDA OOM errors

### 4. Benchmark Endpoints

Both `/decode/rates` and `/decode/seeks` use:
- `interactivePreviewMediaPathForClip()` to get decode path
- `DecoderContext` to decode frames
- Report `hardware_accelerated` flag from `ctx.isHardwareAccelerated()`

## Testing Status

**Cannot test endpoints** - The editor binary requires FFmpeg libraries (libavcodec.so.60) which are not available in the current environment.

```
./editor: error while loading shared libraries: libavcodec.so.60: cannot open shared object file: No such file or directory
```

### Root Cause: FFmpeg Version Mismatch

The system has FFmpeg 4.4.2 installed (libavcodec.so.58), but the editor was compiled against FFmpeg 6.x (libavcodec.so.60).

**Installed FFmpeg versions:**
- libavcodec58 (FFmpeg 4.4.2)
- libavformat58
- libavutil56

**Editor expects:**
- libavcodec.so.60
- libavformat.so.60
- libavutil.so.58

**Solution options:**
1. Install FFmpeg 6.x on this system (from https://ffmpeg.org/download.html or a PPA)
2. Rebuild the editor against the current FFmpeg 4.4 libraries

## Decode Pathway Verification

The decode pathway makes sense:

1. **Path Resolution**: Uses proxy if available, otherwise original file
2. **Validation**: Blocks problematic formats (alpha ProRes MOV)
3. **Hardware Selection**: Platform-appropriate priority order
4. **Memory Management**: Shared device contexts prevent OOM
5. **Benchmark Integration**: Both benchmark endpoints use the same path resolution

## Code Quality Observations

1. **Well-structured**: Clear separation between path selection, decoding, and benchmarking
2. **Hardware support**: Comprehensive platform support (macOS, Windows, Linux)
3. **Memory efficiency**: Shared hardware device contexts is a good pattern
4. **Error handling**: Proper fallback for missing hardware
5. **Benchmarking**: Real-world performance measurement via actual decode

## Recommendations

1. Install FFmpeg libraries to enable runtime testing
2. Consider adding endpoint to query current hardware acceleration status for a specific clip
3. The `/hardware` endpoint could be enhanced with actual decode capability detection per codec