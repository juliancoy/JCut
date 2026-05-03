#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
FFMPEG_SUBMODULE_PATH="ffmpeg"
FFMPEG_SRC_DIR="${SCRIPT_DIR}/ffmpeg"
FFMPEG_BUILD_DIR="${SCRIPT_DIR}/ffmpeg-build"
FFMPEG_INSTALL_DIR="${SCRIPT_DIR}/ffmpeg-install"
FFMPEG_PKGCONFIG_DIR="${SCRIPT_DIR}/ffmpeg-install/lib/pkgconfig"
FFMPEG_PROFILE_FILE="${FFMPEG_INSTALL_DIR}/.build-profile"
FFMPEG_VERSION_FILE="${FFMPEG_INSTALL_DIR}/.build-version"
NVCODEC_SUBMODULE_PATH="nv-codec-headers"
NVCODEC_SRC_DIR="${SCRIPT_DIR}/nv-codec-headers"
NVCODEC_PKGCONFIG_FILE="${FFMPEG_PKGCONFIG_DIR}/ffnvcodec.pc"
NVCODEC_VERSION_FILE="${FFMPEG_INSTALL_DIR}/.build-nvcodec-version"
ASAN="OFF"
FFMPEG_PROFILE="safe"
BUILD_TARGET="editor"
RUN_EDITOR="no"
CMAKE_GENERATOR="Ninja"
FFMPEG_REBUILT="0"
RUN_FACE_BENCH="no"

ensure_submodule_checkout() {
    local submodule_path="$1"
    local checkout_dir="$2"
    if [[ -d "${checkout_dir}" ]]; then
        return 0
    fi
    if ! git -C "${REPO_ROOT}" config -f .gitmodules --get "submodule.${submodule_path}.path" >/dev/null 2>&1; then
        echo "Missing submodule entry for ${submodule_path} in ${REPO_ROOT}/.gitmodules" >&2
        exit 1
    fi
    git -C "${REPO_ROOT}" submodule update --init --recursive -- "${submodule_path}"
    if [[ ! -d "${checkout_dir}" ]]; then
        echo "Submodule checkout missing at ${checkout_dir}" >&2
        exit 1
    fi
}

ensure_nvcodec_installed() {
    if [[ "${FFMPEG_PROFILE}" != "nvidia" ]]; then
        return 0
    fi

    ensure_submodule_checkout "${NVCODEC_SUBMODULE_PATH}" "${NVCODEC_SRC_DIR}"
    mkdir -p "${FFMPEG_INSTALL_DIR}"

    local installed_version=""
    local current_version=""
    current_version="$(git -C "${NVCODEC_SRC_DIR}" rev-parse HEAD)"
    if [[ -f "${NVCODEC_VERSION_FILE}" ]]; then
        installed_version="$(<"${NVCODEC_VERSION_FILE}")"
    fi

    if [[ -f "${NVCODEC_PKGCONFIG_FILE}" && "${installed_version}" == "${current_version}" ]]; then
        return 0
    fi

    if [[ -n "${installed_version}" && "${installed_version}" != "${current_version}" ]]; then
        echo "nv-codec-headers revision mismatch: installed='${installed_version}', requested='${current_version}'"
    else
        echo "Bootstrapping nv-codec-headers into ${FFMPEG_INSTALL_DIR}..."
    fi

    make -C "${NVCODEC_SRC_DIR}" -j"$(nproc)"
    make -C "${NVCODEC_SRC_DIR}" PREFIX="${FFMPEG_INSTALL_DIR}" install
    printf '%s\n' "${current_version}" > "${NVCODEC_VERSION_FILE}"

    if [[ ! -f "${NVCODEC_PKGCONFIG_FILE}" ]]; then
        echo "nv-codec-headers install finished but ${NVCODEC_PKGCONFIG_FILE} was not produced." >&2
        exit 1
    fi
}

