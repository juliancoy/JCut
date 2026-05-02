#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/jcut_headless_backend_compare"
OUT_DIR="${1:-${ROOT_DIR}/testbench_assets/headless_bench_$(date +%Y%m%d_%H%M%S)}"
STATE_PATH="${STATE_PATH:-${ROOT_DIR}/testbench_state.json}"
JCUT_BIN="${JCUT_BIN:-${ROOT_DIR}/build/jcut}"
CLIP_ID="${CLIP_ID:-clip_video_01}"
SPEAKER_ID="${SPEAKER_ID:-spk1}"
FORMAT="${FORMAT:-mp4}"
MODE="${MODE:-render_compare}"

if [[ ! -x "${BIN}" ]]; then
  echo "error: missing executable at ${BIN}" >&2
  echo "hint: build with cmake --build build -j" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"
set +e
"${BIN}" \
  --mode "${MODE}" \
  --out-dir "${OUT_DIR}" \
  --jcut-bin "${JCUT_BIN}" \
  --state "${STATE_PATH}" \
  --clip-id "${CLIP_ID}" \
  --speaker-id "${SPEAKER_ID}" \
  --format "${FORMAT}" | tee "${OUT_DIR}/run.log"
status=$?
set -e

if [[ -f "${OUT_DIR}/summary.txt" ]]; then
  cat "${OUT_DIR}/summary.txt"
fi

exit ${status}
