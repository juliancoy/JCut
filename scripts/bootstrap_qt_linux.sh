#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/.." && pwd)"
qt_version="$(tr -d '[:space:]' < "${repo_root}/.qt-version")"
qt_root="${repo_root}/.deps/qt"
qt_prefix="${qt_root}/${qt_version}/gcc_64"
aqt_venv="${repo_root}/.deps/aqtinstall-3.3.0"

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "The automatic Qt bootstrap currently supports Linux only." >&2
    exit 1
fi
if [[ -x "${qt_prefix}/bin/qtpaths" ]] || [[ -x "${qt_prefix}/bin/qtpaths6" ]]; then
    printf '%s\n' "${qt_prefix}"
    exit 0
fi

command -v python3 >/dev/null 2>&1 || {
    echo "python3 is required to bootstrap Qt ${qt_version}." >&2
    exit 1
}

mkdir -p "${repo_root}/.deps"
if [[ ! -x "${aqt_venv}/bin/aqt" ]]; then
    python3 -m venv "${aqt_venv}"
    "${aqt_venv}/bin/pip" install --disable-pip-version-check "aqtinstall==3.3.0"
fi

echo "Installing pinned Qt ${qt_version} SDK into ${qt_root}..."
"${aqt_venv}/bin/aqt" install-qt \
    linux desktop "${qt_version}" linux_gcc_64 \
    -O "${qt_root}"

qtpaths="${qt_prefix}/bin/qtpaths"
[[ -x "${qtpaths}" ]] || qtpaths="${qt_prefix}/bin/qtpaths6"
if [[ ! -x "${qtpaths}" ]]; then
    echo "Qt bootstrap completed without the expected qtpaths executable." >&2
    exit 1
fi
installed_version="$("${qtpaths}" --qt-version)"
if [[ "${installed_version}" != "${qt_version}" ]]; then
    echo "Qt bootstrap version mismatch: expected ${qt_version}, got ${installed_version}." >&2
    exit 1
fi

printf '%s\n' "${qt_prefix}"
