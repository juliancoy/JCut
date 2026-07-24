# JCut third-party software notices

JCut incorporates or dynamically loads third-party software. The release bundle
contains the corresponding license texts under `usr/share/doc/jcut/licenses`.
The exact files shipped in a release are recorded in `SHA256SUMS` and
`sbom.spdx.json`.

Core dependencies include:

- Qt 6 (Core, Gui, Widgets, Network, Concurrent, Test for test builds, and
  private QRhi headers at build time), under the license selected for the
  release.
- FFmpeg libraries, configured by JCut without GPL, version-3, or nonfree
  components.
- Vulkan headers and loader.
- Dear ImGui (MIT).
- ncnn (BSD 3-Clause).
- nlohmann/json (MIT).
- mapbox/earcut.hpp (ISC).
- RtAudio (MIT-style license).
- Rubber Band Library (GPL-2.0-or-later for the bundled build; commercial
  distribution requires an appropriate commercial Rubber Band license or a
  replacement compatible with the product's licensing policy).

Additional dynamically bundled system libraries are listed with their source
paths and available distribution copyright files in
`usr/share/doc/jcut/bundled-libraries.tsv`.

This notice is an engineering inventory, not legal advice. Release approval
must include review of the generated bundle and the governing commercial or
open-source license agreements.
