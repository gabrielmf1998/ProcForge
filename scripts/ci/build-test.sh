#!/usr/bin/env bash
# Compila com avisos como erro e roda a suíte inteira (ctest) como usuário comum
# (o atualizador recusa root e as ações testam o caminho sem privilégio).
set -euo pipefail
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-Werror"
cmake --build build --parallel "$(nproc)"
id ci >/dev/null 2>&1 || useradd -m ci
chown -R ci: .
runuser -u ci -- env QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure --timeout 180
