#!/usr/bin/env python3

import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_imgui_cmake_qt_boundary.py <link-interface-manifest>", file=sys.stderr)
        return 2

    manifest_path = pathlib.Path(sys.argv[1])
    if not manifest_path.is_file():
        print(f"missing ImGui link-interface manifest: {manifest_path}", file=sys.stderr)
        return 2

    manifest = manifest_path.read_text(encoding="utf-8")
    required_targets = {
        "jcut_ai_gateway_core",
        "jcut_frame_payload_core",
        "jcut_transcript_document_core",
        "jcut_media_path_core",
        "jcut_editor_core",
        "jcut_vulkan_frame_import_core",
        "jcut_imgui_vulkan_import",
        "jcut_imgui_runtime",
        "jcut_imgui_standalone_runtime",
        "jcut_imgui_audio_runtime",
        "jcut_editor_runtime",
        "jcut_runtime_support",
        "jcut_preview_decode_runtime",
    }
    missing = sorted(target for target in required_targets if f"[{target}]" not in manifest)
    if missing:
        print("manifest is missing targets: " + ", ".join(missing), file=sys.stderr)
        return 1

    if "jcut_frame_handle_qrhi_adapter" in manifest:
        print(
            "ImGui runtime target chain exposes the Qt-only FrameHandle QRhi adapter",
            file=sys.stderr,
        )
        return 1

    forbidden = sorted(
        set(
            re.findall(
                r"(?:Qt6::|libQt6)(Widgets|Network|Concurrent|GuiPrivate)",
                manifest,
            )
        )
    )
    if forbidden:
        print(
            "ImGui runtime target chain exposes forbidden Qt components: "
            + ", ".join(forbidden),
            file=sys.stderr,
        )
        return 1

    print(
        "ImGui runtime CMake interfaces do not expose Qt Widgets, Qt Network, "
        "Qt Concurrent, or the private Qt Gui target."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
