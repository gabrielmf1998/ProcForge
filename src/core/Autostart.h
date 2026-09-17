#pragma once
#include <QString>

// Entrada de autostart XDG (~/.config/autostart) — "Iniciar com o sistema" no tray.
// Fonte da verdade é o próprio arquivo .desktop; não duplicamos o estado em QSettings.
namespace autostart {

bool isEnabled();
bool setEnabled(bool on);

} // namespace autostart
