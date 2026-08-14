#include "HelperClient.h"

#include <KLocalizedString>

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusReply>
#include <QStringList>

namespace {

constexpr char kService[] = "org.procforge.Helper1";
constexpr char kPath[]    = "/org/procforge/Helper1";
constexpr char kIface[]   = "org.procforge.Helper1";
constexpr int  kTimeout   = 125000; // ms: permite o usuário digitar a senha

// Faz a chamada e converte o resultado/erro D-Bus em actions::Result.
actions::Result callHelper(const QString &method, const QVariantList &args)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface), method);
    msg.setArguments(args);

    const QDBusMessage reply =
        QDBusConnection::systemBus().call(msg, QDBus::Block, kTimeout);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        const QString name = reply.errorName();
        if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
            || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner")) {
            return actions::Result::fail(
                i18n("Helper privilegiado não instalado. Rode: ./install-helper.sh"));
        }
        if (name == QLatin1String("org.freedesktop.PolicyKit1.Error.NotAuthorized")
            || reply.errorMessage().contains(QLatin1String("Não autorizado"))) {
            return actions::Result::fail(i18n("Autorização negada pelo polkit."));
        }
        return actions::Result::fail(reply.errorMessage());
    }
    return actions::Result::good();
}

} // namespace

namespace helper {

bool available()
{
    auto *iface = QDBusConnection::systemBus().interface();
    if (!iface)
        return false;
    if (iface->isServiceRegistered(QLatin1String(kService)))
        return true;
    const QDBusReply<QStringList> act = iface->call(QStringLiteral("ListActivatableNames"));
    return act.isValid() && act.value().contains(QLatin1String(kService));
}

actions::Result sendSignal(int pid, quint64 starttime, int sig)
{
    return callHelper(QStringLiteral("SendSignal"),
                      { uint(pid), qulonglong(starttime), int(sig) });
}

actions::Result renice(int pid, int nice)
{
    return callHelper(QStringLiteral("Renice"), { uint(pid), int(nice) });
}

actions::Result setAffinity(int pid, const QList<int> &cpus)
{
    QList<uint> u;
    u.reserve(cpus.size());
    for (int c : cpus)
        u.append(uint(c));
    return callHelper(QStringLiteral("SetAffinity"),
                      { uint(pid), QVariant::fromValue(u) });
}

QByteArray readMem(int pid, quint64 addr, int len, bool *ok)
{
    if (ok) *ok = false;
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("ReadMem"));
    msg.setArguments({ uint(pid), qulonglong(addr), uint(len) });
    const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, kTimeout);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
        return {};
    if (ok) *ok = true;
    return reply.arguments().at(0).toByteArray();
}

actions::Result writeMem(int pid, quint64 addr, const QByteArray &bytes)
{
    return callHelper(QStringLiteral("WriteMem"),
                      { uint(pid), qulonglong(addr), bytes });
}

actions::Result injectLibrary(int pid, quint64 starttime, const QString &path)
{
    return callHelper(QStringLiteral("InjectLibrary"),
                      { uint(pid), qulonglong(starttime), path });
}

actions::Result cgroupThrottle(int pid, quint64 starttime, int cpuPercent,
                               quint64 memMaxBytes, quint64 pidsMax, quint64 ioMaxBps)
{
    return callHelper(QStringLiteral("CgroupThrottle"),
                      { uint(pid), qulonglong(starttime), int(cpuPercent),
                        qulonglong(memMaxBytes), qulonglong(pidsMax), qulonglong(ioMaxBps) });
}

actions::Result cgroupRelease(int pid, quint64 starttime)
{
    return callHelper(QStringLiteral("CgroupRelease"),
                      { uint(pid), qulonglong(starttime) });
}

actions::Result setScheduler(int pid, quint64 starttime, int policy, int rtPriority)
{
    return callHelper(QStringLiteral("SetScheduler"),
                      { uint(pid), qulonglong(starttime), int(policy), int(rtPriority) });
}

actions::Result setIoPrio(int pid, quint64 starttime, int ioClass, int prio)
{
    return callHelper(QStringLiteral("SetIoPrio"),
                      { uint(pid), qulonglong(starttime), int(ioClass), int(prio) });
}

actions::Result setRlimit(int pid, quint64 starttime, int resource, quint64 soft, quint64 hard)
{
    return callHelper(QStringLiteral("SetRlimit"),
                      { uint(pid), qulonglong(starttime), int(resource),
                        qulonglong(soft), qulonglong(hard) });
}

static QString callHelperStr(const QString &method, const QVariantList &args, QString *err)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface), method);
    msg.setArguments(args);
    const QDBusMessage reply = QDBusConnection::systemBus().call(msg, QDBus::Block, kTimeout);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        if (err) *err = reply.errorMessage();
        return {};
    }
    if (err) err->clear();
    return reply.arguments().isEmpty() ? QString() : reply.arguments().at(0).toString();
}

QString nsRun(int pid, quint64 starttime, const QString &program,
              const QStringList &args, QString *err)
{
    return callHelperStr(QStringLiteral("NsRun"),
                         { uint(pid), qulonglong(starttime), program, args }, err);
}

QString bpfTrace(int pid, quint64 starttime, const QString &mode, int seconds, QString *err)
{
    return callHelperStr(QStringLiteral("BpfTrace"),
                         { uint(pid), qulonglong(starttime), mode, int(seconds) }, err);
}

