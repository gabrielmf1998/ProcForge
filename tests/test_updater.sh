#!/usr/bin/env bash
# Teste ponta a ponta do procforge-update contra um servidor de releases FALSO
# (python http.server em 127.0.0.1). Cobre: --check, atualização de AppImage,
# já atualizado, SHA256 adulterado, release sem SHA256SUMS e versão pedida.
set -euo pipefail
UPDATER="$1"
T="$(mktemp -d)"
SRV_PID=""
cleanup() { [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FALHOU: $*"; exit 1; }

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
BASE="http://127.0.0.1:$PORT"
mkdir -p "$T/www/releases/tags" "$T/www/files" "$T/app"

mkapp() { printf '#!/bin/sh\necho "procforge %s"\n' "$1" > "$2"; chmod +x "$2"; }
mkrelease() {   # $1 versão  $2 com_sums(1/0)
    local v="$1" d="$T/www/files/v$1"
    mkdir -p "$d"
    mkapp "$v" "$d/ProcForge-$v-x86_64.AppImage"
    ( cd "$d" && sha256sum "ProcForge-$v-x86_64.AppImage" > SHA256SUMS )
    local sums=""
    [ "$2" = 1 ] && sums=",{\"browser_download_url\": \"$BASE/files/v$v/SHA256SUMS\"}"
    printf '{"tag_name": "v%s", "assets": [{"browser_download_url": "%s/files/v%s/ProcForge-%s-x86_64.AppImage"}%s]}' \
        "$v" "$BASE" "$v" "$v" "$sums" > "$T/www/releases/tags/v$v"
}
mkrelease 9.9.9 1
mkrelease 9.9.8 1
mkrelease 9.9.7 0
cp "$T/www/releases/tags/v9.9.9" "$T/www/releases/latest"

( cd "$T/www" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 ) >/dev/null 2>&1 &
SRV_PID=$!
for _ in $(seq 50); do curl -fs "$BASE/releases/latest" >/dev/null 2>&1 && break; sleep 0.1; done

export PROCFORGE_RELEASES_API="$BASE/releases"
export PROCFORGE_GITLAB_API="$BASE/nao-existe"
export PROCFORGE_UPDATE_NO_HELPER=1
APP="$T/app/ProcForge-x86_64.AppImage"
run() { bash "$UPDATER" --binary "$APP" --kind appimage "$@" 2>&1; }

mkapp 0.0.1 "$APP"
out="$(run --check)"; echo "$out" | grep -q "Disponível: 9.9.9" || fail "--check: $out"
echo "$out" | grep -q "Instalado: 0.0.1" || fail "--check não leu a versão instalada: $out"
[ "$(bash "$UPDATER" --binary "$APP" --detect)" = appimage ] || fail "--detect"

out="$(run --yes)"; echo "$out" | grep -q "PROCFORGE-UPDATE-OK 9.9.9" || fail "update: $out"
[ "$("$APP")" = "procforge 9.9.9" ] || fail "AppImage não foi trocado"
[ -x "$APP" ] || fail "AppImage perdeu o +x"

out="$(run --yes)"; echo "$out" | grep -q "PROCFORGE-UPDATE-UPTODATE 9.9.9" || fail "já atualizado: $out"

out="$(run --yes --version 9.9.8)"; echo "$out" | grep -q "PROCFORGE-UPDATE-OK 9.9.8" || fail "--version: $out"
[ "$("$APP")" = "procforge 9.9.8" ] || fail "--version não instalou 9.9.8"

# SHA256 adulterado: tem que recusar e não tocar no binário.
echo "0000000000000000000000000000000000000000000000000000000000000000  ProcForge-9.9.9-x86_64.AppImage" \
    > "$T/www/files/v9.9.9/SHA256SUMS"
if out="$(run --yes)"; then fail "aceitou SHA256 errado: $out"; fi
echo "$out" | grep -q "NÃO confere" || fail "mensagem de SHA errado: $out"
[ "$("$APP")" = "procforge 9.9.8" ] || fail "binário foi alterado apesar do SHA errado"

# Release sem SHA256SUMS: recusa.
if out="$(run --yes --version 9.9.7)"; then fail "aceitou release sem SHA256SUMS: $out"; fi
echo "$out" | grep -q "não publica SHA256SUMS" || fail "mensagem sem SUMS: $out"

# GitHub fora do ar e GitLab também: erro claro.
if out="$(PROCFORGE_RELEASES_API="$BASE/fora" run --check)"; then fail "não detectou falha de rede"; fi
echo "$out" | grep -q "não consegui consultar" || fail "mensagem de rede: $out"

# Fallback GitLab (formato direct_asset_url).
mkdir -p "$T/www/gl"
printf '[{"tag_name": "v9.9.9", "assets": {"links": [{"direct_asset_url": "%s/files/v9.9.9/x"}]}}]' "$BASE" > "$T/www/gl/index.html"
out="$(PROCFORGE_RELEASES_API="$BASE/fora" PROCFORGE_GITLAB_API="$BASE/gl/" run --check)"
echo "$out" | grep -q "Disponível: 9.9.9" || fail "fallback GitLab: $out"

# Recusa rodar como root é verificada só por leitura do script (sem root no teste).
grep -q 'id -u)" -ne 0' "$UPDATER" || fail "sem trava de root"
echo "updater: todos os cenários OK"
