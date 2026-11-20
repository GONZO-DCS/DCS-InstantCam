#!/usr/bin/env bash
# Builds instantcam.dll and the Windows installer.
# Needs: mingw-w64 (g++-mingw-w64-x86-64), nsis, git, python3.
# On Ubuntu:  sudo apt-get install -y g++-mingw-w64-x86-64 nsis
set -euo pipefail
cd "$(dirname "$0")"

# OpenXR headers
if [ ! -d third_party/OpenXR-SDK ]; then
  git clone --depth 1 https://github.com/KhronosGroup/OpenXR-SDK.git third_party/OpenXR-SDK
fi

# embed the shader source
python3 - <<'PY'
src = open("src/shader.hlsl").read()
open("src/shader_src.h", "w").write('#pragma once\nstatic const char* kShaderSrc = R"HLSL(\n' + src + '\n)HLSL";\n')
PY

CXX=x86_64-w64-mingw32-g++-posix
command -v $CXX >/dev/null || CXX=x86_64-w64-mingw32-g++
$CXX -std=c++17 -O2 -s -Wall -Wno-unused-function -shared \
  -Ithird_party/OpenXR-SDK/include -o pkg/instantcam.dll src/instantcam.cpp \
  -static -static-libgcc -static-libstdc++ \
  -ld3d11 -ldxgi -lws2_32 -lwinmm -lole32 -loleaut32 -lshell32 -luuid -lwindowscodecs -lgdi32 -luser32

# tray app (settings + caption editor)
x86_64-w64-mingw32-windres res/tray.rc -O coff -o res/tray.res
$CXX -std=c++17 -O2 -s -Wall -municode -mwindows -o pkg/InstantCam.exe src/tray.cpp res/tray.res \
  -static -static-libgcc -static-libstdc++ \
  -lole32 -loleaut32 -lshell32 -luuid -lwindowscodecs -lgdi32 -luser32 -lcomctl32 -ladvapi32

mkdir -p dist
makensis -V2 installer.nsi
echo "Built: $(ls dist/*.exe)"
