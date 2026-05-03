#!/usr/bin/env bash
set -euo pipefail

# Compares OpenGL vs Vulkan-requested runs on the same JCut instance workflow.
# Note: current code may still fall back from Vulkan to OpenGL; logs capture this.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/jcut"
PORT="${PORT:-40140}"
WAIT_START_SEC="${WAIT_START_SEC:-3}"
WAIT_METRICS_SEC="${WAIT_METRICS_SEC:-8}"
OUT_DIR="${1:-/tmp/jcut_vulkan_compare_$(date +%s)}"

if [[ ! -x "${BIN}" ]]; then
  echo "error: missing executable at ${BIN}" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

run_case() {
  local backend="$1"
  local base="${OUT_DIR}/${backend}"
  local log="${base}.log"
  local health="${base}_health.json"
  local profile="${base}_profile.json"
  local perf="${base}_diag_perf.json"
  local rates="${base}_decode_rates.json"
  local seeks="${base}_decode_seeks.json"
  local shot="${base}.png"

  : > "${log}"
  JCUT_RENDER_BACKEND="${backend}" EDITOR_CONTROL_PORT="${PORT}" "${BIN}" > "${log}" 2>&1 &
  local pid=$!

  for _ in $(seq 1 120); do
    if curl -fsS "http://127.0.0.1:${PORT}/health" >/dev/null 2>&1; then
      break
    fi
    sleep 0.25
  done

  sleep "${WAIT_START_SEC}"
  curl -fsS "http://127.0.0.1:${PORT}/health" > "${health}" || true
  curl -fsS "http://127.0.0.1:${PORT}/profile" > "${profile}" || true

  # Wait for project state cache to include timeline/media before decode benchmarks.
  for _ in $(seq 1 40); do
    state_json="$(curl -fsS "http://127.0.0.1:${PORT}/state" 2>/dev/null || true)"
    timeline_count="$(printf "%s" "${state_json}" | jq -r '.state.timeline_count // 0' 2>/dev/null || echo 0)"
    if [[ "${timeline_count}" =~ ^[0-9]+$ ]] && [[ "${timeline_count}" -gt 0 ]]; then
      break
    fi
    sleep 0.5
  done

  # Screenshot endpoint can temporarily 503 while UI settles; retry briefly.
  for _ in $(seq 1 15); do
    if curl -fsS "http://127.0.0.1:${PORT}/screenshot" -o "${shot}"; then
      break
    fi
    sleep 0.5
  done

  curl -fsS "http://127.0.0.1:${PORT}/decode/rates?refresh=1" >/dev/null || true
  curl -fsS "http://127.0.0.1:${PORT}/decode/seeks?refresh=1" >/dev/null || true
  sleep "${WAIT_METRICS_SEC}"

  curl -fsS "http://127.0.0.1:${PORT}/diag/perf" > "${perf}" || true
  curl -fsS "http://127.0.0.1:${PORT}/decode/rates" > "${rates}" || true
  curl -fsS "http://127.0.0.1:${PORT}/decode/seeks" > "${seeks}" || true

  kill "${pid}" >/dev/null 2>&1 || true
  wait "${pid}" >/dev/null 2>&1 || true
}

run_case "opengl"
run_case "vulkan"

{
  echo "== screenshot hashes =="
  if [[ -f "${OUT_DIR}/opengl.png" && -f "${OUT_DIR}/vulkan.png" ]]; then
    sha256sum "${OUT_DIR}/opengl.png" "${OUT_DIR}/vulkan.png"
    if cmp -s "${OUT_DIR}/opengl.png" "${OUT_DIR}/vulkan.png"; then
      echo "pixel_identity: identical"
    else
      echo "pixel_identity: different"
    fi
  else
    echo "screenshots missing"
  fi
  echo
  echo "== backend logs =="
  rg -n "render-backend-fallback|requested=|effective=|Editor pane built|ControlServer started in" \
    "${OUT_DIR}/opengl.log" "${OUT_DIR}/vulkan.log" || true
  echo
  echo "== decode summaries =="
  for f in "${OUT_DIR}/opengl_decode_rates.json" "${OUT_DIR}/vulkan_decode_rates.json" \
           "${OUT_DIR}/opengl_decode_seeks.json" "${OUT_DIR}/vulkan_decode_seeks.json"; do
    echo "--- $(basename "$f") ---"
    cat "$f" || true
    echo
  done
} | tee "${OUT_DIR}/summary.txt"

echo "Wrote artifacts to: ${OUT_DIR}"
