#!/usr/bin/env bash
# Compila e roda o ProcForge em Wayland puro.
set -e
cd "$(dirname "$0")"
cmake -B build -G Ninja >/dev/null
cmake --build build
exec env QT_QPA_PLATFORM=wayland ./build/procforge "$@"
