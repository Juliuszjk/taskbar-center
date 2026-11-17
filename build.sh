#!/bin/sh
# Cross-compiles TaskbarCenter.exe for 64-bit Windows (needs mingw-w64-gcc).
set -e
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-windres src/app.rc -O coff -o build/app.res
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -Wall -Wextra -municode -mwindows -static \
  src/main.cpp build/app.res -o build/TaskbarCenter.exe \
  -lole32 -loleaut32 -lshell32 -luser32
ls -l build/TaskbarCenter.exe
