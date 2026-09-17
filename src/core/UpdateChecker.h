#pragma once
#include <QObject>
#include <QString>

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

Q_SIGNALS:
    void updateAvailable(const QString &version, const QString &url);
    void upToDate();
    void checkFailed(const QString &error);

private:
    void onReply(QNetworkReply *reply, const QString &source);
    void finishOne();

    QNetworkAccessManager *m_nam = nullptr;
    int     m_pending = 0;
    QString m_bestVersion;
    QString m_bestUrl;
    QString m_lastError;
};
