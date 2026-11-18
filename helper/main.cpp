#include "Helper.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QElapsedTimer>
#include <QTimer>

#include <cstdio>
#include <cstring>
#include <unistd.h>
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

    // Após uma atualização o binário em disco é trocado: este processo antigo
    // sai sozinho (com assinantes ou não) e o D-Bus ativa o novo na próxima
    // chamada. A GUI reassina os eventos ao ver o nome sumir do barramento.
    QElapsedTimer sinceActivity;
    sinceActivity.start();
    QObject::connect(&helper, &Helper::activity, &app, [&]() { sinceActivity.restart(); });
    QTimer selfCheck;
    selfCheck.setInterval(10000);
    QObject::connect(&selfCheck, &QTimer::timeout, &app, [&]() {
        // Os métodos rodam síncronos (polkit incluso), então aqui não há chamada
        // em andamento; ainda assim espera 3 s de silêncio para não perder uma
        // chamada que acabou de chegar na fila.
        if (sinceActivity.elapsed() < 3000)
            return;
        char buf[4096];
        const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            if (std::strstr(buf, " (deleted)")) {
                ::syslog(LOG_AUTHPRIV | LOG_INFO, "procforged: binário atualizado, saindo");
                app.quit();
            }
        }
    });
    selfCheck.start();

    ::syslog(LOG_AUTHPRIV | LOG_INFO, "procforged pronto em org.procforge.Helper1");
    return app.exec();
}
