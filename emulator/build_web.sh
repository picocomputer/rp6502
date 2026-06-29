#!/usr/bin/env bash
#
# Copyright (c) 2026 Rumbledethumps
# SPDX-License-Identifier: BSD-3-Clause
#
# Build the WebAssembly demo into build/web/ — the full deployable web root
# (index.html + rp6502.js + rp6502.wasm).
#
# Self-contained: it uses the project's OWN Emscripten SDK — the vendor/emsdk
# submodule, pinned to emscripten-version.txt — fetching the toolchain
# INTO that submodule on first run. Nothing is assumed in your home directory
# and no EMSDK variable is needed. After the first run the "wasm" CMake preset
# works directly (CLI or VS Code), since it points at vendor/emsdk's toolchain.
#
set -euo pipefail

EMU_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$EMU_DIR/.." && pwd)"
EMSDK_DIR="$ROOT/vendor/emsdk"
EMVER="$(cat "$EMU_DIR/emscripten-version.txt")"

if [ ! -x "$EMSDK_DIR/emsdk" ]; then
    echo "error: the Emscripten SDK submodule is not checked out at '$EMSDK_DIR'." >&2
    echo "Initialize it:  git submodule update --init vendor/emsdk" >&2
    exit 1
fi

# One-time: fetch + activate the pinned toolchain into the submodule (~270 MB,
# into vendor/emsdk/ — NOT into \$HOME). Skipped once present.
if [ ! -x "$EMSDK_DIR/upstream/emscripten/emcc" ]; then
    echo "Fetching Emscripten $EMVER into $EMSDK_DIR (one-time)..."
    "$EMSDK_DIR/emsdk" install "$EMVER"
    "$EMSDK_DIR/emsdk" activate "$EMVER"
fi

for tool in cmake ninja; do
    command -v "$tool" >/dev/null 2>&1 ||
        { echo "error: '$tool' must be on PATH to build the WASM demo." >&2; exit 1; }
done

cd "$EMU_DIR"
rm -rf build/web
cmake --preset wasm
cmake --build --preset wasm

echo
echo "Deployable web root: build/web/"
ls -1 build/web
echo
echo "Preview it (must be served over HTTP — opening the file directly fails to"
echo "fetch the .wasm):"
echo "    python3 -m http.server -d build/web 8000"
echo "    # then visit http://localhost:8000/"
