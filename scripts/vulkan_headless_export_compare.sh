#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/jcut"
STATE="${1:-${ROOT_DIR}/testbench_state.json}"
OUT_DIR="${2:-${ROOT_DIR}/testbench_assets/headless_export_compare_$(date +%Y%m%d_%H%M%S)}"
CLIP_ID="${CLIP_ID:-clip_video_01}"
SPEAKER_ID="${SPEAKER_ID:-spk1}"

mkdir -p "${OUT_DIR}"

run_case() {
  local backend="$1"
  local out_mp4="${OUT_DIR}/${backend}.mp4"
  local log="${OUT_DIR}/${backend}.log"
  local timing="${OUT_DIR}/${backend}_time.txt"

  /usr/bin/time -f "elapsed_sec=%e" -o "${timing}" \
    env QT_QPA_PLATFORM=offscreen JCUT_RENDER_BACKEND="${backend}" \
      "${BIN}" --speaker-export-harness \
      --state "${STATE}" \
      --output "${out_mp4}" \
      --format mp4 \
      --clip-id "${CLIP_ID}" \
      --speaker-id "${SPEAKER_ID}" \
      > "${log}" 2>&1
}

run_case opengl
run_case vulkan

{
  echo "state_file: ${STATE}"
  echo "clip_id: ${CLIP_ID}"
  echo "speaker_id: ${SPEAKER_ID}"
  echo
  echo "== outputs =="
  ls -lh "${OUT_DIR}/opengl.mp4" "${OUT_DIR}/vulkan.mp4"
  echo
  echo "== hashes =="
  sha256sum "${OUT_DIR}/opengl.mp4" "${OUT_DIR}/vulkan.mp4"
  if cmp -s "${OUT_DIR}/opengl.mp4" "${OUT_DIR}/vulkan.mp4"; then
    echo "binary_identity: identical"
  else
    echo "binary_identity: different"
  fi
  echo
  echo "== timing =="
  cat "${OUT_DIR}/opengl_time.txt"
  cat "${OUT_DIR}/vulkan_time.txt"
  echo
  echo "== backend logs =="
  rg -n "render-backend-fallback|render-backend-selection|requested=|effective=|Harness start|progress frame=|Render completed" "${OUT_DIR}/opengl.log" "${OUT_DIR}/vulkan.log" || true
} | tee "${OUT_DIR}/summary.txt"

echo "Wrote artifacts to: ${OUT_DIR}"
