#!/bin/sh
# Build darkwatch.exe with llvm-mingw (Linux, macOS, or MSYS2/Git Bash on
# Windows): configure once, then build. The x86_64-w64-mingw32-* tools must
# be on PATH (or set LLVM_MINGW to llvm-mingw's folder).
#
#   sh darkwatch_game/tools/build-win.sh            -> build/darkwatch.exe
#   OUT=elsewhere sh darkwatch_game/tools/build-win.sh
#
# Generate src/recomp/gen first (tools/generate.py).
set -e
GAME=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$GAME/../build}
[ -n "$LLVM_MINGW" ] && PATH=$LLVM_MINGW/bin:$PATH
cmake -S "$GAME" -B "$OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$GAME/cmake/llvm-mingw.cmake"
# The build stamp (__DATE__ __TIME__ in the log and the launcher) is the
# time of this build, not of the last change to those two files.
touch "$GAME/src/win/win_main.c" "$GAME/src/win/launcher.c"
cmake --build "$OUT"
ls -la "$OUT/darkwatch.exe"
