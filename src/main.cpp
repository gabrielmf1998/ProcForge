#include "MainWindow.h"
#include "core/ProcInfo.h"
#include "core/Procfs.h"
#include "core/ThemeManager.h"
#include "panels/ProcessProperties.h"

#include <QApplication>
#include <QIcon>
#include <QSettings>
#include <QTimer>
#include <KLocalizedString>
#include <cstring>
#include <unistd.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("procforge"));
    QApplication::setApplicationDisplayName(QStringLiteral("ProcForge"));
    QApplication::setApplicationVersion(QStringLiteral("0.4.0"));
    QApplication::setDesktopFileName(QStringLiteral("org.procforge.ProcForge"));

    QApplication::setOrganizationName(QStringLiteral("ProcForge"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/procforge.png")));
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("procforge"));

    // --tray: usado pela entrada de autostart (Iniciar com o sistema) para
    // abrir só na bandeja. Sem isso, a 1ª janela fechada mataria o app
    // inteiro mesmo com o ícone na bandeja vivo — por isso o flag abaixo.
    bool startHidden = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--tray") == 0) startHidden = true;
    QApplication::setQuitOnLastWindowClosed(false);

    // Idioma escolhido pelo usuário (antes de construir a UI).
    {
        const QString lang = QSettings().value(QStringLiteral("language"),
                                               QStringLiteral("system")).toString();
        if (lang == QLatin1String("en"))
            KLocalizedString::setLanguages({QStringLiteral("en")});
        else if (lang == QLatin1String("pt_BR"))
            KLocalizedString::setLanguages({QStringLiteral("pt_BR")});
    }

    // Tema salvo (System = Breeze do Plasma).
    theme::init();
    {
        const int th = QSettings().value(QStringLiteral("theme"), int(theme::System)).toInt();
        if (th != int(theme::System))
            theme::apply(theme::Theme(th));
    }

    qRegisterMetaType<ProcInfo>("ProcInfo");
    qRegisterMetaType<QList<ProcInfo>>("QList<ProcInfo>");

    MainWindow w;
    if (!startHidden)
        w.show();

    // Gancho de teste: PROCFORGE_TEST_PROPS=<pid|self> abre a janela de Propriedades.
    const QByteArray tp = qgetenv("PROCFORGE_TEST_PROPS");
    if (!tp.isEmpty()) {
        const int pid = (tp == "self") ? int(::getpid()) : tp.toInt();
        procfs::StatFields st;
        procfs::readStat(pid, st);
        QTimer::singleShot(400, [pid, st]() {
            (new ProcessProperties(pid, st.starttime, st.comm))->show();
        });
    }
    return app.exec();
}
