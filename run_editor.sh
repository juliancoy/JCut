#!/bin/bash
# Strip Snap-injected environment variables that conflict with system glibc
# This allows the editor to run from VS Code's integrated terminal (which is a Snap)
exec env -i \
    HOME="$HOME" \
    USER="$USER" \
    PATH="$PATH" \
    DISPLAY="$DISPLAY" \
    XAUTHORITY="$XAUTHORITY" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    XDG_CONFIG_DIRS="$XDG_CONFIG_DIRS" \
    XDG_DATA_DIRS="$XDG_DATA_DIRS" \
    QT_QPA_PLATFORM=xcb \
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    ./build/editor "$@"
