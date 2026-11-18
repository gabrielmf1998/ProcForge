#include "UpdateChecker.h"

#include <KLocalizedString>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace {

// gabrielmf1998/ProcForge no GitHub; gabriel17166/ProcForge no GitLab (README).
// PROCFORGE_RELEASES_API / PROCFORGE_GITLAB_API trocam as URLs (testes).
QString githubApi()
{
    const QByteArray env = qgetenv("PROCFORGE_RELEASES_API");
    return env.isEmpty() ? QStringLiteral("https://api.github.com/repos/gabrielmf1998/ProcForge/releases/latest")
                         : QString::fromUtf8(env) + QStringLiteral("/latest");
}
QString gitlabApi()
{
    const QByteArray env = qgetenv("PROCFORGE_GITLAB_API");
    return env.isEmpty() ? QStringLiteral("https://gitlab.com/api/v4/projects/gabriel17166%2FProcForge/releases?per_page=1")
                         : QString::fromUtf8(env);
}

} // namespace

QVersionNumber UpdateChecker::parseVersion(QString s)
{
    s = s.trimmed();
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s.remove(0, 1);
    return QVersionNumber::fromString(s).normalized();
}

bool UpdateChecker::isNewer(const QString &latest, const QString &current)
{
    const QVersionNumber l = parseVersion(latest);
    if (l.isNull())
        return false;
    return l > parseVersion(current);
}

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

void UpdateChecker::checkAsync()
{
    if (m_pending > 0)
        return;          // já há uma consulta em andamento
    m_pending = 2;
    m_bestVersion.clear();
    m_bestUrl.clear();
    m_bestNotes.clear();
    m_lastError.clear();

    for (const auto &[url, source] : {std::pair{githubApi(), QStringLiteral("github")},
                                      std::pair{gitlabApi(), QStringLiteral("gitlab")}}) {
        QNetworkRequest req{QUrl(url)};
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ProcForge"));
        req.setTransferTimeout(20000);
        QNetworkReply *reply = m_nam->get(req);
        const QString src = source;
        connect(reply, &QNetworkReply::finished, this, [this, reply, src]() {
            onReply(reply, src);
            reply->deleteLater();
        });
    }
}

void UpdateChecker::onReply(QNetworkReply *reply, const QString &source)
{
    if (reply->error() == QNetworkReply::NoError) {
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QString tag, url, notes;
        if (source == QLatin1String("github") && doc.isObject()) {
            tag   = doc.object().value(QStringLiteral("tag_name")).toString();
            url   = doc.object().value(QStringLiteral("html_url")).toString();
            notes = doc.object().value(QStringLiteral("body")).toString();
        } else if (source == QLatin1String("gitlab") && doc.isArray() && !doc.array().isEmpty()) {
            const QJsonObject rel = doc.array().first().toObject();
            tag   = rel.value(QStringLiteral("tag_name")).toString();
            url   = rel.value(QStringLiteral("_links")).toObject()
                       .value(QStringLiteral("self")).toString();
            notes = rel.value(QStringLiteral("description")).toString();
        }
        if (!tag.isEmpty() && (m_bestVersion.isEmpty() || isNewer(tag, m_bestVersion))) {
            m_bestVersion = tag;
            m_bestUrl = url;
            m_bestNotes = notes;
        } else if (!tag.isEmpty() && m_bestNotes.isEmpty() && parseVersion(tag) == parseVersion(m_bestVersion)) {
            m_bestNotes = notes;
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
                                ? i18n("Não consegui consultar GitHub/GitLab.")
                                : m_lastError);
        return;
    }

    if (isNewer(m_bestVersion, QCoreApplication::applicationVersion()))
        Q_EMIT updateAvailable(m_bestVersion, m_bestUrl, m_bestNotes);
    else
        Q_EMIT upToDate(m_bestVersion);
}
