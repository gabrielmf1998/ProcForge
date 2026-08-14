#!/usr/bin/env bash
# ============================================================================
# ProcForge — instalador
#   curl -fsSL https://raw.githubusercontent.com/gabrielmf1998/ProcForge/main/install.sh | bash
#
# Instala as dependências (via dnf), compila e instala:
#   - a GUI (procforge) no seu usuário  (~/.local, SEM root)
#   - o helper privilegiado (procforged) + polkit/D-Bus/systemd em /usr (via sudo)
# Distros: Fedora / Nobara e derivados tradicionais. Em atômicos (Bazzite/Silverblue)
# use o pacote .rpm da página de Releases (rpm-ostree install).
# ============================================================================
set -euo pipefail

REPO_URL="${PROCFORGE_REPO:-https://github.com/gabrielmf1998/ProcForge.git}"
BRANCH="${PROCFORGE_BRANCH:-main}"
PREFIX="${HOME}/.local"

# --- cores / logging -------------------------------------------------------
if [ -t 1 ]; then B="\033[1m"; G="\033[1;32m"; Y="\033[1;33m"; R="\033[1;31m"; C="\033[1;36m"; N="\033[0m"; else B= G= Y= R= C= N=; fi
info() { printf "${C}==>${N} ${B}%s${N}\n" "$*"; }
ok()   { printf "${G}  ✓${N} %s\n" "$*"; }
warn() { printf "${Y}  !${N} %s\n" "$*"; }
die()  { printf "${R}==> erro:${N} %s\n" "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "ProcForge é só para Linux."
[ "$(id -u)" -ne 0 ] || die "Não rode o instalador como root. Ele pede 'sudo' só na parte do helper."

echo
info "ProcForge — instalador"

# --- 1. distro / gerenciador ----------------------------------------------
if command -v rpm-ostree >/dev/null 2>&1; then
    warn "Sistema atômico (rpm-ostree) detectado — Bazzite/Silverblue/Kinoite."
    warn "Compilar da fonte aqui não é o ideal. Baixe o .rpm em:"
    warn "  https://github.com/gabrielmf1998/ProcForge/releases"
    warn "e instale com:  rpm-ostree install ./procforge-*.rpm && systemctl reboot"
    die  "instalação por fonte não suportada em atômico."
fi
command -v dnf >/dev/null 2>&1 || die "dnf não encontrado. Este instalador é para Fedora/Nobara. Compile manualmente (veja o README)."

# --- 2. dependências -------------------------------------------------------
DEPS_BUILD=(cmake ninja-build gcc-c++ extra-cmake-modules gettext
            qt6-qtbase-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kwidgetsaddons-devel)
DEPS_RUNTIME=(polkit bpftrace iproute util-linux libcap)
info "Instalando dependências (precisa de sudo)…"
sudo dnf install -y "${DEPS_BUILD[@]}" "${DEPS_RUNTIME[@]}"
ok "dependências instaladas"
if command -v cargo >/dev/null 2>&1; then
    ok "cargo presente — o núcleo de memória será compilado em Rust"
else
    warn "sem 'cargo' — o helper usará o núcleo de memória em C++ (opcional: sudo dnf install rust cargo)"
fi

# --- 3. código-fonte -------------------------------------------------------
if [ -f "CMakeLists.txt" ] && grep -q "project(procforge" CMakeLists.txt 2>/dev/null; then
    SRC="$PWD"
    info "Usando o código-fonte em $SRC"
else
    SRC="${XDG_CACHE_HOME:-$HOME/.cache}/procforge-src"
    if [ -d "$SRC/.git" ]; then
        info "Atualizando fonte em $SRC"
        git -C "$SRC" fetch --depth 1 origin "$BRANCH" && git -C "$SRC" reset --hard "origin/$BRANCH"
    else
        info "Clonando $REPO_URL"
        rm -rf "$SRC"; git clone --depth 1 -b "$BRANCH" "$REPO_URL" "$SRC"
    fi
fi
ok "fonte pronta"

# --- 4. build --------------------------------------------------------------
info "Compilando…"
cmake -S "$SRC" -B "$SRC/build" -G Ninja >/dev/null
cmake --build "$SRC/build"
[ -x "$SRC/build/procforge" ] && [ -x "$SRC/build/procforged" ] || die "compilação não gerou os binários."
ok "compilado"

# --- 5. GUI no usuário (sem root) ------------------------------------------
info "Instalando a GUI em $PREFIX (sem root)…"
install -Dm755 "$SRC/build/procforge"                    "$PREFIX/bin/procforge"
install -Dm644 "$SRC/data/org.procforge.ProcForge.desktop" "$PREFIX/share/applications/org.procforge.ProcForge.desktop"
install -Dm644 "$SRC/data/procforge.svg"                 "$PREFIX/share/icons/hicolor/scalable/apps/procforge.svg"
# O hicolor do usuário precisa de um index.theme, senão o gtk-update-icon-cache
# falha ("No theme index file"). ATENÇÃO: ele tem precedência sobre o
# /usr/share/icons/hicolor, então precisa ser o índice COMPLETO do sistema —
# um índice mínimo sombreia o do sistema e quebra os ícones do desktop inteiro.
if [ ! -f "$PREFIX/share/icons/hicolor/index.theme" ] \
   && [ -f /usr/share/icons/hicolor/index.theme ]; then
    cp /usr/share/icons/hicolor/index.theme "$PREFIX/share/icons/hicolor/index.theme"
fi
update-desktop-database "$PREFIX/share/applications" >/dev/null 2>&1 || true
gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" >/dev/null 2>&1 || true
# o menu do KDE só enxerga o .desktop depois que o ksycoca é reconstruído
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi
# traduções (en_US); pt-BR é o idioma base
[ -x "$SRC/build-i18n.sh" ] && bash "$SRC/build-i18n.sh" >/dev/null 2>&1 && ok "traduções instaladas" || true
ok "GUI instalada em $PREFIX/bin/procforge"
case ":$PATH:" in *":$PREFIX/bin:"*) : ;; *) warn "adicione ao PATH:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;; esac

# --- 6. helper privilegiado (root) ----------------------------------------
info "Instalando o helper privilegiado (procforged) — precisa de sudo…"
sudo bash "$SRC/install-helper.sh"
ok "helper instalado (org.procforge.Helper1, ativado sob demanda pelo systemd)"

echo
printf "${G}==> ProcForge instalado!${N}\n"
printf "    Abra ${B}ProcForge${N} no menu, ou rode ${B}procforge${N} num terminal.\n"
printf "    Ações privilegiadas pedem sua senha pelo diálogo do polkit.\n\n"
