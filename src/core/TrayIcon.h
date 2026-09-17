#pragma once
#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QAction;
class QMainWindow;
class UpdateChecker;

// Ícone na bandeja do sistema: reabrir a janela, checar atualização
// (GitHub/GitLab) e alternar "iniciar com o sistema" (autostart XDG).
// Quando disponível, a janela principal minimiza para cá em vez de fechar
// (ver MainWindow::closeEvent) — o "Sair" de verdade fica só no menu do tray
// e no menu Hacker.
class TrayIcon : public QObject {
    Q_OBJECT
public:
    explicit TrayIcon(QMainWindow *window);

    bool isSupported() const { return m_tray != nullptr; }

Q_SIGNALS:
    void quitRequested();

private:
    void checkForUpdates(bool manual);

    QMainWindow      *m_window = nullptr;
    QSystemTrayIcon  *m_tray = nullptr;
    QAction          *m_autostartAction = nullptr;
    UpdateChecker    *m_updater = nullptr;
    QString           m_pendingUrl;
    bool              m_manualCheck = false;
};
