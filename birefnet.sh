#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE_NAME="${BIREFNET_IMAGE_NAME:-jcut-birefnet:cu126}"
MODEL_CACHE="${BIREFNET_MODEL_CACHE:-$ROOT_DIR/.cache/birefnet/hf}"
RUNTIME_CACHE="${BIREFNET_RUNTIME_CACHE:-$ROOT_DIR/.cache/birefnet/runtime}"
RUN_AS_ROOT="${BIREFNET_DOCKER_RUN_AS_ROOT:-0}"
CONTAINER_NAME="${BIREFNET_CONTAINER_NAME:-}"
JOB_ROOT="${BIREFNET_JOB_ROOT:-}"
LOG_PATH="${BIREFNET_LOG_PATH:-}"
PREVIEW_ONLY=0
SOURCE_FRAME=""

usage() {
  cat >&2 <<'EOF'
Usage: ./birefnet.sh <video> --output-dir <directory> [options]

Options:
  --model <huggingface-id>       Model repository (default: ZhengPeng7/BiRefNet-matting)
  --revision <revision>          Model revision/commit (default: pinned known-good revision)
  --cpu                          Run on CPU instead of CUDA
  --fp32                         Disable FP16 inference
  --alpha-tolerance <0..0.99>    Remove low-confidence foreground (default: 0)
  --source-frame <frame-key>     Preview the exact DecoderContext source-frame key
  --no-resume                    Re-render existing alpha frames
EOF
}

