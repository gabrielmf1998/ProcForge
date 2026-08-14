#pragma once
#include <QString>

// Manipulação de janelas de outro processo no Wayland via KWin scripting
// (org.kde.kwin.Scripting no D-Bus de sessão). Carrega um pequeno JS que acha as
// janelas cujo w.pid == pid e age sobre elas. É o caminho certo no Wayland, onde
// não há XTest/EWMH. Roda na sessão do usuário, sem privilégio.
namespace kwin {

enum class WinAction { Activate, Close, Minimize, Unminimize };

bool runAction(int pid, WinAction action, QString *err);

} // namespace kwin
