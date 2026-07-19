#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel)"
UNAME_S="$(uname -s)"

build_jobs() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        sysctl -n hw.ncpu
    fi
}
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
RTAUDIO_SUBMODULE_PATH="rtaudio"
RTAUDIO_SRC_DIR="${SCRIPT_DIR}/rtaudio"
CPPMONETIZE_SUBMODULE_PATH="external/CPPMonetize"
CPPMONETIZE_SRC_DIR="${SCRIPT_DIR}/external/CPPMonetize"
EARCUT_SUBMODULE_PATH="external/earcut.hpp"
EARCUT_SRC_DIR="${SCRIPT_DIR}/external/earcut.hpp"
RUBBERBAND_SUBMODULE_PATH="external/rubberband"
RUBBERBAND_SRC_DIR="${SCRIPT_DIR}/external/rubberband"
RUBBERBAND_BUILD_DIR="${SCRIPT_DIR}/.deps/rubberband-build"
RUBBERBAND_INSTALL_DIR="${SCRIPT_DIR}/.deps/rubberband-install"
RUBBERBAND_PKGCONFIG_DIR="${RUBBERBAND_INSTALL_DIR}/lib/pkgconfig"
RUBBERBAND_PKGCONFIG_FILE="${RUBBERBAND_PKGCONFIG_DIR}/rubberband.pc"
RUBBERBAND_VERSION_FILE="${RUBBERBAND_INSTALL_DIR}/.build-version"
QT_PRIVATE_DEV_DIR="${SCRIPT_DIR}/.deps/qt6-base-private-dev"
CURL_DEV_DIR="${SCRIPT_DIR}/.deps/libcurl-dev"
CURL_INCLUDE_DIR=""
CURL_LIBRARY=""
GLFW_INSTALL_DIR="${SCRIPT_DIR}/.deps/glfw-install"
GLFW_PKGCONFIG_DIR=""
ASAN="OFF"
FFMPEG_PROFILE="auto"
FFMPEG_ONLY="no"
RUN_EDITOR="no"
CMAKE_GENERATOR="Ninja"
FFMPEG_REBUILT="0"
RUN_FACE_BENCH="no"

ensure_submodule_checkout() {
    local submodule_path="$1"
    local checkout_dir="$2"
    local required_rel_path="${3:-}"

    local has_checkout="0"
    if [[ -d "${checkout_dir}" ]]; then
        if [[ -n "${required_rel_path}" ]]; then
            if [[ -e "${checkout_dir}/${required_rel_path}" ]]; then
                has_checkout="1"
            fi
        elif find "${checkout_dir}" -mindepth 1 -print -quit >/dev/null 2>&1; then
            has_checkout="1"
        fi
    fi
    if [[ "${has_checkout}" == "1" ]]; then
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
    if [[ -n "${required_rel_path}" && ! -e "${checkout_dir}/${required_rel_path}" ]]; then
        echo "Submodule checkout missing expected file ${checkout_dir}/${required_rel_path}" >&2
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
    local installed_prefix=""
    local current_version=""
    current_version="$(git -C "${NVCODEC_SRC_DIR}" rev-parse HEAD)"
    if [[ -f "${NVCODEC_VERSION_FILE}" ]]; then
        installed_version="$(<"${NVCODEC_VERSION_FILE}")"
    fi
    if [[ -f "${NVCODEC_PKGCONFIG_FILE}" ]]; then
        installed_prefix="$(sed -n 's/^prefix=//p' "${NVCODEC_PKGCONFIG_FILE}" | head -n1)"
    fi

    if [[ -f "${NVCODEC_PKGCONFIG_FILE}" && "${installed_version}" == "${current_version}" && "${installed_prefix}" == "${FFMPEG_INSTALL_DIR}" ]]; then
        return 0
    fi

    if [[ -n "${installed_version}" && "${installed_version}" != "${current_version}" ]]; then
        echo "nv-codec-headers revision mismatch: installed='${installed_version}', requested='${current_version}'"
    elif [[ -n "${installed_prefix}" && "${installed_prefix}" != "${FFMPEG_INSTALL_DIR}" ]]; then
        echo "nv-codec-headers install prefix mismatch: installed='${installed_prefix}', requested='${FFMPEG_INSTALL_DIR}'"
    else
        echo "Bootstrapping nv-codec-headers into ${FFMPEG_INSTALL_DIR}..."
    fi

    make -C "${NVCODEC_SRC_DIR}" -j"$(build_jobs)"
    make -C "${NVCODEC_SRC_DIR}" PREFIX="${FFMPEG_INSTALL_DIR}" install
    printf '%s\n' "${current_version}" > "${NVCODEC_VERSION_FILE}"

    if [[ ! -f "${NVCODEC_PKGCONFIG_FILE}" ]]; then
        echo "nv-codec-headers install finished but ${NVCODEC_PKGCONFIG_FILE} was not produced." >&2
        exit 1
    fi
}

