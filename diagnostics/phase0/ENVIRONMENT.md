# Phase 0 — Build Environment (macOS)

> Working notes; finalized when the build is green. Per ambitious_plan.md D1/D2: this Mac is the
> primary dev machine; the RTX 3060 Linux box is deferred.

## Machine

- Mac mini, Apple M1, 8 cores, 8 GB RAM (`Agentic-Mac-mini`), macOS (Darwin 25.2.0, arm64)
- GUI session available (Aqua, console user `natan`) — windowed repro possible
- Checkout: `/Users/natan/Documents/JCut`, branch `main`

## Toolchain (verified 2026-06-11)

| Tool | Version / source |
|---|---|
| Apple clang | 21.0.0 (CommandLineTools) |
| cmake | 4.3.3 (brew) — note: hard-errors on `cmake_minimum_required < 3.5` in subprojects; escape hatch `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` |
| ninja | 1.13.2 (brew) |
| meson | 1.11.1 (brew) — for vendored Rubber Band |
| nasm | brew (FFmpeg asm; arm64 uses NEON so largely moot) |
| Qt | **6.11.1** (brew, framework layout) — pinned Linux box used 6.4.2; private qrhi headers verified present at `lib/QtGui.framework/Versions/A/Headers/6.11.1/QtGui/private/` (`qrhi_p.h`, `qrhivulkan_p.h`, `qrhigles2_p.h`) |
| Vulkan | brew `vulkan-headers` + `vulkan-loader` + `molten-vk` (MoltenVK ICD) |
| glfw, freetype, fontconfig | brew (FONT_RENDER + GLFW pkg-config deps satisfied) |
| ffmpeg (CLI) | brew — **for test-asset generation only**; the editor links the pinned submodule build at `ffmpeg-install/` |

## Source changes for macOS (kept Linux behavior intact)

- `build.sh`: `build_jobs()` (nproc/sysctl), Darwin FFmpeg profile = `safe` (no /proc NVIDIA
  probe; NEON asm kept), skip apt-based Qt private-header download on Darwin (brew Qt ships
  them), `rg`→`grep -Fq`, `CMAKE_PREFIX_PATH=$(brew --prefix)` on Darwin, `LD_PRELOAD` guarded
  Linux-only, `--run` resolves the `.app` bundle binary path.
- `CMakeLists.txt`: `LINKER:--disable-new-dtags` and `LINKER:--no-as-needed` + `libswresample.so`
  guarded `NOT APPLE` (Apple ld64 rejects GNU dtags/as-needed; `.dylib` on Darwin) — two tool
  targets (`jcut_vulkan_zero_copy_video_preprocess`, `jcut_vulkan_facedetections_offscreen`).

## Known asset gap

`projects/testbench/state.json` references `/home/julian/Documents/JCut/testbench_assets/`
(pseudorandom_source.mp4, life_c.png, gradient_a.png, fractal_b.png, a transcript JSON) — not in
the repo, no generator script exists. Plan: synthesize with the brew ffmpeg CLI (lavfi) and remap
paths (investigate the `.mediaRoot` remap mechanism first).

## Build invocation

```bash
git submodule update --init --recursive
./build.sh --with-tests
```

## Result (2026-06-11, first macOS build)

**Green.** FFmpeg (`safe` profile) + Rubber Band bootstrapped and the full editor + 45-test
suite compiled with **zero C++ source changes** — Qt 6.4→6.11 private-API drift did not
materialize. `glslang` (brew) is required for the SPIR-V shader step (warned-missing on first
configure). Binary: `build/jcut.app/Contents/MacOS/jcut` (a `build/editor` symlink unblocks the
Python harnesses pending Phase 5 naming unification). Submodule pinning needed one manual step:
the ffmpeg submodule clone came up HEAD-less and nv-codec-headers diverged to a branch tip —
fixed by explicit `git fetch origin <recorded-sha> && git checkout` in each.

Runtime session facts: control server on 40130; `JCUT_PROJECT_ROOT=<repo>` required on macOS
(see DIAGNOSIS.md F-C); system audio output must be a 2ch/48k device (was a 1ch/16k Bluetooth
headset — RtAudio openStream fails; switched via `SwitchAudioSource -s "DELL S2721H"`);
`caffeinate -dimsu` keeps the display awake for windowed runs. Test baseline and the five
bring-up findings: see `TEST_BASELINE.md` and `DIAGNOSIS.md`.
