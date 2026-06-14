# Phase 4 — CI Fix and Regression Coverage

## 1. What was broken

Per Phase 0 DIAGNOSIS.md §2, CI had **never been green** in its current form. Two independent
causes killed every run:

1. **Qt package XML issue** (`jurplel/install-qt-action@v3`): The action passes
   `--modules qtbase qtdeclarative` to `aqtinstall` for Qt 6.4.2, which triggers
   `"packages were not found while parsing XML"`. The jobs died before configure.

2. **Missing libcurl dev headers**: `CMakeLists.txt:29` calls `find_package(CURL REQUIRED)`,
   but `libcurl4-openssl-dev` was never installed. The ASAN and static-analysis jobs got
   further than build-linux/build-macos but died at configure.

Additionally, the macOS job built but ran **zero tests**, and the workflow hand-listed 5
test executables + one `ctest -L temporal` call instead of using tiered ctest labels.

## 2. Changes applied

### 2a. `.github/workflows/ci.yml` — complete rewrite

| Change | Rationale |
|--------|-----------|
| Replaced `jurplel/install-qt-action@v3` with direct `pip install aqtinstall` + `aqt install-qt` | Avoids the XML-parsing bug in the action wrapper |
| Added `libcurl4-openssl-dev`, `libfontconfig-dev`, `libfreetype-dev`, `libglfw3-dev`, `meson`, `ninja-build` to apt install | Fixes `find_package(CURL REQUIRED)` and other missing deps |
| Added `--ffmpeg-only` bootstrap step before CMake configure | Ensures bundled FFmpeg is built and pkg-config paths are correct |
| Restructured into tiered jobs: `test-logic-linux`, `test-temporal-linux`, `test-gpu-soft-linux` | Each job runs `ctest -L <tier>` instead of hand-listed executables |
| Added `forbidden-patterns` job running `scripts/ci_forbidden_patterns.sh` | Enforces file-size caps and forbidden anti-patterns |
| macOS job now runs `ctest -L logic` (excluding decode/gpu-soft/gpu-hw/temporal) | macOS is no longer build-only |
| ASAN job widened to run the full logic tier (not just 2 tests) | Better sanitizer coverage |
| Added `VK_ICD_FILENAMES` env var for lavapipe in gpu-soft job | Ensures software Vulkan ICD is selected |
| Added `submodules: recursive` to checkout step | Ensures ncnn, rtaudio, etc. are available |

### 2b. `tests/CMakeLists.txt` — tier labels

Added a `_jcut_assign_tier_labels()` function that classifies every test target into one or
more of: `logic`, `decode`, `gpu-soft`, `gpu-hw`. All standalone tests (cmake script tests,
Python harnesses, imgui tests) also get explicit LABELS properties.

Tier classification rules:
- **logic**: Every test gets this by default. No GPU, no real decode required.
- **decode**: Tests that exercise FFmpeg software decode (async_decoder, timeline_cache,
  waveform_service, audio_*, render_contract, etc.)
- **gpu-soft**: Tests that require Vulkan (runs via lavapipe offscreen). Includes
  vulkan_direct_render_parity, vulkan_text_generation, handoff_pipeline_contract, etc.
- **gpu-hw**: Tests that need real GPU hardware (NVDEC/CUDA interop, live probes).
  Currently only `test_no_proxy_hardware_playback_contract` and `test_live_playback_lag_probe`.

### 2c. `scripts/ci_forbidden_patterns.sh` — new file

Enforces three categories of CI guard:

1. **File-size cap**: Any first-party `.cpp`/`.h` >1500 lines → FAIL; >1200 lines → WARN.
2. **Forbidden fallback patterns**: Scans the direct-Vulkan visible path files for silent
   CPU-upload or OpenGL fallbacks that bypass the strict-payload contract.
3. **Forbidden anti-patterns** (§A5): Scans for batch-frame completion patterns, stale
   tolerance widening, and other forbidden moves.

## 3. Remaining work (not done in this pass)

- **Task 3 — Missing invariant tests**: The §A4 matrix lists 6 rows marked **ADD** that
  require new C++ test files. These are:
  - `visibleRequestNeverCompletesViaOlderBatchFrame` — decode never reports older batch
    frame as completed visible request
  - `strictPayloadRequirementRejectsCpuFrames` — extend to the surface layer
  - `noClearFallbackDrawDuringSteadyPlayback` — offscreen presenter probe
  - `boundedContinuityReads` — RSS stability check on UI path
  - `exportAt60FpsFrom30FpsRange` — produces 2x output samples
  - `currentVisibleWindowNotEvictedBeforePresentation` — retention policy check

  These require new test files and are deferred to a follow-up pass.

- **Task 5 — Extend `scan_redundant_code.sh`**: The existing script already runs jscpd
  clone detection and targeted seam search. The new `ci_forbidden_patterns.sh` covers the
  CI guard role. The two scripts could be merged in a future pass.

## 4. Exit Gate 4 status

| Criterion | Status |
|-----------|--------|
| Every invariant in §A4 has a named test row | **Partial** — 6 ADD rows remain |
| `ctest` green locally for all runnable tiers | **Not verified** — needs CI run |
| GitHub workflow runs logic+decode+gpu-soft tiers on push/PR | **Done** — workflow structured |
| CI fails red on any failure | **Done** — no `|| true` escapes |
| macOS CI job runs the logic tier | **Done** |
| Forbidden-pattern + file-size guards wired in and green | **Done** — `ci_forbidden_patterns.sh` |
