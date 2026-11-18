#include "TrayIcon.h"
#include "Autostart.h"

#include <KFormat>
#include <KLocalizedString>

#include <QAction>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QSystemTrayIcon>

TrayIcon::TrayIcon(QMainWindow *window)
    : QObject(window), m_window(window)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_tray = new QSystemTrayIcon(
        QIcon::fromTheme(QStringLiteral("procforge"), QIcon(QStringLiteral(":/procforge.png"))),
        this);
    m_tray->setToolTip(QStringLiteral("ProcForge"));

    auto *menu = new QMenu();
    connect(this, &QObject::destroyed, menu, &QObject::deleteLater);
    connect(menu->addAction(i18n("Abrir ProcForge")), &QAction::triggered, this, [this]() {
        m_window->show();
        m_window->raise();
        m_window->activateWindow();
    });
    menu->addSeparator();
    connect(menu->addAction(i18n("Verificar atualizações")), &QAction::triggered,
            this, &TrayIcon::checkUpdatesRequested);

    m_autostartAction = menu->addAction(i18n("Iniciar com o sistema"));
    m_autostartAction->setCheckable(true);
    m_autostartAction->setChecked(autostart::isEnabled());
    connect(m_autostartAction, &QAction::toggled, this, [](bool on) {
        autostart::setEnabled(on);
    });
    connect(menu, &QMenu::aboutToShow, this, [this]() {
        const QSignalBlocker block(m_autostartAction);   // pode ter mudado nas Opções
        m_autostartAction->setChecked(autostart::isEnabled());
    });

    menu->addSeparator();
    connect(menu->addAction(i18n("Sair")), &QAction::triggered, this, &TrayIcon::quitRequested);

    m_tray->setContextMenu(menu);

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r != QSystemTrayIcon::Trigger)
            return;
        if (m_window->isVisible() && !m_window->isMinimized()) {
            m_window->hide();
        } else {
            m_window->showNormal();
            m_window->raise();
            m_window->activateWindow();
        }
    });
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, &TrayIcon::notificationClicked);

    m_tray->show();
}

void TrayIcon::notify(const QString &title, const QString &text)
{
    if (m_tray)
        m_tray->showMessage(title, text, QSystemTrayIcon::Information, 10000);
}

void TrayIcon::setStatus(double cpuPercent, quint64 memUsed, quint64 memTotal)
{
    if (!m_tray)
        return;
    static const KFormat fmt;
    m_tray->setToolTip(i18n("ProcForge\nCPU: %1%\nMemória: %2 de %3",
                            QString::number(cpuPercent, 'f', 1),
                            fmt.formatByteSize(memUsed, 1), fmt.formatByteSize(memTotal, 1)));
}
