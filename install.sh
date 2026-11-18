#!/usr/bin/env bash
# ============================================================================
# ProcForge — instalador
#   curl -fsSL https://raw.githubusercontent.com/gabrielmf1998/ProcForge/main/install.sh | bash
#
# Instala as dependências (dnf, pacman ou apt), compila e instala:
#   - a GUI (procforge) e o atualizador (procforge-update) no seu usuário (~/.local, SEM root)
#   - o helper privilegiado (procforged) + polkit/D-Bus/systemd em /usr (sudo ou pkexec)
# Distros: Fedora/Nobara, Arch e derivados, Debian 13+/Ubuntu 24.10+. Em atômicos
# (Bazzite/Silverblue) use o pacote .rpm da página de Releases (rpm-ostree install).
# Depois de instalado, atualize com:  procforge-update  (ou Ajuda > Verificar atualizações)
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
[ "$(id -u)" -ne 0 ] || die "Não rode o instalador como root. Ele pede a senha só nas partes que precisam."

echo
info "ProcForge — instalador"

# Root só onde precisa (dependências e helper). sudo no terminal; pkexec se não houver sudo.
if command -v sudo >/dev/null 2>&1; then AS_ROOT=(sudo); elif command -v pkexec >/dev/null 2>&1; then AS_ROOT=(pkexec); else die "preciso de sudo ou pkexec."; fi

# --- 1. distro / gerenciador ----------------------------------------------
if command -v rpm-ostree >/dev/null 2>&1 && [ -e /run/ostree-booted ]; then
    warn "Sistema atômico (rpm-ostree) detectado: Bazzite/Silverblue/Kinoite."
    warn "Compilar da fonte aqui não é o ideal. Baixe o .rpm em:"
    warn "  https://github.com/gabrielmf1998/ProcForge/releases"
    warn "e instale com:  rpm-ostree install ./procforge-*.rpm && systemctl reboot"
    die  "instalação por fonte não suportada em atômico."
fi

# --- 2. dependências (só o que falta) --------------------------------------
if command -v dnf >/dev/null 2>&1; then
    info "Fedora/Nobara: instalando dependências com dnf"
    "${AS_ROOT[@]}" dnf install -y cmake ninja-build gcc-c++ extra-cmake-modules gettext \
        qt6-qtbase-devel kf6-kcoreaddons-devel kf6-ki18n-devel kf6-kwidgetsaddons-devel \
        polkit iproute util-linux libcap curl git
elif command -v pacman >/dev/null 2>&1; then
    info "Arch: instalando dependências com pacman (--needed: não reinstala o que já existe)"
    "${AS_ROOT[@]}" pacman -S --needed --noconfirm cmake ninja gcc extra-cmake-modules gettext \
        qt6-base kcoreaddons ki18n kwidgetsaddons polkit iproute2 util-linux libcap curl git
elif command -v apt-get >/dev/null 2>&1; then
    info "Debian/Ubuntu: instalando dependências com apt"
    "${AS_ROOT[@]}" apt-get install -y --no-install-recommends cmake ninja-build g++ \
        extra-cmake-modules gettext qt6-base-dev libkf6coreaddons-dev libkf6i18n-dev \
        libkf6widgetsaddons-dev polkitd iproute2 util-linux libcap2-bin curl git
else
    die "gerenciador de pacotes não suportado (dnf, pacman ou apt). Compile manualmente (veja o README)."
fi
ok "dependências prontas"
command -v bpftrace >/dev/null 2>&1 || warn "opcional: instale 'bpftrace' para o rastreamento eBPF por processo"

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
cmake -S "$SRC" -B "$SRC/build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DPROCFORGE_BUILD_TESTS=OFF >/dev/null
nice -n 10 cmake --build "$SRC/build" --parallel "$(nproc 2>/dev/null || echo 2)"
[ -x "$SRC/build/procforge" ] && [ -x "$SRC/build/procforged" ] || die "compilação não gerou os binários."
ok "compilado"

# --- 5. GUI no usuário (sem root) ------------------------------------------
info "Instalando a GUI em $PREFIX (sem root)…"
bash "$SRC/scripts/install-local.sh" --prefix "$PREFIX" --build "$SRC/build"
ok "GUI e atualizador (procforge-update) instalados em $PREFIX/bin"
case ":$PATH:" in *":$PREFIX/bin:"*) : ;; *) warn "adicione ao PATH:  export PATH=\"\$HOME/.local/bin:\$PATH\"" ;; esac

# --- 6. helper privilegiado (root) ----------------------------------------
info "Instalando o helper privilegiado (procforged), precisa de root…"
"${AS_ROOT[@]}" bash "$SRC/install-helper.sh"
ok "helper instalado (org.procforge.Helper1, ativado sob demanda pelo systemd)"

echo
printf "${G}==> ProcForge instalado!${N}\n"
printf "    Abra ${B}ProcForge${N} no menu, ou rode ${B}procforge${N} num terminal.\n"
printf "    Ações privilegiadas pedem sua senha pelo diálogo do polkit.\n\n"
