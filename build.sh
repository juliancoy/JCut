#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
FFMPEG_SUBMODULE_PATH="editor/ffmpeg"
FFMPEG_PKGCONFIG_DIR="${SCRIPT_DIR}/ffmpeg-install/lib/pkgconfig"
ASAN="OFF"

for arg in "$@"; do
    case "$arg" in
        --asan)
            ASAN="ON"
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: $0 [--asan]" >&2
            exit 1
            ;;
    esac
done

if ! git -C "${REPO_ROOT}" config -f .gitmodules --get "submodule.${FFMPEG_SUBMODULE_PATH}.path" >/dev/null 2>&1; then
    echo "Missing submodule entry for ${FFMPEG_SUBMODULE_PATH} in ${REPO_ROOT}/.gitmodules" >&2
    exit 1
fi

git -C "${REPO_ROOT}" submodule update --init --recursive -- "${FFMPEG_SUBMODULE_PATH}"

if [[ ! -d "${SCRIPT_DIR}/ffmpeg" ]]; then
    echo "FFmpeg submodule checkout missing at ${SCRIPT_DIR}/ffmpeg" >&2
    exit 1
fi

if [[ ! -d "${FFMPEG_PKGCONFIG_DIR}" ]]; then
    echo "Missing FFmpeg pkg-config directory: ${FFMPEG_PKGCONFIG_DIR}" >&2
    echo "Build/install FFmpeg into ${SCRIPT_DIR}/ffmpeg-install first." >&2
    exit 1
fi

export PKG_CONFIG_PATH="${FFMPEG_PKGCONFIG_DIR}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

if [[ "${ASAN}" == "ON" ]]; then
    BUILD_DIR="${SCRIPT_DIR}/build-asan"
else
    BUILD_DIR="${SCRIPT_DIR}/build"
fi

if [[ "${ASAN}" == "ON" ]]; then
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
        -DEDITOR_ASAN=ON \
        -DCMAKE_BUILD_TYPE=Debug
else
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
        -DEDITOR_ASAN=OFF
fi
cmake --build "${BUILD_DIR}" -j
