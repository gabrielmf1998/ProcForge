#!/usr/bin/env bash
# Gera ProcForge-<V>-x86_64.AppImage (só a GUI e o atualizador; o helper
# privilegiado precisa de arquivos de sistema e fica de fora).
# Usa linuxdeploy + plugin Qt das releases oficiais (github.com/linuxdeploy).
set -euo pipefail
V="$1"
export APPIMAGE_EXTRACT_AND_RUN=1
TOOLS="$PWD/.appimage-tools"
mkdir -p "$TOOLS"
for t in linuxdeploy linuxdeploy-plugin-qt; do
    [ -x "$TOOLS/$t-x86_64.AppImage" ] || curl -fsSL -o "$TOOLS/$t-x86_64.AppImage" \
        "https://github.com/linuxdeploy/$t/releases/download/continuous/$t-x86_64.AppImage"
    chmod +x "$TOOLS/$t-x86_64.AppImage"
done
export PATH="$TOOLS:$PATH"

cmake -S . -B build-appimage -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DPROCFORGE_BUILD_TESTS=OFF
cmake --build build-appimage --parallel "$(nproc)"
rm -rf AppDir
DESTDIR="$PWD/AppDir" cmake --install build-appimage
# Partes do helper não funcionam de dentro de um AppImage.
rm -rf AppDir/usr/libexec AppDir/usr/share/polkit-1 AppDir/usr/share/dbus-1 AppDir/usr/lib/systemd

QMAKE="$(command -v qmake6 || echo /usr/lib/qt6/bin/qmake)"
export QMAKE
PLUG=""
for p in libqwayland-egl.so libqwayland-generic.so libqwayland.so; do
    [ -f "/usr/lib/x86_64-linux-gnu/qt6/plugins/platforms/$p" ] && PLUG="${PLUG:+$PLUG;}$p"
done
export EXTRA_PLATFORM_PLUGINS="$PLUG"
export EXTRA_QT_MODULES="waylandclient"
export LINUXDEPLOY_OUTPUT_VERSION="$V"
linuxdeploy-x86_64.AppImage --appdir AppDir --plugin qt \
    -d AppDir/usr/share/applications/org.procforge.ProcForge.desktop \
    -i AppDir/usr/share/icons/hicolor/256x256/apps/procforge.png \
    --output appimage
out="$(ls -t ProcForge*-x86_64.AppImage | head -1)"
[ "$out" = "ProcForge-$V-x86_64.AppImage" ] || mv -f "$out" "ProcForge-$V-x86_64.AppImage"
# Sanidade: o AppImage responde --version sem display.
"./ProcForge-$V-x86_64.AppImage" --version | grep -qx "procforge $V"
ls -l "ProcForge-$V-x86_64.AppImage"
