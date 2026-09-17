#include "UpdateChecker.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QVersionNumber>

namespace {

// gabrielmf1998/ProcForge no GitHub; gabriel17166/ProcForge no GitLab (README).
constexpr char kGithubApi[] = "https://api.github.com/repos/gabrielmf1998/ProcForge/releases/latest";
constexpr char kGitlabApi[] = "https://gitlab.com/api/v4/projects/gabriel17166%2FProcForge/releases";

QVersionNumber parseVer(QString s)
{
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s.remove(0, 1);
    return QVersionNumber::fromString(s);
}

} // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void UpdateChecker::checkAsync()
{
    m_pending = 2;
    m_bestVersion.clear();
    m_bestUrl.clear();
    m_lastError.clear();

    QNetworkRequest ghReq{QUrl(QString::fromLatin1(kGithubApi))};
    ghReq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ProcForge"));
    QNetworkReply *ghReply = m_nam->get(ghReq);
    connect(ghReply, &QNetworkReply::finished, this, [this, ghReply]() {
        onReply(ghReply, QStringLiteral("github"));
        ghReply->deleteLater();
    });

    QNetworkRequest glReq{QUrl(QString::fromLatin1(kGitlabApi))};
    glReq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ProcForge"));
    QNetworkReply *glReply = m_nam->get(glReq);
    connect(glReply, &QNetworkReply::finished, this, [this, glReply]() {
        onReply(glReply, QStringLiteral("gitlab"));
        glReply->deleteLater();
    });
}

void UpdateChecker::onReply(QNetworkReply *reply, const QString &source)
{
    if (reply->error() == QNetworkReply::NoError) {
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QString tag, url;
        if (source == QLatin1String("github") && doc.isObject()) {
            tag = doc.object().value(QStringLiteral("tag_name")).toString();
            url = doc.object().value(QStringLiteral("html_url")).toString();
        } else if (source == QLatin1String("gitlab") && doc.isArray() && !doc.array().isEmpty()) {
            const QJsonObject rel = doc.array().first().toObject();
            tag = rel.value(QStringLiteral("tag_name")).toString();
            url = rel.value(QStringLiteral("_links")).toObject()
                     .value(QStringLiteral("self")).toString();
        }
        if (!tag.isEmpty() && (m_bestVersion.isEmpty() || parseVer(tag) > parseVer(m_bestVersion))) {
            m_bestVersion = tag;
            m_bestUrl = url;
        }
    } else {
        m_lastError = reply->errorString();
    }
    finishOne();
}

void UpdateChecker::finishOne()
{
    if (--m_pending > 0)
        return;

    if (m_bestVersion.isEmpty()) {
        Q_EMIT checkFailed(m_lastError.isEmpty()
                                ? QStringLiteral("Não consegui consultar GitHub/GitLab.")
                                : m_lastError);
        return;
    }

    const QVersionNumber cur    = parseVer(QCoreApplication::applicationVersion());
    const QVersionNumber latest = parseVer(m_bestVersion);
    if (latest > cur)
        Q_EMIT updateAvailable(m_bestVersion, m_bestUrl);
    else
        Q_EMIT upToDate();
}