if [[ $# -lt 1 ]]; then
  usage
  exit 2
fi
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
  usage
  exit 0
fi

INPUT="$1"
shift
OUTPUT_DIR=""
FORWARD_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      [[ $# -ge 2 ]] || { echo "ERROR: --output-dir requires a value" >&2; exit 2; }
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --model|--revision|--guidance-gate-radius|--alpha-tolerance|--progress-every|--frame-index|--source-frame|--live-preview-every)
      [[ $# -ge 2 ]] || { echo "ERROR: $1 requires a value" >&2; exit 2; }
      if [[ "$1" == "--frame-index" ]]; then
        PREVIEW_ONLY=1
      elif [[ "$1" == "--source-frame" ]]; then
        PREVIEW_ONLY=1
        SOURCE_FRAME="$2"
        shift 2
        continue
      fi
      FORWARD_ARGS+=("$1" "$2")
      shift 2
      ;;
    --cpu)
      FORWARD_ARGS+=("--device" "cpu")
      shift
      ;;
    --fp32)
      FORWARD_ARGS+=("--no-fp16")
      shift
      ;;
    --no-resume)
      FORWARD_ARGS+=("--no-resume")
      shift
      ;;
    --live-preview)
      FORWARD_ARGS+=("--live-preview")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

[[ -n "$OUTPUT_DIR" ]] || { echo "ERROR: --output-dir is required" >&2; exit 2; }
[[ -f "$INPUT" ]] || { echo "ERROR: input video does not exist: $INPUT" >&2; exit 2; }
command -v docker >/dev/null 2>&1 || { echo "ERROR: docker is required" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "ERROR: python3 is required" >&2; exit 1; }
command -v ffprobe >/dev/null 2>&1 || { echo "ERROR: ffprobe is required" >&2; exit 1; }

INPUT_ABS="$(realpath "$INPUT")"
mkdir -p "$OUTPUT_DIR" "$MODEL_CACHE" "$RUNTIME_CACHE"
OUTPUT_ABS="$(realpath "$OUTPUT_DIR")"
MODEL_CACHE_ABS="$(realpath "$MODEL_CACHE")"
RUNTIME_CACHE_ABS="$(realpath "$RUNTIME_CACHE")"

if [[ -n "$LOG_PATH" ]]; then
  mkdir -p "$(dirname "$LOG_PATH")"
  LOG_PATH="$(realpath "$(dirname "$LOG_PATH")")/$(basename "$LOG_PATH")"
  exec > >(tee -a "$LOG_PATH") 2>&1
  echo
  echo "[birefnet] launch: $(date --iso-8601=seconds)"
fi

if [[ -n "$SOURCE_FRAME" ]]; then
  DECODED_FRAME="$(python3 "$ROOT_DIR/jcut_frame_index_map.py" \
    --input "$INPUT_ABS" \
    --lookup-source-frame "$SOURCE_FRAME")"
  [[ "$DECODED_FRAME" =~ ^[1-9][0-9]*$ ]] || {
    echo "ERROR: unable to resolve source frame $SOURCE_FRAME" >&2
    exit 1
  }
  FORWARD_ARGS+=("--frame-index" "$DECODED_FRAME")
  echo "[birefnet] source frame $SOURCE_FRAME -> decoded ordinal $DECODED_FRAME" >&2
fi

# BiRefNet numbers masks by decoded presentation order, while JCut addresses
# source frames by timestamp.  Generate the durable conversion before any
# resumable inference so partial and completed runs use the same frame domain.
if [[ "$PREVIEW_ONLY" != "1" ]]; then
  python3 "$ROOT_DIR/jcut_frame_index_map.py" \
    --input "$INPUT_ABS" \
    --output "$OUTPUT_ABS/jcut_frame_map.tsv"
fi

JOB_ROOT_ABS=""
if [[ -n "$JOB_ROOT" ]]; then
  mkdir -p "$JOB_ROOT"
  JOB_ROOT_ABS="$(realpath "$JOB_ROOT")"
fi

if [[ -n "$CONTAINER_NAME" && ! "$CONTAINER_NAME" =~ ^[A-Za-z0-9][A-Za-z0-9_.-]+$ ]]; then
  echo "[birefnet] invalid Docker container name: $CONTAINER_NAME" >&2
  exit 2
fi

BUILD_HASH="$(sha256sum "$ROOT_DIR/birefnet/Dockerfile" "$ROOT_DIR/birefnet/requirements.txt" | sha256sum | cut -d' ' -f1)"
IMAGE_HASH="$(docker image inspect "$IMAGE_NAME" --format '{{ index .Config.Labels "birefnet.build_hash" }}' 2>/dev/null || true)"
if [[ "$IMAGE_HASH" != "$BUILD_HASH" ]]; then
  echo "[birefnet] building runtime image $IMAGE_NAME" >&2
  docker build \
    --build-arg "BUILD_HASH=$BUILD_HASH" \
    --label "birefnet.build_hash=$BUILD_HASH" \
    -t "$IMAGE_NAME" \
    "$ROOT_DIR/birefnet"
fi

DOCKER_ARGS=(--rm --init)
if [[ -n "$CONTAINER_NAME" ]]; then
  if EXISTING_STATE="$(docker inspect --format '{{.State.Status}}' "$CONTAINER_NAME" 2>/dev/null)"; then
    if [[ "$EXISTING_STATE" == "running" ]]; then
      echo "[birefnet] container '$CONTAINER_NAME' is already running" >&2
      exit 44
    fi
    docker rm "$CONTAINER_NAME" >/dev/null
  fi
  DOCKER_ARGS+=(--name "$CONTAINER_NAME")
fi
if [[ "$RUN_AS_ROOT" != "1" ]]; then
  DOCKER_ARGS+=(--user "$(id -u):$(id -g)")
fi
if [[ " ${FORWARD_ARGS[*]} " != *" --device cpu "* ]]; then
  DOCKER_ARGS+=(--gpus all)
fi
DOCKER_ARGS+=(
  -e HOME=/runtime/home
  -e HF_HOME=/models
  -e HUGGINGFACE_HUB_CACHE=/models
  -e TRANSFORMERS_CACHE=/models
  -v "$INPUT_ABS:/input/video:ro"
  -v "$OUTPUT_ABS:/output"
  -v "$MODEL_CACHE_ABS:/models"
  -v "$RUNTIME_CACHE_ABS:/runtime"
  -v "$ROOT_DIR/birefnet_run.py:/workspace/birefnet_run.py:ro"
  -v "$ROOT_DIR/jcut_frame_index_map.py:/workspace/jcut_frame_index_map.py:ro"
  -v "$ROOT_DIR/sam3_resume.py:/workspace/sam3_resume.py:ro"
)
if [[ -n "$JOB_ROOT_ABS" ]]; then
  DOCKER_ARGS+=(
    --label "jcut.operation=birefnet"
    --label "jcut.job_root=$JOB_ROOT_ABS"
    --label "jcut.manifest=$JOB_ROOT_ABS/manifest.json"
    -v "$JOB_ROOT_ABS:/job"
  )
fi

COMMAND=(python /workspace/birefnet_run.py --input /input/video --output-dir /output)
COMMAND+=("${FORWARD_ARGS[@]}")
if [[ -n "$JOB_ROOT_ABS" ]]; then
  COMMAND+=(--progress-json /job/progress.json)
fi
echo "[birefnet] output: $OUTPUT_ABS" >&2
if [[ -n "$CONTAINER_NAME" ]]; then
  echo "[birefnet] container: $CONTAINER_NAME" >&2
fi
docker run "${DOCKER_ARGS[@]}" "$IMAGE_NAME" "${COMMAND[@]}"
