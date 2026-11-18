#pragma once
#include <QDialog>
#include <QObject>
#include <QPointer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QProcess;
class QTextBrowser;
class QTimer;
class QMainWindow;
class TrayIcon;
class UpdateChecker;

// Diálogo de atualização: mostra a versão nova e as notas da release, e
// "Atualizar agora" roda o procforge-update (que detecta AppImage, pacote
// pacman/rpm/deb ou instalação pelo código-fonte e confere o SHA256 de tudo).
class UpdateDialog : public QDialog {
    Q_OBJECT
public:
    UpdateDialog(const QString &version, const QString &url, const QString &notes,
                 QWidget *parent = nullptr);
    static QString updaterPath();      // procforge-update ao lado do binário ou no PATH
    static QString selfExecutable();   // $APPIMAGE quando roda como AppImage
protected:
    void reject() override;            // não fecha no meio da atualização
private:
    void startUpdate();
    void restartApp();

    QString         m_version, m_url;
    QByteArray      m_output;   // saída crua do procforge-update
    QLabel         *m_status = nullptr;
    QTextBrowser   *m_notes = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton    *m_update = nullptr;
    QPushButton    *m_open = nullptr;
    QPushButton    *m_close = nullptr;
    QProcess       *m_proc = nullptr;
    bool            m_done = false;
};

// Agenda as checagens (8 s depois de abrir e a cada 12 h, se ligado nas
// Opções), avisa pela bandeja e abre o diálogo.
class UpdateManager : public QObject {
    Q_OBJECT
public:
    UpdateManager(QMainWindow *window, TrayIcon *tray);
    void checkNow(bool manual);
    void applySettings();              // relê "autoUpdateCheck"
    static bool autoCheckEnabled();
private:
    void openDialog(const QString &version, const QString &url, const QString &notes);

    QMainWindow   *m_window = nullptr;
    TrayIcon      *m_tray = nullptr;
    UpdateChecker *m_checker = nullptr;
    QTimer        *m_timer = nullptr;
    bool           m_manual = false;
    QString        m_notified;         // versão já avisada nesta sessão
    QString        m_pendingVersion, m_pendingUrl, m_pendingNotes;
    QPointer<UpdateDialog> m_dialog;
};
