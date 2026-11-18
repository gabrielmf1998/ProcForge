#!/usr/bin/env bash
# Instala as dependências de build (e de teste) dentro do container do CI.
# Uso: scripts/ci/deps.sh arch|fedora|debian
set -euo pipefail
case "$1" in
arch)
    pacman -Syu --noconfirm --needed base-devel cmake ninja extra-cmake-modules gettext git \
        qt6-base qt6-wayland kcoreaddons ki18n kwidgetsaddons polkit iproute2 python curl \
        procps-ng
    ;;
fedora)
    dnf install -y --setopt=install_weak_deps=False cmake ninja-build gcc-c++ extra-cmake-modules \
        gettext git qt6-qtbase-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kwidgetsaddons-devel \
        polkit iproute python3 curl procps-ng rpm-build systemd-rpm-macros
    ;;
debian)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends build-essential cmake ninja-build extra-cmake-modules \
        gettext git qt6-base-dev qt6-base-dev-tools qt6-wayland libkf6coreaddons-dev libkf6i18n-dev \
        libkf6widgetsaddons-dev polkitd iproute2 python3 curl ca-certificates procps \
        debhelper devscripts file
    ;;
*) echo "distro desconhecida: $1" >&2; exit 2 ;;
esac
