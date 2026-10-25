#!/usr/bin/env bash
# Builds and runs the host preview: PNG screenshots in host/out/ plus UI checks.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
mkdir -p host/out
c++ -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-deprecated-declarations \
    -Isource -Ithird_party host/preview.cpp source/gfx.cpp source/ui.cpp -o host/out/preview
host/out/preview "$@"
