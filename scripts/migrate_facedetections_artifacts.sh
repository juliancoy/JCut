#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
TOOL="${REPO_ROOT}/build/jcut_migrate_facedetections_artifacts"
BUILD_TOOL=1
DRY_RUN=0
NO_BACKUP=0
CLIP_ID=""
ARTIFACT_DIR=""
TRANSCRIPTS=()

usage() {
  cat <<'EOF'
Usage:
  scripts/migrate_facedetections_artifacts.sh [options] <transcript.json> [...]

Options:
  --dry-run       Print planned artifact changes without writing.
  --no-backup     Replace artifacts without creating .bak files.
  --clip <id>     Migrate only one clip id.
  --artifact-dir <dir>
                  Import one completed detached generator artifact directory.
  --no-build      Do not build the migration tool first.
  -h, --help      Show this help.

This migrates FaceDetections artifacts referenced by transcript debug runs
directly to the indexed tracks.idx/tracks.dat and detections.idx/detections.dat shape.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-backup)
      NO_BACKUP=1
      shift
      ;;
    --clip)
      [[ $# -ge 2 ]] || { echo "error: --clip requires a value" >&2; exit 2; }
      CLIP_ID="$2"
      shift 2
      ;;
    --artifact-dir)
      [[ $# -ge 2 ]] || { echo "error: --artifact-dir requires a value" >&2; exit 2; }
      ARTIFACT_DIR="$2"
      shift 2
      ;;
    --no-build)
      BUILD_TOOL=0
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        TRANSCRIPTS+=("$1")
        shift
      done
      ;;
    -*)
      echo "error: unknown option: $1" >&2
      exit 2
      ;;
    *)
      TRANSCRIPTS+=("$1")
      shift
      ;;
  esac
done

if [[ ${#TRANSCRIPTS[@]} -eq 0 ]]; then
  usage >&2
  exit 2
fi

if [[ "${BUILD_TOOL}" -eq 1 ]]; then
  cmake --build "${REPO_ROOT}/build" --target jcut_migrate_facedetections_artifacts -j"$(nproc)"
fi

if [[ ! -x "${TOOL}" ]]; then
  echo "error: missing migration tool at ${TOOL}" >&2
  exit 1
fi

for transcript in "${TRANSCRIPTS[@]}"; do
  args=()
  if [[ "${DRY_RUN}" -eq 1 ]]; then
    args+=(--dry-run)
  fi
  if [[ "${NO_BACKUP}" -eq 1 ]]; then
    args+=(--no-backup)
  fi
  if [[ -n "${CLIP_ID}" ]]; then
    args+=(--clip "${CLIP_ID}")
  fi
  if [[ -n "${ARTIFACT_DIR}" ]]; then
    args+=(--artifact-dir "${ARTIFACT_DIR}")
  fi
  "${TOOL}" "${args[@]}" "${transcript}"
done
