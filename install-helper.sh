#!/usr/bin/env bash
# Instala o helper privilegiado procforged (Fase 1). Rode como root:
#   pkexec ./install-helper.sh      (ou: sudo ./install-helper.sh)
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"

if [[ $EUID -ne 0 ]]; then
    echo "Precisa de root. Rode: pkexec $0" >&2
    exit 1
fi

if [[ ! -x "$DIR/build/procforged" ]]; then
    echo "build/procforged não encontrado — compile antes: cmake --build build" >&2
    exit 1
fi

install -Dm0755 "$DIR/build/procforged"                    /usr/libexec/procforged
install -Dm0644 "$DIR/data/org.procforge.helper.policy"    /usr/share/polkit-1/actions/org.procforge.helper.policy
install -Dm0644 "$DIR/data/org.procforge.Helper1.conf"     /usr/share/dbus-1/system.d/org.procforge.Helper1.conf
install -Dm0644 "$DIR/data/org.procforge.Helper1.service"  /usr/share/dbus-1/system-services/org.procforge.Helper1.service
install -Dm0644 "$DIR/data/procforged.service"             /usr/lib/systemd/system/procforged.service

systemctl daemon-reload
# encerra instância antiga p/ a próxima ativação carregar o binário novo
systemctl stop procforged 2>/dev/null || true
# recarrega config do barramento de sistema (dbus-broker no Fedora)
systemctl reload dbus 2>/dev/null || systemctl reload dbus-broker 2>/dev/null || true
# polkit relê as actions por inotify; forçamos por garantia
systemctl try-restart polkit 2>/dev/null || true

echo "OK. procforged instalado."
echo "Teste rápido:"
echo "  gdbus call --system --dest org.procforge.Helper1 \\"
echo "    --object-path /org/procforge/Helper1 --method org.procforge.Helper1.Ping"
