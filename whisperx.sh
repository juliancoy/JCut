#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Keep project-relative files next to this script.
PROJECT_ROOT="$SCRIPT_DIR"
TOKEN_FILE="$PROJECT_ROOT/hftoken.txt"
CACHE_DIR="$PROJECT_ROOT/.cache"
DOCKER_INSTALL_URL="https://docs.docker.com/engine/install/"
NVIDIA_CONTAINER_URL="https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/1.12.0/install-guide.html"

if [[ $# -lt 1 ]]; then
  echo "Usage: $0 <media-file>" >&2
  exit 1
fi

INPUT_PATH="$1"
if [[ "$INPUT_PATH" != /* ]]; then
  INPUT_PATH="$(cd "$(dirname "$INPUT_PATH")" && pwd)/$(basename "$INPUT_PATH")"
fi

if [[ ! -f "$INPUT_PATH" ]]; then
  echo "ERROR: input file not found: $INPUT_PATH" >&2
  exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: Docker is not installed or not on PATH." >&2
  echo "Install Docker first: $DOCKER_INSTALL_URL" >&2
  exit 1
fi

if ! docker info >/dev/null 2>&1; then
  echo "ERROR: Docker is installed, but the Docker daemon is not reachable." >&2
  echo "Start Docker and verify 'docker info' works, then retry." >&2
  echo "Docker install docs: $DOCKER_INSTALL_URL" >&2
  exit 1
fi

DOCKER_RUNTIMES="$(docker info --format '{{json .Runtimes}}' 2>/dev/null || true)"
HAS_NVIDIA_RUNTIME=0
if [[ "$DOCKER_RUNTIMES" == *'"nvidia"'* ]]; then
  HAS_NVIDIA_RUNTIME=1
fi
if command -v nvidia-container-toolkit >/dev/null 2>&1 || command -v nvidia-ctk >/dev/null 2>&1; then
  HAS_NVIDIA_RUNTIME=1
fi

DOCKER_GPU_ARGS=()
WHISPERX_DEVICE_ARGS=()
WHISPERX_EXTRA_ARGS=()
WHISPERX_TOKEN_ARGS=()
WHISPERX_MODEL="large-v3"
WHISPERX_LANGUAGE="en"
HF_TOKEN_REQUIRED=0

if [[ $HAS_NVIDIA_RUNTIME -eq 1 ]]; then
  DOCKER_GPU_ARGS=(--gpus all)
  WHISPERX_EXTRA_ARGS=(--diarize)
  HF_TOKEN_REQUIRED=1
else
  echo "WARNING: NVIDIA Container Toolkit is not available for Docker." >&2
  echo "Falling back to CPU transcription with a smaller fast model and no diarization." >&2
  echo "GPU setup docs: $NVIDIA_CONTAINER_URL" >&2
  WHISPERX_DEVICE_ARGS=(--device cpu --compute_type int8)
  WHISPERX_MODEL="small"
fi

HF_TOKEN=""
if [[ $HF_TOKEN_REQUIRED -eq 1 ]]; then
  if [[ ! -f "$TOKEN_FILE" ]]; then
    echo "No Hugging Face token found at $TOKEN_FILE"
    printf "Enter Hugging Face token: " >&2
    read -r HF_TOKEN_INPUT
    if [[ -z "${HF_TOKEN_INPUT:-}" ]]; then
      echo "ERROR: token is required for diarization-enabled runs" >&2
      exit 1
    fi
    printf '%s\n' "$HF_TOKEN_INPUT" > "$TOKEN_FILE"
    chmod 600 "$TOKEN_FILE"
    echo "Saved token to $TOKEN_FILE"
  fi

  # read token and strip newline/carriage return
  HF_TOKEN="$(tr -d '\r\n' < "$TOKEN_FILE")"
  WHISPERX_TOKEN_ARGS=(--hf_token "$HF_TOKEN")
fi

# ensure local cache dir exists (mapped into container)
mkdir -p "$CACHE_DIR"

# write outputs alongside the input file
INPUT_DIR="$(dirname "$INPUT_PATH")"
INPUT_BASENAME="$(basename "$INPUT_PATH")"
CONTAINER_MEDIA_DIR="/media"
CONTAINER_INPUT_PATH="$CONTAINER_MEDIA_DIR/$INPUT_BASENAME"
OUT_DIR="$CONTAINER_MEDIA_DIR"

DOCKER_TTY_ARGS=(-i)
if [[ -t 0 && -t 1 ]]; then
  DOCKER_TTY_ARGS+=(-t)
fi

# optional: avoid exposing token in ps output by not using env var inline on the docker command line
# (here we set it with -e so it is available inside the container)
docker run "${DOCKER_GPU_ARGS[@]}" "${DOCKER_TTY_ARGS[@]}" \
  --user "$(id -u):$(id -g)" \
  -v "$CACHE_DIR":/.cache \
  -v "$CACHE_DIR":/tmp/.cache \
  -v "$PROJECT_ROOT":/app -w /app \
  -v "$INPUT_DIR":"$CONTAINER_MEDIA_DIR" \
  -e HOME="/tmp" \
  -e WHISPER_MODEL="$WHISPERX_MODEL" \
  -e LANG="$WHISPERX_LANGUAGE" \
  -e MPLCONFIGDIR="/tmp/.cache/matplotlib" \
  -e HF_HOME="/tmp/.cache/huggingface" \
  -e HF_HUB_CACHE="/tmp/.cache/huggingface/hub" \
  -e TRANSFORMERS_CACHE="/tmp/.cache/huggingface/transformers" \
  -e TORCH_HOME="/tmp/.cache/torch" \
  -e HF_TOKEN="$HF_TOKEN" \
  ghcr.io/jim60105/whisperx:large-v3-tl-77e20c4 \
  whisperx "$CONTAINER_INPUT_PATH" \
    --output_dir "$OUT_DIR" \
    --output_format json \
    "${WHISPERX_DEVICE_ARGS[@]}" \
    "${WHISPERX_EXTRA_ARGS[@]}" \
    "${WHISPERX_TOKEN_ARGS[@]}"

#-v "$(pwd)/.cache":/.cache/torch/hub/checkpoints \
