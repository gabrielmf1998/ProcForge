#include "TrayIcon.h"
#include "Autostart.h"
#include "UpdateChecker.h"

#include <KLocalizedString>

#include <QAction>
#include <QDesktopServices>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QUrl>

TrayIcon::TrayIcon(QMainWindow *window)
    : QObject(window), m_window(window)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_updater = new UpdateChecker(this);
    connect(m_updater, &UpdateChecker::updateAvailable, this,
            [this](const QString &version, const QString &url) {
        m_pendingUrl = url;
        m_tray->showMessage(i18n("Atualização disponível"),
                            i18n("ProcForge %1 disponível — clique para abrir.", version),
                            QSystemTrayIcon::Information, 10000);
    });
    connect(m_updater, &UpdateChecker::upToDate, this, [this]() {
        if (m_manualCheck)
            m_tray->showMessage(QStringLiteral("ProcForge"),
                                i18n("Você já está na versão mais recente."),
                                QSystemTrayIcon::Information, 5000);
    });
    connect(m_updater, &UpdateChecker::checkFailed, this, [this](const QString &err) {
        if (m_manualCheck)
            m_tray->showMessage(QStringLiteral("ProcForge"),
                                i18n("Falha ao verificar atualizações: %1", err),
                                QSystemTrayIcon::Warning, 5000);
    });

    m_tray = new QSystemTrayIcon(
        QIcon::fromTheme(QStringLiteral("procforge"), QIcon(QStringLiteral(":/procforge.png"))),
        this);
    m_tray->setToolTip(QStringLiteral("ProcForge"));

    auto *menu = new QMenu();
    connect(menu->addAction(i18n("Abrir ProcForge")), &QAction::triggered, this, [this]() {
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
    });
    menu->addSeparator();
    connect(menu->addAction(i18n("Verificar atualizações")), &QAction::triggered,
            this, [this]() { checkForUpdates(true); });

    m_autostartAction = menu->addAction(i18n("Iniciar com o sistema"));
    m_autostartAction->setCheckable(true);
    m_autostartAction->setChecked(autostart::isEnabled());
    connect(m_autostartAction, &QAction::toggled, this, [](bool on) {
        autostart::setEnabled(on);
    });

    menu->addSeparator();
    connect(menu->addAction(i18n("Sair")), &QAction::triggered, this, &TrayIcon::quitRequested);

    m_tray->setContextMenu(menu);

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger)
            return;
        if (m_window->isVisible()) {
            m_window->hide();
        } else {
            m_window->show();
            m_window->raise();
            m_window->activateWindow();
        }
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]() {
        if (!m_pendingUrl.isEmpty())
            QDesktopServices::openUrl(QUrl(m_pendingUrl));
    });

    m_tray->show();

    // Checagem silenciosa ao iniciar: só incomoda se houver atualização de fato.
    checkForUpdates(false);
}

void TrayIcon::checkForUpdates(bool manual)
{
    m_manualCheck = manual;
    m_updater->checkAsync();
}
