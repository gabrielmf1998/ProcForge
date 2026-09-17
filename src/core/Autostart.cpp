#include "Autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextStream>

namespace {

QString autostartPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                       + QStringLiteral("/autostart");
    return dir + QStringLiteral("/org.procforge.ProcForge.desktop");
}

} // namespace

namespace autostart {

bool isEnabled()
{
    QFile f(autostartPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString content = QString::fromUtf8(f.readAll());
    // Presença do arquivo já basta; só desliga se explicitamente marcado como
    // desabilitado (convenção do XDG Autostart Spec para "ocultar sem apagar").
    return !content.contains(QLatin1String("X-GNOME-Autostart-enabled=false"));
}

bool setEnabled(bool on)
{
    const QString path = autostartPath();
    if (!on)
        return QFile::remove(path) || !QFile::exists(path);

    QDir().mkpath(QFileInfo(path).absolutePath());

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    // Caminho do binário em uso agora (funciona tanto instalado em /usr quanto
    // em ~/.local, ou rodando direto de build/). --tray: abre só na bandeja.
    const QString exe = QCoreApplication::applicationFilePath();

    QTextStream out(&f);
    out << "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=ProcForge\n"
           "Exec=\"" << exe << "\" --tray\n"
           "Icon=procforge\n"
           "Terminal=false\n"
           "NoDisplay=false\n"
           "X-GNOME-Autostart-enabled=true\n"
           "Comment=Inicia o ProcForge minimizado na bandeja\n";
    return true;
}

} // namespace autostart