ensure_ffmpeg_installed() {
    local codec_pc="${FFMPEG_PKGCONFIG_DIR}/libavcodec.pc"
    local format_pc="${FFMPEG_PKGCONFIG_DIR}/libavformat.pc"
    local util_pc="${FFMPEG_PKGCONFIG_DIR}/libavutil.pc"
    local swr_pc="${FFMPEG_PKGCONFIG_DIR}/libswresample.pc"
    local sws_pc="${FFMPEG_PKGCONFIG_DIR}/libswscale.pc"

    local installed_profile=""
    local installed_version=""
    local installed_nvcodec_version=""
    local installed_prefix=""
    local current_version=""
    current_version="$(git -C "${FFMPEG_SRC_DIR}" rev-parse HEAD)"
    if [[ -f "${FFMPEG_PROFILE_FILE}" ]]; then
        installed_profile="$(<"${FFMPEG_PROFILE_FILE}")"
    fi
    if [[ -f "${FFMPEG_VERSION_FILE}" ]]; then
        installed_version="$(<"${FFMPEG_VERSION_FILE}")"
    fi
    if [[ -f "${NVCODEC_VERSION_FILE}" ]]; then
        installed_nvcodec_version="$(<"${NVCODEC_VERSION_FILE}")"
    fi
    if [[ -f "${codec_pc}" ]]; then
        installed_prefix="$(sed -n 's/^prefix=//p' "${codec_pc}" | head -n1)"
    fi

    local current_nvcodec_version=""
    if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
        ensure_nvcodec_installed
        current_nvcodec_version="$(git -C "${NVCODEC_SRC_DIR}" rev-parse HEAD)"
    fi

    if [[ -f "${codec_pc}" && -f "${format_pc}" && -f "${util_pc}" && -f "${swr_pc}" && -f "${sws_pc}" && "${installed_profile}" == "${FFMPEG_PROFILE}" && "${installed_version}" == "${current_version}" && "${installed_prefix}" == "${FFMPEG_INSTALL_DIR}" ]]; then
        if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
            if [[ ! -f "${NVCODEC_PKGCONFIG_FILE}" || "${installed_nvcodec_version}" != "${current_nvcodec_version}" ]]; then
                echo "FFmpeg NVIDIA toolchain mismatch detected; rebuilding against local nv-codec-headers."
            else
                return 0
            fi
        else
            return 0
        fi
    fi

    FFMPEG_REBUILT="1"
    if [[ "${FFMPEG_PROFILE}" == "nvidia" && -n "${installed_nvcodec_version}" && "${installed_nvcodec_version}" != "${current_nvcodec_version}" ]]; then
        echo "NVIDIA headers revision mismatch: installed='${installed_nvcodec_version}', requested='${current_nvcodec_version}'"
    elif [[ -n "${installed_prefix}" && "${installed_prefix}" != "${FFMPEG_INSTALL_DIR}" ]]; then
        echo "FFmpeg install prefix mismatch: installed='${installed_prefix}', requested='${FFMPEG_INSTALL_DIR}'"
    elif [[ "${installed_profile}" != "${FFMPEG_PROFILE}" && -n "${installed_profile}" ]]; then
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
    if [[ -f "${FFMPEG_BUILD_DIR}/Makefile" ]]; then
        local expected_include="include ${FFMPEG_SRC_DIR}/Makefile"
        if ! rg -Fq "${expected_include}" "${FFMPEG_BUILD_DIR}/Makefile"; then
            echo "FFmpeg build directory references a different source tree; cleaning ${FFMPEG_BUILD_DIR}..."
            rm -rf "${FFMPEG_BUILD_DIR}"
            mkdir -p "${FFMPEG_BUILD_DIR}"
        fi
    fi

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

    local ffmpeg_configure_pkg_config_path="${FFMPEG_PKGCONFIG_DIR}"
    if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
        ffmpeg_configure_pkg_config_path="${ffmpeg_configure_pkg_config_path}:${PKG_CONFIG_PATH}"
    fi

    if ! env PKG_CONFIG_PATH="${ffmpeg_configure_pkg_config_path}" "${ffmpeg_configure[@]}"; then
        echo "FFmpeg configure failed for profile '${FFMPEG_PROFILE}'." >&2
        exit 1
    fi

    if ! env PKG_CONFIG_PATH="${ffmpeg_configure_pkg_config_path}" make -j"$(nproc)"; then
        if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
            cat >&2 <<'EOF'
FFmpeg build failed in NVIDIA profile.
This repository expects local nv-codec-headers from the nv-codec-headers submodule.
Common causes when it still fails:
  1) NVIDIA driver/CUDA runtime mismatch for your host
  2) nv-codec-headers submodule revision is not compatible with ffmpeg submodule revision
