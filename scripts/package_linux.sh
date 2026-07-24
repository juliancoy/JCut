#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
build_dir="${repo_root}/build"
output_dir="${repo_root}/dist"
skip_build="no"
appdir_only="no"
allow_dirty="no"
package_version=""
qt_license=""
qt_source_url=""
rubberband_license=""

usage() {
    echo "Usage: $0 --version VERSION --qt-license commercial|lgpl --rubberband-license commercial|gpl [options]" >&2
    echo "Options: --skip-build --appdir-only --allow-dirty --qt-source-url URL --output-dir PATH" >&2
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
        --allow-dirty)
            allow_dirty="yes"
            shift
            ;;
        --version)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            package_version="$2"
            shift 2
            ;;
        --qt-license)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            qt_license="$2"
            shift 2
            ;;
        --qt-source-url)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            qt_source_url="$2"
            shift 2
            ;;
        --rubberband-license)
            [[ $# -ge 2 ]] || { usage; exit 2; }
            rubberband_license="$2"
            shift 2
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

if [[ -z "${package_version}" || ! "${package_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
    echo "--version must be an explicit release version such as 1.2.3." >&2
    exit 2
fi
if [[ "${qt_license}" != "commercial" && "${qt_license}" != "lgpl" ]]; then
    echo "--qt-license must explicitly be commercial or lgpl." >&2
    exit 2
fi
if [[ "${qt_license}" == "lgpl" && -z "${qt_source_url}" ]]; then
    echo "LGPL Qt releases require --qt-source-url pointing to the corresponding source controlled by the distributor." >&2
    exit 2
fi
if [[ "${rubberband_license}" != "commercial" && "${rubberband_license}" != "gpl" ]]; then
    echo "--rubberband-license must explicitly be commercial or gpl." >&2
    exit 2
fi
if [[ "${qt_license}" == "commercial" && "${JCUT_COMMERCIAL_QT_CONFIRMED:-0}" != "1" ]]; then
    echo "Set JCUT_COMMERCIAL_QT_CONFIRMED=1 only after confirming this build uses your commercial Qt entitlement." >&2
    exit 1
fi
if [[ "${rubberband_license}" == "commercial" && "${JCUT_COMMERCIAL_RUBBERBAND_CONFIRMED:-0}" != "1" ]]; then
    echo "Set JCUT_COMMERCIAL_RUBBERBAND_CONFIRMED=1 only after confirming a commercial Rubber Band license covers this release." >&2
    exit 1
fi
if [[ "${allow_dirty}" != "yes" && -n "$(git -C "${repo_root}" status --porcelain)" ]]; then
    echo "Refusing to create a release from a dirty worktree. Use --allow-dirty only for local validation." >&2
    exit 1
fi

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "Linux packaging must run on Linux." >&2
    exit 1
fi

for tool in ldd patchelf python3 tar; do
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

qt_dir="$(sed -n 's/^Qt6_DIR:PATH=//p' "${build_dir}/CMakeCache.txt" | tail -n1)"
if [[ -z "${qt_dir}" || "${qt_dir}" != */lib/cmake/Qt6 ]]; then
    echo "Unable to resolve the Qt SDK used by ${build_dir}." >&2
    exit 1
fi
qt_prefix="${qt_dir%/lib/cmake/Qt6}"
qtpaths="${qt_prefix}/bin/qtpaths"
[[ -x "${qtpaths}" ]] || qtpaths="${qt_prefix}/bin/qtpaths6"
if [[ ! -x "${qtpaths}" ]]; then
    echo "The build's Qt SDK has no qtpaths executable: ${qt_prefix}" >&2
    exit 1
fi
required_qt_version="$(tr -d '[:space:]' < "${repo_root}/.qt-version")"
actual_qt_version="$("${qtpaths}" --qt-version)"
if [[ "${actual_qt_version}" != "${required_qt_version}" ]]; then
    echo "Packaging Qt mismatch: required ${required_qt_version}, build uses ${actual_qt_version}." >&2
    exit 1
fi
qt_plugin_dir="$("${qtpaths}" --plugin-dir)"
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

cat > "${app_dir}/usr/bin/qt.conf" <<'EOF'
[Paths]
Plugins=../plugins
EOF

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
declare -a bundled_library_sources=()
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
        bundled_library_sources+=("${library_name}"$'\t'"${dependency}")
    done < <(bundle_ldd "${candidate}" | awk '
        /=> \/[^ ]+/ { print $3 }
        /^\/[^( ]+/ { print $1 }
    ')
done

doc_dir="${app_dir}/usr/share/doc/jcut"
license_dir="${doc_dir}/licenses"
mkdir -p "${license_dir}"
install -m 0644 "${repo_root}/THIRD_PARTY_NOTICES.md" "${doc_dir}/THIRD_PARTY_NOTICES.md"
install -m 0644 "${repo_root}/ffmpeg/COPYING.LGPLv2.1" "${license_dir}/FFmpeg-LGPL-2.1.txt"
install -m 0644 "${repo_root}/ffmpeg/LICENSE.md" "${license_dir}/FFmpeg-LICENSE.md"
install -m 0644 "${repo_root}/external/imgui/LICENSE.txt" "${license_dir}/Dear-ImGui-MIT.txt"
install -m 0644 "${repo_root}/external/ncnn/LICENSE.txt" "${license_dir}/ncnn-BSD-3-Clause.txt"
install -m 0644 "${repo_root}/external/nlohmann_json/LICENSE.MIT" "${license_dir}/nlohmann-json-MIT.txt"
install -m 0644 "${repo_root}/external/earcut.hpp/LICENSE" "${license_dir}/earcut-ISC.txt"
install -m 0644 "${repo_root}/rtaudio/LICENSE" "${license_dir}/RtAudio-LICENSE.txt"
if [[ "${rubberband_license}" == "gpl" ]]; then
    install -m 0644 "${repo_root}/external/rubberband/COPYING" "${license_dir}/Rubber-Band-GPL-2.0.txt"
else
    cat > "${doc_dir}/RUBBERBAND_LICENSE.txt" <<EOF
Rubber Band Library is distributed with JCut under JCut's commercial Rubber Band license.
EOF
fi
if [[ "${qt_license}" == "lgpl" ]]; then
    install -m 0644 /usr/share/common-licenses/LGPL-3 "${license_dir}/Qt-LGPL-3.0.txt"
    cat > "${doc_dir}/QT_SOURCE_OFFER.txt" <<EOF
This product uses Qt ${actual_qt_version} under LGPLv3.
The complete corresponding Qt source used for this build is available from:
${qt_source_url}
EOF
else
    cat > "${doc_dir}/QT_LICENSE.txt" <<EOF
Qt ${actual_qt_version} is distributed with JCut under JCut's commercial Qt license.
EOF
fi

printf '%s\n' "${bundled_library_sources[@]}" | sort -u > "${doc_dir}/bundled-libraries.tsv"
while IFS=$'\t' read -r library_name source_path; do
    package_name="$(dpkg-query -S "${source_path}" 2>/dev/null | head -n1 | cut -d: -f1 || true)"
    copyright_file="/usr/share/doc/${package_name}/copyright"
    if [[ -n "${package_name}" && -f "${copyright_file}" ]]; then
        install -m 0644 "${copyright_file}" "${license_dir}/${package_name}.copyright"
    fi
done < "${doc_dir}/bundled-libraries.tsv"

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

if [[ ! -f "${app_dir}/usr/lib/libQt6Core.so.6" ]] ||
   ! strings "${app_dir}/usr/lib/libQt6Core.so.6" | grep -Fx "${actual_qt_version}" >/dev/null; then
    echo "Packaged Qt Core does not identify as the build's exact Qt ${actual_qt_version}." >&2
    exit 1
fi

git_revision="$(git -C "${repo_root}" rev-parse HEAD)"
source_date_epoch="${SOURCE_DATE_EPOCH:-$(git -C "${repo_root}" show -s --format=%ct HEAD)}"
cat > "${doc_dir}/BUILD-INFO.txt" <<EOF
JCut-Version: ${package_version}
Git-Revision: ${git_revision}
Qt-Version: ${actual_qt_version}
Qt-License-Mode: ${qt_license}
Rubber-Band-License-Mode: ${rubberband_license}
Compiler: $(c++ --version | head -n1)
Source-Date-Epoch: ${source_date_epoch}
EOF
python3 "${repo_root}/scripts/generate_release_metadata.py" \
    --app-dir "${app_dir}" \
    --version "${package_version}" \
    --git-revision "${git_revision}" \
    --qt-version "${actual_qt_version}"

if [[ "${appdir_only}" != "yes" ]]; then
    rm -f -- "${archive_path}"
    tar --sort=name \
        --mtime="@${source_date_epoch}" \
        --owner=0 --group=0 --numeric-owner \
        -C "${output_dir}" -cf - "$(basename "${app_dir}")" |
        gzip -n > "${archive_path}"
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
