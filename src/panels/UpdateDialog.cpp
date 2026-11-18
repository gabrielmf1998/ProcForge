#include "UpdateDialog.h"
#include "core/TrayIcon.h"
#include "core/UpdateChecker.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

// ===================== UpdateDialog =====================

QString UpdateDialog::selfExecutable()
{
    // Guardado na 1ª chamada (feita na inicialização): depois que o updater troca
    // o binário, /proc/self/exe passa a apontar para "... (deleted)".
    static const QString path = []() {
        const QByteArray appimage = qgetenv("APPIMAGE");
        if (!appimage.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(appimage)))
            return QString::fromLocal8Bit(appimage);
        QString p = QCoreApplication::applicationFilePath();
        if (p.endsWith(QLatin1String(" (deleted)")))
            p.chop(10);
        return p;
    }();
    return path;
}

QString UpdateDialog::updaterPath()
{
    const QString local = QStandardPaths::findExecutable(QStringLiteral("procforge-update"),
                                                         {QCoreApplication::applicationDirPath()});
    if (!local.isEmpty())
        return local;
    return QStandardPaths::findExecutable(QStringLiteral("procforge-update"));
}

UpdateDialog::UpdateDialog(const QString &version, const QString &url, const QString &notes,
                           QWidget *parent)
    : QDialog(parent), m_version(version.startsWith(QLatin1Char('v')) ? version.mid(1) : version), m_url(url)
{
    setWindowTitle(i18n("Atualização do ProcForge"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(640, 520);

    auto *v = new QVBoxLayout(this);
    auto *title = new QLabel(i18n("<h3>ProcForge %1 disponível</h3>Você está na versão %2.",
                                  m_version.toHtmlEscaped(),
                                  QCoreApplication::applicationVersion().toHtmlEscaped()), this);
    v->addWidget(title);

    m_notes = new QTextBrowser(this);
    m_notes->setOpenExternalLinks(true);
    if (notes.trimmed().isEmpty())
        m_notes->setPlainText(i18n("(a release não tem notas)"));
    else
        m_notes->setMarkdown(notes);
    v->addWidget(m_notes, 2);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_log->setVisible(false);
    v->addWidget(m_log, 2);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    v->addWidget(m_status);

    auto *bb = new QDialogButtonBox(this);
    m_update = bb->addButton(i18n("Atualizar agora"), QDialogButtonBox::AcceptRole);
    m_update->setIcon(QIcon::fromTheme(QStringLiteral("update-none")));
    m_open = bb->addButton(i18n("Abrir página da versão"), QDialogButtonBox::ActionRole);
    m_close = bb->addButton(QDialogButtonBox::Close);
    v->addWidget(bb);

    connect(m_open, &QPushButton::clicked, this, [this]() {
        if (!m_url.isEmpty()) QDesktopServices::openUrl(QUrl(m_url));
    });
    connect(m_close, &QPushButton::clicked, this, &UpdateDialog::reject);
    connect(m_update, &QPushButton::clicked, this, [this]() {
        if (m_done) restartApp(); else startUpdate();
    });

    if (updaterPath().isEmpty()) {
        m_update->setEnabled(false);
        m_status->setText(i18n("O atualizador (procforge-update) não está instalado; use a página da versão."));
    }
}

void UpdateDialog::reject()
{
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        KMessageBox::information(this, i18n("Aguarde a atualização terminar."));
        return;
    }
    QDialog::reject();
}

void UpdateDialog::startUpdate()
{
    const QString updater = updaterPath();
    if (updater.isEmpty())
        return;
    m_log->clear();
    m_log->setVisible(true);
    m_notes->setMaximumHeight(140);
    m_update->setEnabled(false);
    m_close->setEnabled(false);
    m_status->setText(i18n("Atualizando... pode aparecer um pedido de senha (polkit)."));

    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_output.clear();
    connect(m_proc, &QProcess::readyRead, this, [this]() {
        // Pedaços não terminam em fim de linha: acumula cru e insere sem quebrar.
        const QByteArray chunk = m_proc->readAll();
        m_output += chunk;
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(QString::fromUtf8(chunk));
        m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum());
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        m_close->setEnabled(true);
        const bool ok = st == QProcess::NormalExit && code == 0
                     && m_output.contains("PROCFORGE-UPDATE-OK");
        if (ok) {
            m_done = true;
            m_status->setText(i18n("<b>Atualizado para %1.</b> Reinicie o ProcForge para usar a versão nova.",
                                   m_version.toHtmlEscaped()));
            m_update->setText(i18n("Reiniciar agora"));
            m_update->setEnabled(true);
        } else if (m_output.contains("PROCFORGE-UPDATE-UPTODATE")) {
            m_status->setText(i18n("Você já está na versão mais recente."));
        } else {
            m_status->setText(i18n("<b>A atualização falhou</b> (código %1). Nada foi trocado se a "
                                   "verificação falhou; veja o log acima.", code));
            m_update->setText(i18n("Tentar de novo"));
            m_update->setEnabled(true);
        }
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        m_close->setEnabled(true);
        m_update->setEnabled(true);
        m_status->setText(i18n("Não consegui iniciar o atualizador."));
    });
    m_proc->start(updater, {QStringLiteral("--yes"),
                            QStringLiteral("--version"), m_version,
                            QStringLiteral("--binary"), selfExecutable(),
                            QStringLiteral("--current"), QCoreApplication::applicationVersion()});
}

