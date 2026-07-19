#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${repo_root}/build"
output_dir="${repo_root}/dist"
skip_build="no"
appdir_only="no"

usage() {
    echo "Usage: $0 [--skip-build] [--appdir-only] [--output-dir PATH]" >&2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)
            skip_build="yes"
            shift
            ;;
        --appdir-only)
            appdir_only="yes"
            shift
            ;;
        --output-dir)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            output_dir="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            exit 2
            ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux packaging must run on Linux." >&2
    exit 1
fi

for tool in ldd patchelf qtpaths6 tar; do
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "Required packaging tool is missing: ${tool}" >&2
        exit 1
    fi
done

if [[ "${skip_build}" != "yes" ]]; then
    "${repo_root}/build.sh"
fi

editor_binary="${build_dir}/jcut"
shader_dir="${build_dir}/generated/vulkan_shaders"
if [[ ! -x "${editor_binary}" ]]; then
    echo "Editor executable not found: ${editor_binary}" >&2
    exit 1
fi
if [[ ! -f "${shader_dir}/effects.vert.spv" ]]; then
    echo "Compiled Vulkan shaders not found: ${shader_dir}" >&2
    exit 1
fi

case "$(uname -m)" in
    x86_64|amd64) package_arch="x86_64" ;;
    aarch64|arm64) package_arch="aarch64" ;;
    *) package_arch="$(uname -m)" ;;
esac

mkdir -p "${output_dir}"
app_dir="${output_dir}/JCut-${package_arch}.AppDir"
archive_path="${output_dir}/JCut-${package_arch}.tar.gz"

rm -rf -- "${app_dir}"
mkdir -p \
    "${app_dir}/usr/bin" \
    "${app_dir}/usr/lib" \
    "${app_dir}/usr/plugins" \
    "${app_dir}/usr/share/applications" \
    "${app_dir}/usr/share/icons/hicolor/scalable/apps" \
    "${app_dir}/usr/share/jcut/shaders"

install -m 0755 "${editor_binary}" "${app_dir}/usr/bin/jcut"
install -m 0755 "${repo_root}/packaging/linux/AppRun" "${app_dir}/AppRun"
install -m 0644 \
    "${repo_root}/packaging/linux/io.github.jcut.JCut.desktop" \
    "${app_dir}/io.github.jcut.JCut.desktop"
install -m 0644 \
    "${repo_root}/packaging/linux/io.github.jcut.JCut.desktop" \
    "${app_dir}/usr/share/applications/io.github.jcut.JCut.desktop"
install -m 0644 \
    "${repo_root}/packaging/linux/io.github.jcut.JCut.svg" \
    "${app_dir}/io.github.jcut.JCut.svg"
ln -s "io.github.jcut.JCut.svg" "${app_dir}/.DirIcon"
install -m 0644 \
    "${repo_root}/packaging/linux/io.github.jcut.JCut.svg" \
    "${app_dir}/usr/share/icons/hicolor/scalable/apps/io.github.jcut.JCut.svg"
cp -a "${shader_dir}/." "${app_dir}/usr/share/jcut/shaders/"

qt_plugin_dir="$(qtpaths6 --plugin-dir)"
if [[ ! -d "${qt_plugin_dir}/platforms" ]]; then
    echo "Qt platform plugins not found under ${qt_plugin_dir}" >&2
    exit 1
fi

plugin_groups=(
    iconengines
    imageformats
    networkinformation
    platforms
    tls
    wayland-decoration-client
    wayland-graphics-integration-client
    xcbglintegrations
)
for group in "${plugin_groups[@]}"; do
    if [[ -d "${qt_plugin_dir}/${group}" ]]; then
        cp -a "${qt_plugin_dir}/${group}" "${app_dir}/usr/plugins/"
    fi
done

is_host_runtime_library() {
    local name="$1"
    case "${name}" in
        ld-linux*.so*|libanl.so*|libc.so*|libdl.so*|libm.so*|libmvec.so*|libnss_*.so*|libpthread.so*|libresolv.so*|librt.so*|libthread_db.so*|libutil.so*)
            return 0
            ;;
        libcuda.so*|libdrm.so*|libEGL.so*|libgbm.so*|libGL.so*|libGLdispatch.so*|libGLX.so*|libnvidia-*.so*|libOpenGL.so*|libvulkan.so*)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

declare -a scan_queue=("${app_dir}/usr/bin/jcut")
while IFS= read -r plugin; do
    scan_queue+=("${plugin}")
done < <(find "${app_dir}/usr/plugins" -type f -name '*.so*' -print)

declare -A scanned=()
queue_index=0
bundle_ldd() {
    LD_LIBRARY_PATH="${app_dir}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}" ldd "$1"
}

while (( queue_index < ${#scan_queue[@]} )); do
    candidate="${scan_queue[queue_index]}"
    ((queue_index += 1))
    [[ -n "${scanned[${candidate}]:-}" ]] && continue
    scanned["${candidate}"]=1

    if bundle_ldd "${candidate}" 2>&1 | grep -q 'not found'; then
        echo "Unresolved dependency while scanning ${candidate}:" >&2
        bundle_ldd "${candidate}" >&2 || true
        exit 1
    fi

    while IFS= read -r dependency; do
        [[ -n "${dependency}" && -f "${dependency}" ]] || continue
        library_name="$(basename "${dependency}")"
        is_host_runtime_library "${library_name}" && continue
        destination="${app_dir}/usr/lib/${library_name}"
        if [[ -e "${destination}" ]]; then
            if ! cmp -s "${dependency}" "${destination}"; then
                echo "Conflicting libraries share the name ${library_name}: ${dependency}" >&2
                exit 1
            fi
            continue
        fi
        cp -L "${dependency}" "${destination}"
        chmod u+w "${destination}"
        scan_queue+=("${destination}")
    done < <(bundle_ldd "${candidate}" | awk '
        /=> \/[^ ]+/ { print $3 }
        /^\/[^( ]+/ { print $1 }
    ')
done

patchelf --force-rpath --set-rpath '$ORIGIN/../lib' "${app_dir}/usr/bin/jcut"
while IFS= read -r library; do
    patchelf --force-rpath --set-rpath '$ORIGIN' "${library}"
done < <(find "${app_dir}/usr/lib" -type f -name '*.so*' -print)
while IFS= read -r plugin; do
    patchelf --force-rpath --set-rpath '$ORIGIN/../../lib' "${plugin}"
done < <(find "${app_dir}/usr/plugins" -type f -name '*.so*' -print)

if bundle_ldd "${app_dir}/usr/bin/jcut" 2>&1 | grep -q 'not found'; then
    echo "Packaged executable still has unresolved dependencies:" >&2
    bundle_ldd "${app_dir}/usr/bin/jcut" >&2 || true
    exit 1
fi

if [[ "${appdir_only}" != "yes" ]]; then
    rm -f -- "${archive_path}"
    tar -C "${output_dir}" -czf "${archive_path}" "$(basename "${app_dir}")"
    echo "Created ${archive_path}"
fi

if command -v appimagetool >/dev/null 2>&1; then
    appimage_path="${output_dir}/JCut-${package_arch}.AppImage"
    rm -f -- "${appimage_path}"
    ARCH="${package_arch}" appimagetool "${app_dir}" "${appimage_path}"
    echo "Created ${appimage_path}"
else
    echo "appimagetool not installed; AppDir and tarball are ready, AppImage generation skipped."
fi

echo "Created ${app_dir}"
