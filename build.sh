#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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