void UpdateDialog::restartApp()
{
    QProcess::startDetached(selfExecutable(), {});
    QApplication::quit();
}

// ===================== UpdateManager =====================

bool UpdateManager::autoCheckEnabled()
{
    return QSettings().value(QStringLiteral("autoUpdateCheck"), true).toBool();
}

UpdateManager::UpdateManager(QMainWindow *window, TrayIcon *tray)
    : QObject(window), m_window(window), m_tray(tray)
{
    UpdateDialog::selfExecutable();   // fixa o caminho antes de qualquer atualização
    m_checker = new UpdateChecker(this);
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        m_timer->setInterval(12 * 60 * 60 * 1000);   // depois da primeira: a cada 12 h
        checkNow(false);
    });

    connect(m_checker, &UpdateChecker::updateAvailable, this,
            [this](const QString &tag, const QString &url, const QString &notes) {
        const QString version = tag.startsWith(QLatin1Char('v')) ? tag.mid(1) : tag;
        m_pendingVersion = version; m_pendingUrl = url; m_pendingNotes = notes;
        if (m_manual) {
            openDialog(version, url, notes);
            return;
        }
        if (m_notified == version)
            return;
        m_notified = version;
        if (m_tray && m_tray->isSupported())
            m_tray->notify(i18n("Atualização disponível"),
                           i18n("ProcForge %1 disponível. Clique para ver e atualizar.", version));
        m_window->statusBar()->showMessage(
            i18n("ProcForge %1 disponível: Ajuda › Verificar atualizações.", version), 15000);
    });
    connect(m_checker, &UpdateChecker::upToDate, this, [this](const QString &latest) {
        if (m_manual)
            KMessageBox::information(m_window, i18n("Você já está na versão mais recente (%1).",
                                                    QCoreApplication::applicationVersion()),
                                     i18n("Atualizações"));
        Q_UNUSED(latest);
    });
    connect(m_checker, &UpdateChecker::checkFailed, this, [this](const QString &err) {
        if (m_manual)
            KMessageBox::error(m_window, i18n("Falha ao verificar atualizações: %1", err));
    });
    if (m_tray)
        connect(m_tray, &TrayIcon::notificationClicked, this, [this]() {
            if (!m_pendingVersion.isEmpty())
                openDialog(m_pendingVersion, m_pendingUrl, m_pendingNotes);
        });

    applySettings();
}

void UpdateManager::applySettings()
{
    if (autoCheckEnabled()) {
        if (!m_timer->isActive()) {
            m_timer->setInterval(8000);
            m_timer->start();
        }
    } else {
        m_timer->stop();
    }
}

void UpdateManager::checkNow(bool manual)
{
    if (m_checker->isRunning()) {
        // Já há consulta em andamento: um pedido manual "promove" ela (mostra o
        // resultado); uma automática nunca silencia um pedido manual.
        m_manual = m_manual || manual;
        return;
    }
    m_manual = manual;
    m_checker->checkAsync();
}

void UpdateManager::openDialog(const QString &version, const QString &url, const QString &notes)
{
    if (m_dialog) {
        m_dialog->raise();
        m_dialog->activateWindow();
        return;
    }
    m_window->show();
    m_dialog = new UpdateDialog(version, url, notes, m_window);
    m_dialog->show();
}