resolve_ffmpeg_profile() {
    if [[ "${FFMPEG_PROFILE}" != "auto" ]]; then
        return 0
    fi

    if [[ "${UNAME_S}" == "Darwin" ]]; then
        # No NVDEC/CUDA on macOS; keep native (NEON) asm enabled.
        FFMPEG_PROFILE="safe"
        return 0
    fi

    if [[ -e /proc/driver/nvidia/version || -e /dev/nvidia0 ]]; then
        FFMPEG_PROFILE="nvidia"
    else
        FFMPEG_PROFILE="safe-noasm"
    fi
}

verify_nvidia_ffmpeg_config() {
    if [[ "${FFMPEG_PROFILE}" != "nvidia" ]]; then
        return 0
    fi

    local config_h="${FFMPEG_BUILD_DIR}/config.h"
    local components_h="${FFMPEG_BUILD_DIR}/config_components.h"
    local failed="0"
    for required in \
        "CONFIG_CUDA 1" \
        "CONFIG_FFNVCODEC 1" \
        "CONFIG_NVDEC 1" \
        "CONFIG_NVENC 1"; do
        if ! grep -Eq "^#define ${required}$" "${config_h}"; then
            echo "FFmpeg NVIDIA profile validation failed: ${required} is not enabled in ${config_h}." >&2
            failed="1"
        fi
    done
    if ! grep -Eq '^#define CONFIG_(H264|HEVC)_CUVID_DECODER 1$' "${components_h}"; then
        echo "FFmpeg NVIDIA profile validation failed: no H.264/HEVC CUVID decoder is enabled in ${components_h}." >&2
        failed="1"
    fi
    if ! grep -Eq '^#define CONFIG_(H264|HEVC|AV1)_NVENC_ENCODER 1$' "${components_h}"; then
        echo "FFmpeg NVIDIA profile validation failed: no NVENC encoder is enabled in ${components_h}." >&2
        failed="1"
    fi
    if [[ "${failed}" != "0" ]]; then
        cat >&2 <<'EOF'
The Vulkan preview path requires hardware/GPU frames. Rebuild FFmpeg with the
NVIDIA profile after fixing nv-codec-headers/driver prerequisites:
  git submodule update --init --recursive
  ./build.sh --ffmpeg-enable-nvidia
EOF
        exit 1
    fi
}

sanitize_ffmpeg_config_mak() {
    local config_mak="${FFMPEG_BUILD_DIR}/ffbuild/config.mak"
    if [[ ! -f "${config_mak}" ]]; then
        return 0
    fi
    sed -i -E 's/^!([A-Za-z0-9_]+)=yes$/# !\1=yes/' "${config_mak}"
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
                verify_nvidia_ffmpeg_config
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
        if ! grep -Fq "${expected_include}" "${FFMPEG_BUILD_DIR}/Makefile"; then
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
        --disable-debug \
        --disable-vulkan
    )

    if [[ "${FFMPEG_PROFILE}" == "safe" || "${FFMPEG_PROFILE}" == "safe-noasm" ]]; then
        ffmpeg_configure+=(
            --disable-cuda
            --disable-cuvid
            --disable-nvenc
            --disable-nvdec
            --disable-ffnvcodec
        )
        if [[ "${FFMPEG_PROFILE}" == "safe-noasm" ]]; then
            ffmpeg_configure+=(
                --disable-x86asm
            )
        fi
    fi

    local ffmpeg_configure_pkg_config_path="${FFMPEG_PKGCONFIG_DIR}"
    if [[ -n "${PKG_CONFIG_PATH:-}" ]]; then
        ffmpeg_configure_pkg_config_path="${ffmpeg_configure_pkg_config_path}:${PKG_CONFIG_PATH}"
    fi

    if ! env PKG_CONFIG_PATH="${ffmpeg_configure_pkg_config_path}" "${ffmpeg_configure[@]}"; then
        echo "FFmpeg configure failed for profile '${FFMPEG_PROFILE}'." >&2
        exit 1
    fi
    sanitize_ffmpeg_config_mak

    if ! env PKG_CONFIG_PATH="${ffmpeg_configure_pkg_config_path}" make -j"$(build_jobs)"; then
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
        verify_nvidia_ffmpeg_config
    fi
}

