#pragma once
#include <QObject>
#include <QString>
#include <QVersionNumber>

class QNetworkAccessManager;
class QNetworkReply;

// Verifica a última release no GitHub e no GitLab e compara com a versão em
// execução (QApplication::applicationVersion()). Assíncrono (QNetworkAccessManager);
// nunca bloqueia a UI. As duas consultas correm em paralelo; fica com a mais nova.
class UpdateChecker : public QObject {
    Q_OBJECT
public:
    explicit UpdateChecker(QObject *parent = nullptr);

    void checkAsync();
    bool isRunning() const { return m_pending > 0; }

    // "v0.5.0" -> 0.5.0. Público para os testes.
    static QVersionNumber parseVersion(QString s);
    // latest > current, comparando como versões (0.3.0 == 0.3, 0.10 > 0.9).
    static bool isNewer(const QString &latest, const QString &current);

Q_SIGNALS:
    void updateAvailable(const QString &version, const QString &url, const QString &notes);
    void upToDate(const QString &version);
    void checkFailed(const QString &error);

private:
    void onReply(QNetworkReply *reply, const QString &source);
    void finishOne();

    QNetworkAccessManager *m_nam = nullptr;
    int     m_pending = 0;
    QString m_bestVersion;
    QString m_bestUrl;
    QString m_bestNotes;
    QString m_lastError;
};
