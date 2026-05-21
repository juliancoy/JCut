#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

report_dir="${1:-$repo_root/build/redundancy-report}"
mkdir -p "$report_dir"

first_party_filter='^(build|build-|external|ffbuild|ffmpeg|ffmpeg-build|ffmpeg-install|rtaudio|third_party|nv-codec-headers|CMakeFiles|Testing|__pycache__|\.git)/'
clone_ignore='**/build/**,**/build-*/**,**/external/**,**/ffbuild/**,**/ffmpeg/**,**/ffmpeg-build/**,**/ffmpeg-install/**,**/rtaudio/**,**/third_party/**,**/nv-codec-headers/**,**/CMakeFiles/**,**/Testing/**,**/__pycache__/**,**/.git/**'

echo "Writing reports to: $report_dir"

echo
echo "== Largest first-party C++ files =="
rg --files -g '*.cpp' -g '*.h' \
  | rg -v "$first_party_filter" \
  | xargs wc -l \
  | sort -n \
  | tee "$report_dir/largest-first-party-files.txt" \
  | tail -n 40

echo
echo "== Running jscpd clone scan =="
npx --yes jscpd . \
  --min-lines 10 \
  --min-tokens 80 \
  --pattern "**/*.{cpp,h,hpp,c,cc,cxx}" \
  --ignore "$clone_ignore" \
  --reporters console,json \
  --output "$report_dir/jscpd" \
  | tee "$report_dir/jscpd-console.txt"

echo
echo "== Targeted seam search =="
rg -n -F \
  -e 'QFutureWatcher' \
  -e 'loadTranscript' \
  -e 'saveTranscript' \
  -e 'vkCmdCopyImageToBuffer' \
  -e 'createWindowContainer' \
  -e 'supportedDeviceExtensions' \
  -e 'setToolTip(' \
  -e 'QJsonDocument::fromJson' \
  -g '*.cpp' -g '*.h' \
  | rg -v "$first_party_filter" \
  | tee "$report_dir/targeted-seams.txt"

echo
echo "== Done =="
echo "jscpd JSON: $report_dir/jscpd/jscpd-report.json"
