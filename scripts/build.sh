#!/usr/bin/env bash
# Builds quick-settings.ovl and stages the SD card layout in dist/sd/:
#   switch/.overlays/ovlmenu.ovl          this overlay (nx-ovlloader starts it)
#   atmosphere/contents/420000000007E51A  nx-ovlloader v2.0.2 (+ its reloader)
#   atmosphere/contents/00FF0000636C6BFF  sys-clk 2.0.1-21fix sysmodule
#   switch/.overlays/sys-clk-overlay.ovl  sys-clk's own overlay (opens from "Overlays")
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

docker run --rm -v "$ROOT":/workspace -w /workspace devkitpro/devkita64:latest \
  bash -lc 'make clean >/dev/null && make -j8'

fetch() {  # url sha256 -> path in .cache
  local url=$1 sum=$2 out=".cache/$(basename "$1")"
  mkdir -p .cache
  [ -f "$out" ] || curl -sfL -o "$out" "$url"
  echo "$sum  $out" | shasum -a 256 -c - >/dev/null || { echo "checksum mismatch: $out" >&2; exit 1; }
  echo "$out"
}
LOADER=$(fetch https://github.com/ppkantorski/nx-ovlloader/releases/download/v2.0.2/nx-ovlloader.zip \
  f06d260bcaff3aff2dd319c9e36ab4dfe87f9d012d0c1f4c6eb794e2bf2228ff)
SYSCLK=$(fetch https://github.com/retronx-team/sys-clk/releases/download/2.0.1/sys-clk-2.0.1-21fix.zip \
  2099d960644afb07564b803d5b06bd37554cb186d5bac8868da96307148a5cd7)

rm -rf dist
mkdir -p dist/sd/switch/.overlays
cp quick-settings.ovl dist/sd/switch/.overlays/ovlmenu.ovl
unzip -q "$LOADER" 'atmosphere/*' -d dist/sd
unzip -q "$SYSCLK" 'atmosphere/*' 'switch/.overlays/*' -d dist/sd
find dist/sd -type f | sort
