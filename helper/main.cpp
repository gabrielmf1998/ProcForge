#include "Helper.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QTimer>

#include <cstdio>
#include <syslog.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::openlog("procforged", LOG_PID, LOG_AUTHPRIV);

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        std::fprintf(stderr, "procforged: sem barramento de sistema\n");
        return 1;
    }

    Helper helper;
    if (!bus.registerObject(QStringLiteral("/org/procforge/Helper1"), &helper,
                            QDBusConnection::ExportAllSlots
                                | QDBusConnection::ExportScriptableSignals)) {
        std::fprintf(stderr, "procforged: registerObject falhou\n");
        return 1;
    }
    if (!bus.registerService(QStringLiteral("org.procforge.Helper1"))) {
        std::fprintf(stderr, "procforged: registerService falhou: %s\n",
                     qPrintable(bus.lastError().message()));
        return 1;
    }

    // Idle-exit: sai após 30 s sem atividade — mas fica vivo enquanto houver
    // assinante de eventos de processo (cn_proc). D-Bus reativa sob demanda.
    QTimer idle;
    idle.setSingleShot(true);
    idle.setInterval(30000);
    QObject::connect(&idle, &QTimer::timeout, &app, [&]() {
        if (helper.hasSubscriber()) idle.start();  // re-arma; não sai
        else app.quit();
    });
    QObject::connect(&helper, &Helper::activity, &idle, qOverload<>(&QTimer::start));
    idle.start();

    ::syslog(LOG_AUTHPRIV | LOG_INFO, "procforged pronto em org.procforge.Helper1");
    return app.exec();
}
