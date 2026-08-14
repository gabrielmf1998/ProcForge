#include "Polkit.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusMetaType>
#include <QVariantMap>
#include <QMap>

// Subject do polkit: (s a{sv}) = (kind, details).
struct PkSubject {
    QString      kind;
    QVariantMap  details;
};
Q_DECLARE_METATYPE(PkSubject)

static QDBusArgument &operator<<(QDBusArgument &arg, const PkSubject &s)
{
    arg.beginStructure();
    arg << s.kind << s.details;
    arg.endStructure();
    return arg;
}
static const QDBusArgument &operator>>(const QDBusArgument &arg, PkSubject &s)
{
    arg.beginStructure();
    arg >> s.kind >> s.details;
    arg.endStructure();
    return arg;
}

namespace Polkit {

Result check(const QString &caller, const QString &actionId, QString *errorOut)
{
    static const bool reg = []() {
        qDBusRegisterMetaType<PkSubject>();
        qDBusRegisterMetaType<QMap<QString, QString>>();
        return true;
    }();
    Q_UNUSED(reg);

    PkSubject subject;
    subject.kind = QStringLiteral("system-bus-name");
    subject.details.insert(QStringLiteral("name"), caller);

    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.PolicyKit1"),
        QStringLiteral("/org/freedesktop/PolicyKit1/Authority"),
        QStringLiteral("org.freedesktop.PolicyKit1.Authority"),
        QStringLiteral("CheckAuthorization"));

    const QMap<QString, QString> emptyDetails;
    const uint AllowUserInteraction = 1u;
    msg.setArguments({
        QVariant::fromValue(subject),
        actionId,
        QVariant::fromValue(emptyDetails),
        AllowUserInteraction,
        QString(),   // cancellation_id
    });

    // Timeout longo: o usuário pode levar tempo digitando a senha no diálogo.
    QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, 120000);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (errorOut) *errorOut = reply.errorMessage();
        return Error;
    }
    if (reply.arguments().isEmpty())
        return Error;

    // Resposta: (b is_authorized, b is_challenge, a{ss} details)
    const QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
    bool authorized = false, challenge = false;
    QMap<QString, QString> outDetails;
    arg.beginStructure();
    arg >> authorized >> challenge >> outDetails;
    arg.endStructure();

    return authorized ? Yes : No;
}

} // namespace Polkit