ensure_rubberband_installed() {
    local current_version=""
    local installed_version=""
    current_version="$(git -C "${RUBBERBAND_SRC_DIR}" rev-parse HEAD)"
    if [[ -f "${RUBBERBAND_VERSION_FILE}" ]]; then
        installed_version="$(<"${RUBBERBAND_VERSION_FILE}")"
    fi

    if [[ -f "${RUBBERBAND_PKGCONFIG_FILE}" && "${installed_version}" == "${current_version}" ]]; then
        return 0
    fi

    if ! command -v meson >/dev/null 2>&1; then
        cat >&2 <<EOF
Rubber Band source is available at ${RUBBERBAND_SRC_DIR}, but Meson is not installed.
Install meson to build the vendored high-quality audio time-stretch backend, then rerun ./build.sh.
The editor requires Rubber Band because the SOLA fallback is intentionally disabled.
EOF
        exit 1
    fi
    if ! command -v ninja >/dev/null 2>&1; then
        echo "Rubber Band requires ninja for the Meson build; install ninja-build and rerun ./build.sh." >&2
        exit 1
    fi

    echo "Bootstrapping Rubber Band into ${RUBBERBAND_INSTALL_DIR}..."
    rm -rf "${RUBBERBAND_BUILD_DIR}"
    meson setup "${RUBBERBAND_BUILD_DIR}" "${RUBBERBAND_SRC_DIR}" \
        --prefix="${RUBBERBAND_INSTALL_DIR}" \
        --libdir=lib \
        --buildtype=release \
        -Ddefault_library=static \
        -Dfft=builtin \
        -Dresampler=builtin \
        -Dcmdline=disabled \
        -Dtests=disabled \
        -Djni=disabled \
        -Dladspa=disabled \
        -Dlv2=disabled \
        -Dvamp=disabled
    meson compile -C "${RUBBERBAND_BUILD_DIR}"
    meson install -C "${RUBBERBAND_BUILD_DIR}"
    printf '%s\n' "${current_version}" > "${RUBBERBAND_VERSION_FILE}"

    if [[ ! -f "${RUBBERBAND_PKGCONFIG_FILE}" ]]; then
        echo "Rubber Band install finished but ${RUBBERBAND_PKGCONFIG_FILE} was not produced." >&2
        exit 1
    fi
}

