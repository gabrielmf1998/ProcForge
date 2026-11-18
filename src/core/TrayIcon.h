#pragma once
#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QAction;
class QMainWindow;

// Ícone na bandeja do sistema: reabrir a janela, checar atualização e
// alternar "iniciar com o sistema" (autostart XDG). Quando disponível, a
// janela principal minimiza para cá em vez de fechar (MainWindow::closeEvent);
// o "Sair" de verdade fica no menu do tray e no menu Hacker.
class TrayIcon : public QObject {
    Q_OBJECT
public:
    explicit TrayIcon(QMainWindow *window);

    bool isSupported() const { return m_tray != nullptr; }
    void notify(const QString &title, const QString &text);
    void setStatus(double cpuPercent, quint64 memUsed, quint64 memTotal);

Q_SIGNALS:
    void quitRequested();
    void checkUpdatesRequested();
    void notificationClicked();

private:
    QMainWindow      *m_window = nullptr;
    QSystemTrayIcon  *m_tray = nullptr;
    QAction          *m_autostartAction = nullptr;
};
