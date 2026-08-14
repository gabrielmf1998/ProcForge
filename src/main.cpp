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
#include <unistd.h>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("procforge"));
    QApplication::setApplicationDisplayName(QStringLiteral("ProcForge"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));
    QApplication::setDesktopFileName(QStringLiteral("org.procforge.ProcForge"));

    QApplication::setOrganizationName(QStringLiteral("ProcForge"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/procforge.svg")));
    KLocalizedString::setApplicationDomain(QByteArrayLiteral("procforge"));

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