ensure_qt_private_headers() {
    if [[ "${UNAME_S}" == "Darwin" ]]; then
        # Homebrew Qt ships private headers; Qt6::GuiPrivate/CorePrivate
        # resolve natively and the CMake fix-up function no-ops.
        return 0
    fi

    local qt_version=""
    local qt_package_version=""
    local qt_headers="/usr/include/x86_64-linux-gnu/qt6"
    if command -v qmake6 >/dev/null 2>&1; then
        qt_version="$(qmake6 -query QT_VERSION)"
        qt_headers="$(qmake6 -query QT_INSTALL_HEADERS)"
    elif command -v qtpaths6 >/dev/null 2>&1; then
        qt_version="$(qtpaths6 --qt-version)"
        qt_headers="$(qtpaths6 --query QT_INSTALL_HEADERS)"
    elif command -v pkg-config >/dev/null 2>&1 && pkg-config --exists Qt6Core; then
        qt_version="$(pkg-config --modversion Qt6Core)"
        qt_headers="$(pkg-config --variable=includedir Qt6Core)"
    fi

    if command -v dpkg-query >/dev/null 2>&1; then
        qt_package_version="$(dpkg-query -W -f='${Version}' qt6-base-dev 2>/dev/null || true)"
        if [[ -z "${qt_version}" && "${qt_package_version}" =~ ([0-9]+\.[0-9]+\.[0-9]+) ]]; then
            qt_version="${BASH_REMATCH[1]}"
        fi
    fi

    if [[ -z "${qt_version}" ]]; then
        cat >&2 <<'EOF'
Unable to determine the installed Qt development version.
Install the public and private development packages from the same repository:
  sudo apt-get install qt6-base-dev qt6-base-private-dev
EOF
        exit 1
    fi

    local required_header="${qt_headers}/QtGui/${qt_version}/QtGui/private/qrhi_p.h"
    local local_required_header="${QT_PRIVATE_DEV_DIR}${required_header}"
    if [[ -f "${required_header}" || -f "${local_required_header}" ]]; then
        return 0
    fi

    if ! command -v apt-get >/dev/null 2>&1 || ! command -v dpkg-deb >/dev/null 2>&1; then
        cat >&2 <<'EOF'
Qt private development headers are required but missing.
Install them with:
  sudo apt-get install qt6-base-private-dev
EOF
        exit 1
    fi

    local package_name="qt6-base-private-dev"
    local apt_dir="${SCRIPT_DIR}/.deps/apt/${package_name}-${qt_version}"
    mkdir -p "${apt_dir}" "${QT_PRIVATE_DEV_DIR}"

    local package_spec="${package_name}"
    if [[ -n "${qt_package_version}" ]]; then
        package_spec="${package_name}=${qt_package_version}"
    fi

    echo "Qt ${qt_version} private headers missing; downloading ${package_spec} into ${SCRIPT_DIR}/.deps..."
    if ! (
        cd "${apt_dir}"
        apt-get download "${package_spec}"
    ); then
        cat >&2 <<EOF
Unable to download ${package_spec}. The installed Qt development package may no
longer be available from your configured APT repositories. Refresh or correct
the repositories, then install matching packages together:
  sudo apt-get install qt6-base-dev qt6-base-private-dev
EOF
        exit 1
    fi

    local deb_file=""
    deb_file="$(find "${apt_dir}" -maxdepth 1 -type f -name "${package_name}_*.deb" | sort | tail -n1)"
    if [[ -z "${deb_file}" ]]; then
        echo "Failed to download ${package_name}." >&2
        exit 1
    fi

    dpkg-deb -x "${deb_file}" "${QT_PRIVATE_DEV_DIR}"
    if [[ ! -f "${local_required_header}" ]]; then
        local downloaded_package_version=""
        downloaded_package_version="$(dpkg-deb -f "${deb_file}" Version 2>/dev/null || true)"
        cat >&2 <<EOF
Downloaded ${package_name} ${downloaded_package_version}, but it does not provide the
Qt ${qt_version} private header expected at:
  ${local_required_header}

The installed Qt development files and your configured APT repositories do not match.
Install matching packages from the same repository:
  sudo apt-get install qt6-base-dev qt6-base-private-dev
EOF
        exit 1
    fi
}

ensure_curl_development_files() {
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl; then
        return 0
    fi

    if [[ "${UNAME_S}" != "Linux" ]] || \
       ! command -v apt-get >/dev/null 2>&1 || \
       ! command -v dpkg-deb >/dev/null 2>&1 || \
       ! command -v dpkg-architecture >/dev/null 2>&1; then
        cat >&2 <<'EOF'
The libcurl development files are required but missing.
On Debian or Ubuntu, install them with:
  sudo apt-get install libcurl4-openssl-dev
EOF
        exit 1
    fi

    local multiarch=""
    multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
    CURL_INCLUDE_DIR="${CURL_DEV_DIR}/usr/include/${multiarch}"
    CURL_LIBRARY="/usr/lib/${multiarch}/libcurl.so.4"
    if [[ -f "${CURL_INCLUDE_DIR}/curl/curl.h" && -f "${CURL_LIBRARY}" ]]; then
        return 0
    fi

    local package_name="libcurl4-openssl-dev"
    local apt_dir="${SCRIPT_DIR}/.deps/apt/${package_name}"
    mkdir -p "${apt_dir}" "${CURL_DEV_DIR}"

    echo "libcurl development files missing; downloading ${package_name} into ${SCRIPT_DIR}/.deps..."
    if ! (
        cd "${apt_dir}"
        apt-get download "${package_name}"
    ); then
        cat >&2 <<EOF
Unable to download ${package_name}. Install it from your configured repository:
  sudo apt-get install ${package_name}
EOF
        exit 1
    fi

    local deb_file=""
    deb_file="$(find "${apt_dir}" -maxdepth 1 -type f -name "${package_name}_*.deb" | sort | tail -n1)"
    if [[ -z "${deb_file}" ]]; then
        echo "Failed to download ${package_name}." >&2
        exit 1
    fi

    dpkg-deb -x "${deb_file}" "${CURL_DEV_DIR}"
    if [[ ! -f "${CURL_INCLUDE_DIR}/curl/curl.h" || ! -f "${CURL_LIBRARY}" ]]; then
        cat >&2 <<EOF
The downloaded ${package_name} is incompatible with the installed libcurl runtime.
Install matching runtime and development packages together:
  sudo apt-get install libcurl4t64 ${package_name}
EOF
        exit 1
    fi
}