Fix path:
  - git submodule update --init --recursive
  - ./build.sh --ffmpeg-enable-nvidia
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

    if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
        printf '%s\n' "${current_nvcodec_version}" > "${NVCODEC_VERSION_FILE}"
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
        --ninja)
            CMAKE_GENERATOR="Ninja"
            ;;
        --make)
            CMAKE_GENERATOR="Unix Makefiles"
            ;;
        --run)
            RUN_EDITOR="yes"
            ;;
        --headless-face-bench)
            RUN_FACE_BENCH="yes"
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Usage: $0 [--asan] [--ffmpeg-enable-nvidia] [--with-tests] [--ninja|--make] [--run] [--headless-face-bench]" >&2
            exit 1
            ;;
    esac
done

ensure_submodule_checkout "${FFMPEG_SUBMODULE_PATH}" "${FFMPEG_SRC_DIR}"
if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
    ensure_submodule_checkout "${NVCODEC_SUBMODULE_PATH}" "${NVCODEC_SRC_DIR}"
fi

ensure_ffmpeg_installed

export PKG_CONFIG_PATH="${FFMPEG_PKGCONFIG_DIR}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

if [[ "${CMAKE_GENERATOR}" == "Ninja" ]]; then
    BUILD_DIR_BASE="${SCRIPT_DIR}/build"
else
    BUILD_DIR_BASE="${SCRIPT_DIR}/build-cmake"
fi

if [[ "${ASAN}" == "ON" ]]; then
    BUILD_DIR="${BUILD_DIR_BASE}-asan"
else
    BUILD_DIR="${BUILD_DIR_BASE}"
fi

reset_stale_cmake_cache() {
    local build_dir="$1"
    local cache_file="${build_dir}/CMakeCache.txt"
    if [[ ! -f "${cache_file}" ]]; then
        return 0
    fi

    local cached_source_dir=""
    local cached_build_dir=""
    local cached_generator=""
    cached_source_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache_file}" | tail -n1)"
    cached_build_dir="$(sed -n 's/^CMAKE_CACHEFILE_DIR:INTERNAL=//p' "${cache_file}" | tail -n1)"
    cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "${cache_file}" | tail -n1)"

    if [[ "${cached_source_dir}" != "${SCRIPT_DIR}" ]] || \
       [[ "${cached_build_dir}" != "${build_dir}" ]] || \
       [[ "${cached_generator}" != "${CMAKE_GENERATOR}" ]]; then
        echo "Detected stale CMake cache in ${build_dir}; recreating build directory..."
        rm -rf "${build_dir}"
    fi
}

reset_stale_cmake_cache "${BUILD_DIR}"
if [[ "${FFMPEG_REBUILT}" == "1" ]]; then
    echo "FFmpeg was rebuilt; recreating ${BUILD_DIR} to clear stale pkg-config cache..."
    rm -rf "${BUILD_DIR}"
fi

if [[ ! -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
    if [[ "${ASAN}" == "ON" ]]; then
        cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
            -G "${CMAKE_GENERATOR}" \
            -DEDITOR_ASAN=ON \
            -DCMAKE_BUILD_TYPE=Debug
    else
        cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
            -G "${CMAKE_GENERATOR}" \
            -DEDITOR_ASAN=OFF
    fi
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
    exec "${BUILD_DIR}/jcut" "$@"
fi

if [[ "${RUN_FACE_BENCH}" == "yes" ]]; then
    VIDEO_PATH="${SCRIPT_DIR}/nasreen.mp4"
    if [[ ! -f "${VIDEO_PATH}" ]]; then
        echo "Headless face bench requested but video is missing: ${VIDEO_PATH}" >&2
        exit 1
    fi
    echo "Running headless face tracer benchmark on ${VIDEO_PATH} ..."
    python3 "${SCRIPT_DIR}/face_tracer_bench.py" \
        --video "${VIDEO_PATH}" \
        --out-dir "${BUILD_DIR}/face_tracer_bench"
fi