bool subscribeProcEvents(QObject *receiver, const char *slot)
{
    auto bus = QDBusConnection::systemBus();
    bus.connect(QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
                QStringLiteral("ProcEvent"), receiver, slot);
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("SubscribeProcEvents"));
    const QDBusMessage reply = bus.call(msg, QDBus::Block, 8000);
    return reply.type() != QDBusMessage::ErrorMessage;
}

void unsubscribeProcEvents()
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface),
        QStringLiteral("UnsubscribeProcEvents"));
    QDBusConnection::systemBus().call(msg, QDBus::NoBlock);
}

// ---- assíncrono ----

static actions::Result resultFromError(const QDBusError &e)
{
    const QString name = e.name();
    if (name == QLatin1String("org.freedesktop.DBus.Error.ServiceUnknown")
        || name == QLatin1String("org.freedesktop.DBus.Error.NameHasNoOwner"))
        return actions::Result::fail(i18n("Helper privilegiado não instalado. Rode: ./install-helper.sh"));
    if (name.contains(QLatin1String("NotAuthorized"))
        || e.message().contains(QLatin1String("Não autorizado")))
        return actions::Result::fail(i18n("Autorização negada pelo polkit."));
    return actions::Result::fail(e.message());
}

static void callAsync(const QString &method, const QVariantList &args, QObject *ctx, ResultCb cb)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface), method);
    msg.setArguments(args);
    const QDBusPendingCall pending = QDBusConnection::systemBus().asyncCall(msg, kTimeout);
    auto *watcher = new QDBusPendingCallWatcher(pending, ctx);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, ctx,
                     [cb = std::move(cb)](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        cb(reply.isError() ? resultFromError(reply.error()) : actions::Result::good());
        w->deleteLater();
    });
}

void sendSignalAsync(int pid, quint64 st, int sig, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("SendSignal"), { uint(pid), qulonglong(st), int(sig) }, ctx, std::move(cb)); }

void reniceAsync(int pid, quint64 st, int nice, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("Renice"), { uint(pid), qulonglong(st), int(nice) }, ctx, std::move(cb)); }

void setAffinityAsync(int pid, quint64 st, const QList<int> &cpus, QObject *ctx, ResultCb cb)
{
    QList<uint> u; u.reserve(cpus.size());
    for (int c : cpus) u.append(uint(c));
    callAsync(QStringLiteral("SetAffinity"), { uint(pid), qulonglong(st), QVariant::fromValue(u) }, ctx, std::move(cb));
}

void setSchedulerAsync(int pid, quint64 st, int policy, int rt, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("SetScheduler"), { uint(pid), qulonglong(st), int(policy), int(rt) }, ctx, std::move(cb)); }

void setIoPrioAsync(int pid, quint64 st, int ioClass, int prio, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("SetIoPrio"), { uint(pid), qulonglong(st), int(ioClass), int(prio) }, ctx, std::move(cb)); }

void setRlimitAsync(int pid, quint64 st, int resource, quint64 soft, quint64 hard, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("SetRlimit"), { uint(pid), qulonglong(st), int(resource), qulonglong(soft), qulonglong(hard) }, ctx, std::move(cb)); }

void injectLibraryAsync(int pid, quint64 st, const QString &path, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("InjectLibrary"), { uint(pid), qulonglong(st), path }, ctx, std::move(cb)); }

void cgroupThrottleAsync(int pid, quint64 st, int cpuPercent, quint64 memMaxBytes,
                         quint64 pidsMax, quint64 ioMaxBps, QObject *ctx, ResultCb cb)
{
    callAsync(QStringLiteral("CgroupThrottle"),
              { uint(pid), qulonglong(st), int(cpuPercent), qulonglong(memMaxBytes),
                qulonglong(pidsMax), qulonglong(ioMaxBps) }, ctx, std::move(cb));
}

void cgroupReleaseAsync(int pid, quint64 st, QObject *ctx, ResultCb cb)
{ callAsync(QStringLiteral("CgroupRelease"), { uint(pid), qulonglong(st) }, ctx, std::move(cb)); }

static void callAsyncStr(const QString &method, const QVariantList &args, QObject *ctx, StrCb cb, int timeout)
{
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QLatin1String(kService), QLatin1String(kPath), QLatin1String(kIface), method);
    msg.setArguments(args);
    const QDBusPendingCall pending = QDBusConnection::systemBus().asyncCall(msg, timeout);
    auto *watcher = new QDBusPendingCallWatcher(pending, ctx);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, ctx,
                     [cb = std::move(cb)](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<QString> reply = *w;
        if (reply.isError()) cb(QString(), resultFromError(reply.error()).error);
        else                 cb(reply.value(), QString());
        w->deleteLater();
    });
}

void bpfTraceAsync(int pid, quint64 st, const QString &mode, int seconds, QObject *ctx, StrCb cb)
{
    // timeout do D-Bus > duração do trace (+ margem p/ o polkit).
    callAsyncStr(QStringLiteral("BpfTrace"),
                 { uint(pid), qulonglong(st), mode, int(seconds) }, ctx, std::move(cb),
                 (seconds + 120) * 1000);
}

void nsRunAsync(int pid, quint64 st, const QString &program, const QStringList &args, QObject *ctx, StrCb cb)
{
    callAsyncStr(QStringLiteral("NsRun"),
                 { uint(pid), qulonglong(st), program, args }, ctx, std::move(cb), kTimeout);
}

} // namespace helper