ensure_glfw_installed() {
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists glfw3; then
        return 0
    fi

    if [[ "${UNAME_S}" != "Linux" ]] || \
       ! command -v apt-get >/dev/null 2>&1 || \
       ! command -v dpkg-deb >/dev/null 2>&1 || \
       ! command -v dpkg-architecture >/dev/null 2>&1; then
        cat >&2 <<'EOF'
GLFW development files are required but missing.
On Debian or Ubuntu, install them with:
  sudo apt-get install libglfw3-dev
EOF
        exit 1
    fi

    local multiarch=""
    multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
    GLFW_PKGCONFIG_DIR="${GLFW_INSTALL_DIR}/usr/lib/${multiarch}/pkgconfig"
    local glfw_pc="${GLFW_PKGCONFIG_DIR}/glfw3.pc"
    if [[ -f "${glfw_pc}" && -f "${GLFW_INSTALL_DIR}/usr/lib/${multiarch}/libglfw.so.3" ]]; then
        sed -i \
            -e "s|^prefix=.*$|prefix=${GLFW_INSTALL_DIR}/usr|" \
            -e 's|^includedir=/usr/include$|includedir=${prefix}/include|' \
            -e "s|^libdir=/usr/lib/${multiarch}$|libdir=\${prefix}/lib/${multiarch}|" \
            "${glfw_pc}"
        return 0
    fi

    local apt_dir="${SCRIPT_DIR}/.deps/apt/glfw"
    mkdir -p "${apt_dir}" "${GLFW_INSTALL_DIR}"
    echo "GLFW missing; downloading libglfw3 and libglfw3-dev into ${SCRIPT_DIR}/.deps..."
    if ! (
        cd "${apt_dir}"
        apt-get download libglfw3 libglfw3-dev
    ); then
        cat >&2 <<'EOF'
Unable to download GLFW. Install its runtime and development package together:
  sudo apt-get install libglfw3-dev
EOF
        exit 1
    fi

    local deb_file=""
    while IFS= read -r deb_file; do
        dpkg-deb -x "${deb_file}" "${GLFW_INSTALL_DIR}"
    done < <(find "${apt_dir}" -maxdepth 1 -type f \( -name 'libglfw3_*.deb' -o -name 'libglfw3-dev_*.deb' \) | sort)

    if [[ ! -f "${glfw_pc}" || ! -f "${GLFW_INSTALL_DIR}/usr/lib/${multiarch}/libglfw.so.3" ]]; then
        echo "Downloaded GLFW packages did not contain the expected development files." >&2
        exit 1
    fi
    sed -i \
        -e "s|^prefix=.*$|prefix=${GLFW_INSTALL_DIR}/usr|" \
        -e 's|^includedir=/usr/include$|includedir=${prefix}/include|' \
        -e "s|^libdir=/usr/lib/${multiarch}$|libdir=\${prefix}/lib/${multiarch}|" \
        "${glfw_pc}"
}

for arg in "$@"; do
    case "$arg" in
        --asan)
            ASAN="ON"
            ;;
        --ffmpeg-enable-nvidia)
            FFMPEG_PROFILE="nvidia"
            ;;
        --ffmpeg-safe)
        FFMPEG_PROFILE="safe-noasm"
            ;;
        --ffmpeg-only)
            FFMPEG_ONLY="yes"
            ;;
        --with-tests)
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
            echo "Usage: $0 [--asan] [--ffmpeg-enable-nvidia|--ffmpeg-safe] [--ffmpeg-only] [--with-tests] [--ninja|--make] [--run] [--headless-face-bench]" >&2
            exit 1
            ;;
    esac
done

resolve_ffmpeg_profile

ensure_submodule_checkout "${FFMPEG_SUBMODULE_PATH}" "${FFMPEG_SRC_DIR}" "configure"
ensure_submodule_checkout "${RTAUDIO_SUBMODULE_PATH}" "${RTAUDIO_SRC_DIR}" "CMakeLists.txt"
ensure_submodule_checkout "${CPPMONETIZE_SUBMODULE_PATH}" "${CPPMONETIZE_SRC_DIR}" "CMakeLists.txt"
ensure_submodule_checkout "${EARCUT_SUBMODULE_PATH}" "${EARCUT_SRC_DIR}" "include/mapbox/earcut.hpp"
ensure_submodule_checkout "${RUBBERBAND_SUBMODULE_PATH}" "${RUBBERBAND_SRC_DIR}" "meson.build"
if [[ "${FFMPEG_PROFILE}" == "nvidia" ]]; then
    ensure_submodule_checkout "${NVCODEC_SUBMODULE_PATH}" "${NVCODEC_SRC_DIR}" "Makefile"
