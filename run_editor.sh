#!/bin/bash
set -euo pipefail

# Strip Snap-injected environment variables that conflict with system glibc.
# Keep the desktop-session variables Qt needs to select the native platform
# plugin. Callers can still set QT_QPA_PLATFORM explicitly when an override is
# required (for example, QT_QPA_PLATFORM=xcb ./run_editor.sh).
launch_environment=(
    "HOME=$HOME"
    "USER=$USER"
    "PATH=$PATH"
)

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for variable in \
    DISPLAY \
    WAYLAND_DISPLAY \
    XAUTHORITY \
    XDG_RUNTIME_DIR \
    XDG_CONFIG_DIRS \
    XDG_DATA_DIRS \
    XDG_SESSION_TYPE \
    QT_QPA_PLATFORM \
    LD_LIBRARY_PATH
do
    if [[ -v "$variable" ]]; then
        launch_environment+=("$variable=${!variable}")
    fi
done

exec env -i "${launch_environment[@]}" "${script_dir}/build/jcut" "$@"
