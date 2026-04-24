#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
FFMPEG_SUBMODULE_PATH="editor/ffmpeg"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg"
FFMPEG_BUILD_DIR="${SCRIPT_DIR}/ffmpeg-build"
FFMPEG_INSTALL_DIR="${SCRIPT_DIR}/ffmpeg-install"
FFMPEG_PKGCONFIG_DIR="${SCRIPT_DIR}/ffmpeg-install/lib/pkgconfig"
FFMPEG_PROFILE_FILE="${FFMPEG_INSTALL_DIR}/.build-profile"
FFMPEG_VERSION_FILE="${FFMPEG_INSTALL_DIR}/.build-version"
ASAN="OFF"
FFMPEG_PROFILE="safe"
BUILD_TARGET="editor"
RUN_EDITOR="no"

ensure_ffmpeg_installed() {
    local codec_pc="${FFMPEG_PKGCONFIG_DIR}/libavcodec.pc"
    local format_pc="${FFMPEG_PKGCONFIG_DIR}/libavformat.pc"
    local util_pc="${FFMPEG_PKGCONFIG_DIR}/libavutil.pc"
    local swr_pc="${FFMPEG_PKGCONFIG_DIR}/libswresample.pc"
    local sws_pc="${FFMPEG_PKGCONFIG_DIR}/libswscale.pc"

    local installed_profile=""
    local installed_version=""
    local current_version=""
    current_version="$(git -C "${FFMPEG_SRC_DIR}" rev-parse HEAD)"
    if [[ -f "${FFMPEG_PROFILE_FILE}" ]]; then
        installed_profile="$(<"${FFMPEG_PROFILE_FILE}")"
    fi
    if [[ -f "${FFMPEG_VERSION_FILE}" ]]; then
        installed_version="$(<"${FFMPEG_VERSION_FILE}")"
    fi

    if [[ -f "${codec_pc}" && -f "${format_pc}" && -f "${util_pc}" && -f "${swr_pc}" && -f "${sws_pc}" && "${installed_profile}" == "${FFMPEG_PROFILE}" && "${installed_version}" == "${current_version}" ]]; then
        return 0
    fi

    if [[ "${installed_profile}" != "${FFMPEG_PROFILE}" && -n "${installed_profile}" ]]; then
        echo "FFmpeg profile mismatch: installed='${installed_profile}', requested='${FFMPEG_PROFILE}'"
    elif [[ "${installed_version}" != "${current_version}" && -n "${installed_version}" ]]; then
        echo "FFmpeg source revision mismatch: installed='${installed_version}', requested='${current_version}'"
    else
        echo "FFmpeg pkg-config files missing in ${FFMPEG_PKGCONFIG_DIR}"
    fi
    echo "Bootstrapping FFmpeg profile '${FFMPEG_PROFILE}' into ${FFMPEG_INSTALL_DIR}..."

    if [[ -f "${FFMPEG_SRC_DIR}/config.h" ]]; then
        echo "Found in-tree FFmpeg configure artifacts; cleaning ${FFMPEG_SRC_DIR}..."
        rm -f "${FFMPEG_SRC_DIR}/config.h" \
              "${FFMPEG_SRC_DIR}/config.mak" \
              "${FFMPEG_SRC_DIR}/config.asm"
    fi

    mkdir -p "${FFMPEG_BUILD_DIR}" "${FFMPEG_INSTALL_DIR}"

    pushd "${FFMPEG_BUILD_DIR}" >/dev/null
    local -a ffmpeg_configure=(
        "${FFMPEG_SRC_DIR}/configure"
        --prefix="${FFMPEG_INSTALL_DIR}" \
        --enable-shared \
        --disable-static \
        --disable-programs \
        --disable-doc \
        --disable-debug
    )

    if [[ "${FFMPEG_PROFILE}" == "safe" ]]; then
        ffmpeg_configure+=(
            --disable-cuda
            --disable-cuvid
            --disable-nvenc
            --disable-nvdec
            --disable-ffnvcodec
        )
    fi

    if ! "${ffmpeg_configure[@]}"; then
        echo "FFmpeg configure failed for profile '${FFMPEG_PROFILE}'." >&2
        exit 1
    fi

    if ! make -j"$(nproc)"; then
        if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
            cat >&2 <<'EOF'
FFmpeg build failed in NVIDIA profile.
Required toolchain pieces usually missing/mismatched:
  1) Install/upgrade NVIDIA driver and CUDA toolkit
  2) Install matching nv-codec-headers for your FFmpeg/CUDA stack
  3) Re-run: ./build.sh --ffmpeg-enable-nvidia
Or use the portable profile:
  ./build.sh
EOF
        else
            echo "FFmpeg build failed for profile '${FFMPEG_PROFILE}'." >&2
        fi
        exit 1
    fi

    make install
    printf '%s\n' "${FFMPEG_PROFILE}" > "${FFMPEG_PROFILE_FILE}"
    printf '%s\n' "${current_version}" > "${FFMPEG_VERSION_FILE}"
    popd >/dev/null

    if [[ ! -f "${codec_pc}" ]]; then
        echo "FFmpeg bootstrap finished but ${codec_pc} was not produced." >&2
        exit 1
    fi
}

for arg in "$@"; do
    case "$arg" in
        --asan)
            ASAN="ON"
            ;;
        --ffmpeg-enable-nvidia)
            FFMPEG_PROFILE="nvidia"
            ;;
        --with-tests)
            BUILD_TARGET="all"
            ;;
        --run)
            RUN_EDITOR="yes"
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: $0 [--asan] [--ffmpeg-enable-nvidia] [--with-tests] [--run]" >&2
            exit 1
            ;;
    esac
done

if [[ ! -d "${FFMPEG_SRC_DIR}" ]]; then
    if ! git -C "${REPO_ROOT}" config -f .gitmodules --get "submodule.${FFMPEG_SUBMODULE_PATH}.path" >/dev/null 2>&1; then
        echo "Missing submodule entry for ${FFMPEG_SUBMODULE_PATH} in ${REPO_ROOT}/.gitmodules" >&2
        exit 1
    fi
    git -C "${REPO_ROOT}" submodule update --init --recursive -- "${FFMPEG_SUBMODULE_PATH}"
    if [[ ! -d "${FFMPEG_SRC_DIR}" ]]; then
        echo "FFmpeg submodule checkout missing at ${FFMPEG_SRC_DIR}" >&2
        exit 1
    fi
fi

ensure_ffmpeg_installed

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
if [[ "${BUILD_TARGET}" == "all" ]]; then
    cmake --build "${BUILD_DIR}" -j
else
    cmake --build "${BUILD_DIR}" --target editor -j
fi

# Fix for snap library conflicts (snap's libpthread is incompatible with system glibc)
# Preload system libpthread to avoid snap version being picked up
export LD_PRELOAD="/lib/x86_64-linux-gnu/libpthread.so.0"

if [[ "${RUN_EDITOR}" == "yes" ]]; then
    # Filter out --run from args passed to editor
    shift  # Remove --run from "$@"
    exec "${BUILD_DIR}/editor" "$@"
fi