fi

ensure_ffmpeg_installed
if [[ "${FFMPEG_ONLY}" == "yes" ]]; then
    echo "FFmpeg bootstrap complete (--ffmpeg-only). Skipping editor build."
    exit 0
fi
ensure_rubberband_installed
ensure_curl_development_files
ensure_glfw_installed
ensure_qt_private_headers

export PKG_CONFIG_PATH="${FFMPEG_PKGCONFIG_DIR}:${RUBBERBAND_PKGCONFIG_DIR}${GLFW_PKGCONFIG_DIR:+:${GLFW_PKGCONFIG_DIR}}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"

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
        return 0
    fi

    local cached_qt_network_dir=""
    cached_qt_network_dir="$(sed -n 's/^Qt6Network_DIR:PATH=//p' "${cache_file}" | tail -n1)"
    if [[ -n "${cached_qt_network_dir}" ]]; then
        local qt_network_targets="${cached_qt_network_dir}/Qt6NetworkTargets.cmake"
        if [[ -f "${qt_network_targets}" ]] && \
           grep -Eq 'QT_DISABLED_PUBLIC_FEATURES.*(^|;)ssl(;|")' "${qt_network_targets}"; then
            echo "Cached Qt in ${build_dir} was built without SSL support; recreating build directory..."
            rm -rf "${build_dir}"
            return 0
        fi
    fi
}

reset_stale_cmake_cache "${BUILD_DIR}"
if [[ "${FFMPEG_REBUILT}" == "1" ]]; then
    echo "FFmpeg was rebuilt; recreating ${BUILD_DIR} to clear stale pkg-config cache..."
    rm -rf "${BUILD_DIR}"
fi

cmake_configure_args=(
    -S "${SCRIPT_DIR}"
    -B "${BUILD_DIR}"
    -G "${CMAKE_GENERATOR}"
    -DJCUT_USE_SYSTEM_FFMPEG=OFF
    -DWITH_VULKAN=ON
)
if [[ -n "${CURL_INCLUDE_DIR}" ]]; then
    cmake_configure_args+=(
        -DCURL_INCLUDE_DIR="${CURL_INCLUDE_DIR}"
        -DCURL_LIBRARY="${CURL_LIBRARY}"
    )
fi
if [[ -n "${GLFW_PKGCONFIG_DIR}" ]]; then
    cmake_configure_args+=(
        '-UGLFW_*'
    )
fi
if [[ -n "${JCUT_QT_PREFIX:-}" ]]; then
    cmake_configure_args+=(
        -DCMAKE_PREFIX_PATH="${JCUT_QT_PREFIX}"
    )
elif [[ "${UNAME_S}" == "Linux" && -z "${CMAKE_PREFIX_PATH:-}" ]]; then
    cmake_configure_args+=(
        -DCMAKE_PREFIX_PATH="/usr/lib/x86_64-linux-gnu/cmake"
    )
fi
if [[ "${UNAME_S}" == "Darwin" ]]; then
    cmake_configure_args+=(
        -DCMAKE_PREFIX_PATH="$(brew --prefix)"
    )
fi
if [[ "${ASAN}" == "ON" ]]; then
    cmake_configure_args+=(
        -DEDITOR_ASAN=ON
        -DCMAKE_BUILD_TYPE=Debug
    )
else
    cmake_configure_args+=(
        -DEDITOR_ASAN=OFF
    )
fi
cmake "${cmake_configure_args[@]}"

cmake --build "${BUILD_DIR}" -j

# Fix for snap library conflicts (snap's libpthread is incompatible with system glibc)
# Preload system libpthread to avoid snap version being picked up
if [[ "${UNAME_S}" == "Linux" ]]; then
    export LD_PRELOAD="/lib/x86_64-linux-gnu/libpthread.so.0"
fi

if [[ "${RUN_EDITOR}" == "yes" ]]; then
    # Filter out --run from args passed to editor
    shift  # Remove --run from "$@"
    EDITOR_BIN="${BUILD_DIR}/jcut"
    if [[ ! -x "${EDITOR_BIN}" && -x "${BUILD_DIR}/jcut.app/Contents/MacOS/jcut" ]]; then
        EDITOR_BIN="${BUILD_DIR}/jcut.app/Contents/MacOS/jcut"
    fi
    exec "${EDITOR_BIN}" "$@"
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
