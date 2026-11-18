#!/usr/bin/env bash
# Instala a GUI do ProcForge (sem root) num prefixo do usuário, a partir de uma
# pasta de compilação. Usado pelo install.sh e pelo procforge-update.
#   scripts/install-local.sh [--prefix ~/.local] [--build build]
set -euo pipefail

SRC="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${HOME}/.local"
BUILD="$SRC/build"
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift ;;
        --build)  BUILD="$2"; shift ;;
        *) echo "opção desconhecida: $1" >&2; exit 2 ;;
    esac
    shift
done
[ "$(id -u)" -ne 0 ] || { echo "rode como seu usuário (a GUI não é instalada como root)" >&2; exit 1; }
[ -x "$BUILD/procforge" ] || { echo "não achei $BUILD/procforge; compile antes" >&2; exit 1; }

# Copia para um temporário e troca com mv: o binário em uso não é sobrescrito
# no lugar (quem está rodando continua com o inode antigo).
put() {   # put <modo> <origem> <destino>
    install -d "$(dirname "$3")"
    install -m "$1" "$2" "$3.new.$$"
    mv -f "$3.new.$$" "$3"
}

put 755 "$BUILD/procforge"               "$PREFIX/bin/procforge"
put 755 "$SRC/scripts/procforge-update"  "$PREFIX/bin/procforge-update"
put 644 "$SRC/data/org.procforge.ProcForge.desktop" "$PREFIX/share/applications/org.procforge.ProcForge.desktop"
for sz in 16 22 24 32 48 64 128 256; do
    put 644 "$SRC/data/icons/procforge-${sz}.png" "$PREFIX/share/icons/hicolor/${sz}x${sz}/apps/procforge.png"
done
if [ -f "$BUILD/en.mo" ]; then
    put 644 "$BUILD/en.mo" "$PREFIX/share/locale/en/LC_MESSAGES/procforge.mo"
fi
# O hicolor do usuário precisa de um index.theme, senão o gtk-update-icon-cache
# falha. Ele tem precedência sobre o do sistema, então precisa ser o índice
# COMPLETO do sistema (um índice mínimo quebraria os ícones do desktop).
if [ ! -f "$PREFIX/share/icons/hicolor/index.theme" ] && [ -f /usr/share/icons/hicolor/index.theme ]; then
    cp /usr/share/icons/hicolor/index.theme "$PREFIX/share/icons/hicolor/index.theme"
fi
update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
# O menu do KDE só enxerga o .desktop novo depois de um rebuild completo.
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi
echo "GUI instalada em $PREFIX/bin/procforge"
