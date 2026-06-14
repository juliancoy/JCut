#!/usr/bin/env bash
# Phase 4, Task 5 — CI guards for forbidden patterns and file-size caps.
#
# Fails (exit 1) if:
#   1. Any first-party .cpp or .h file exceeds 1500 lines (hard cap).
#   2. Any new silent OpenGL/CPU-upload fallback appears on the visible
#      direct-Vulkan path (matched by fallback-counter key spellings).
#   3. Any file contains a forbidden pattern from §A5.
#
# Warnings (printed, non-fatal) for files >1200 lines.
#
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

first_party_filter='^(build|build-|external|ffbuild|ffmpeg|ffmpeg-build|ffmpeg-install|rtaudio|third_party|nv-codec-headers|CMakeFiles|Testing|__pycache__|\.git|\.deps|loiacono)/'

exit_code=0

# ---------------------------------------------------------------------------
# 1. File-size cap enforcement
# ---------------------------------------------------------------------------
echo "=== File-size cap check (hard: >1500 fail, warn: >1200) ==="
oversized_files=""
warning_files=""

while IFS= read -r file; do
    # Skip files matching the first-party filter
    if echo "$file" | grep -qE "$first_party_filter"; then
        continue
    fi
    # Only check .cpp and .h files
    case "$file" in
        *.cpp|*.h|*.hpp) ;;
        *) continue ;;
    esac
    line_count=$(wc -l < "$file" 2>/dev/null || echo 0)
    if [ "$line_count" -gt 1500 ]; then
        oversized_files="$oversized_files  $file ($line_count lines)\n"
    elif [ "$line_count" -gt 1200 ]; then
        warning_files="$warning_files  $file ($line_count lines)\n"
    fi
done < <(git ls-files 2>/dev/null || find . -type f -name '*.cpp' -o -name '*.h' -o -name '*.hpp')

if [ -n "$oversized_files" ]; then
    echo "FAIL: Files exceeding 1500-line hard cap:"
    printf "$oversized_files"
    exit_code=1
else
    echo "  No files exceed 1500 lines."
fi

if [ -n "$warning_files" ]; then
    echo "WARNING: Files exceeding 1200-line soft cap (consider splitting):"
    printf "$warning_files"
fi

# ---------------------------------------------------------------------------
# 2. Forbidden fallback patterns on the visible direct-Vulkan path
# ---------------------------------------------------------------------------
echo ""
echo "=== Forbidden fallback pattern scan ==="

# These patterns must NOT appear in the direct-Vulkan visible path files.
# They represent silent CPU-upload or OpenGL fallbacks that bypass the
# strict-payload contract.
forbidden_patterns=(
    # CPU image upload on the visible path (must use GPU handoff)
    'cpu_upload_fallback_enabled=true'
    # OpenGL present on the direct-Vulkan path
    'opengl_preview_window_pipeline'
    # Silent fallback to QImage on the visible path
    'hasCpuImage.*&&.*true'
    # New fallback counter spellings that bypass the unified naming
    'fallback_draw_count[^_]'
)

for pattern in "${forbidden_patterns[@]}"; do
    matches=$(grep -rn "$pattern" --include='*.cpp' --include='*.h' \
        direct_vulkan_preview_window.cpp \
        direct_vulkan_preview_presenter.cpp \
        direct_vulkan_preview_presenter.h \
        vulkan_preview_surface.cpp \
        vulkan_preview_surface.h \
        2>/dev/null || true)
    if [ -n "$matches" ]; then
        echo "FAIL: Found forbidden pattern '$pattern':"
        echo "$matches"
        exit_code=1
    fi
done

# Also check for any NEW files that introduce silent fallbacks
# by scanning for the counter key names in files not in the known allowlist
new_fallback_usages=$(grep -rn 'clear_fallback_draw_count\|fallback_draw_count' \
    --include='*.cpp' --include='*.h' \
    --exclude-dir=build \
    --exclude-dir=external \
    --exclude-dir=ffmpeg \
    --exclude-dir=ffmpeg-install \
    --exclude-dir=.git \
    2>/dev/null | grep -v \
    -e 'direct_vulkan_preview_presenter.cpp' \
    -e 'direct_vulkan_preview_window.cpp' \
    -e 'vulkan_preview_surface.cpp' \
    -e 'vulkan_preview_surface_profiling.cpp' \
    || true)

if [ -n "$new_fallback_usages" ]; then
    echo "WARNING: New fallback counter usages outside known files (review needed):"
    echo "$new_fallback_usages"
fi

# ---------------------------------------------------------------------------
# 3. Forbidden moves from §A5 — check for specific anti-patterns
# ---------------------------------------------------------------------------
echo ""
echo "=== Forbidden anti-pattern scan ==="

# A5.1: Present older/batch/nearest frame as completed visible request
# Check for patterns that would bypass the exact-match requirement
batch_frame_completion=$(grep -rn 'completed.*visible.*request\|visible.*completed.*batch\|nearest.*frame.*visible' \
    --include='*.cpp' --include='*.h' \
    --exclude-dir=build \
    --exclude-dir=external \
    --exclude-dir=ffmpeg \
    --exclude-dir=ffmpeg-install \
    --exclude-dir=.git \
    --exclude-dir=ffbuild \
    2>/dev/null | grep -v '//.*TODO\|//.*FIXME\|//.*HACK' || true)

if [ -n "$batch_frame_completion" ]; then
    echo "WARNING: Possible batch-frame completion pattern found (review needed):"
    echo "$batch_frame_completion"
fi

# A5.3: Widen stale tolerance or retention window
stale_tolerance_changes=$(grep -rn 'kStaleTolerance\|stale_tolerance\|kObsoleteVisibleFrameSlack\|effectiveVisibleDecodeKeepWindow' \
    --include='*.cpp' --include='*.h' \
    --exclude-dir=build \
    --exclude-dir=external \
    --exclude-dir=ffmpeg \
    --exclude-dir=ffmpeg-install \
    --exclude-dir=.git \
    --exclude-dir=ffbuild \
    2>/dev/null | grep -v 'constexpr\|const int\|#define' || true)

if [ -n "$stale_tolerance_changes" ]; then
    echo "WARNING: Stale tolerance / retention window tunables found (review needed):"
    echo "$stale_tolerance_changes"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
if [ "$exit_code" -eq 0 ]; then
    echo "=== All CI pattern guards passed ==="
else
    echo "=== CI pattern guards FAILED ==="
fi

exit "$exit_code"
