#include "KWinControl.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDateTime>
#include <QDir>
#include <QTemporaryFile>

namespace {

QString actionJs(kwin::WinAction a)
{
    switch (a) {
    case kwin::WinAction::Activate:   return QStringLiteral("w.minimized=false; workspace.activeWindow=w;");
    case kwin::WinAction::Close:      return QStringLiteral("w.closeWindow();");
    case kwin::WinAction::Minimize:   return QStringLiteral("w.minimized=true;");
    case kwin::WinAction::Unminimize: return QStringLiteral("w.minimized=false;");
    }
    return {};
}

} // namespace

namespace kwin {

bool runAction(int pid, WinAction action, QString *err)
{
    const QString js = QStringLiteral(
        "const pid=%1;\n"
        "const wins=(typeof workspace.windowList==='function')"
        "?workspace.windowList():workspace.clientList();\n"
        "for(const w of wins){ if(w.pid===pid){ %2 } }\n").arg(pid).arg(actionJs(action));

    QTemporaryFile f(QDir::tempPath() + QStringLiteral("/procforge_kwin_XXXXXX.js"));
    if (!f.open()) { if (err) *err = QStringLiteral("não criei o arquivo temporário do script"); return false; }
    f.write(js.toUtf8());
    f.flush();

    const QString plugin =
        QStringLiteral("procforge_%1_%2").arg(pid).arg(QDateTime::currentMSecsSinceEpoch());
    auto bus = QDBusConnection::sessionBus();

    QDBusInterface scripting(QStringLiteral("org.kde.KWin"), QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"), bus);
    const QDBusReply<int> id = scripting.call(QStringLiteral("loadScript"), f.fileName(), plugin);
    if (!id.isValid()) {
        if (err) *err = scripting.lastError().message().isEmpty()
                            ? QStringLiteral("KWin não aceitou o script")
                            : scripting.lastError().message();
        return false;
    }
    QDBusInterface script(QStringLiteral("org.kde.KWin"),
                          QStringLiteral("/Scripting/Script%1").arg(id.value()),
                          QStringLiteral("org.kde.kwin.Script"), bus);
    script.call(QStringLiteral("run"));
    scripting.call(QStringLiteral("unloadScript"), plugin);
    return true;
}

} // namespace kwin
