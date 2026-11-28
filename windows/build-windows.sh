#!/usr/bin/env bash
set -euo pipefail

# Build the upstream Win32 GUI as a standalone 64-bit executable with MSYS2.
# Run from the repository root in an MSYS2 UCRT64 shell.

CXX="${CXX:-g++}"
OUT_DIR="${OUT_DIR:-dist}"
mkdir -p "$OUT_DIR"

sources=(
  src/app.cpp
  src/base/checksum.cpp
  src/base/common.cpp
  src/base/error.cpp
  src/base/file.cpp
  src/base/http.cpp
  src/base/json.cpp
  src/base/path.cpp
  src/base/pool.cpp
  src/base/regexp.cpp
  src/base/string.cpp
  src/base/thread.cpp
  src/base/utf8.cpp
  src/data.cpp
  src/frameui/controlframes.cpp
  src/frameui/fontsys.cpp
  src/frameui/frame.cpp
  src/frameui/framewnd.cpp
  src/frameui/listctrl.cpp
  src/frameui/window.cpp
  src/main.cpp
  src/ngdp.cpp
  src/pages/build.cpp
  src/pages/download.cpp
  src/pages/program.cpp
  src/pages/tags.cpp
  src/wizard.cpp
  libs/lz4/lz4.c
)

curl_libs="$(pkg-config --static --libs libcurl)"
"$CXX" -std=gnu++17 -O2 -mwindows \
  -I src -I vc13 -I libs/lz4 -I libs/zlib \
  "${sources[@]}" \
  $curl_libs -lz -llz4 \
  -lcomctl32 -lole32 -luuid -loleaut32 -lshlwapi -lws2_32 -lcrypt32 \
  -static -static-libgcc -static-libstdc++ \
  -o "$OUT_DIR/BlizzGet.exe"

echo "built $OUT_DIR/BlizzGet.exe"
