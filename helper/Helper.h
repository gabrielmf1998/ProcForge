#pragma once
#include <QObject>
#include <QDBusContext>
#include <QList>
#include <QByteArray>
#include <QSet>

class ProcConnector;
class QDBusServiceWatcher;

// Serviço privilegiado org.procforge.Helper1. Cada método é guardado por uma
// action polkit distinta e valida a identidade do alvo (pid, starttime) antes
// de agir. Roda como root com caps mínimas (impostas pela unit systemd).
class Helper : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.procforge.Helper1")
public:
    explicit Helper(QObject *parent = nullptr);
    bool hasSubscriber() const;   // mantém o helper vivo enquanto houver assinante

Q_SIGNALS:
    void activity();   // reinicia o timer de idle-exit (não exportado)
    Q_SCRIPTABLE void ProcEvent(uint what, uint pid, uint ppid);  // 1=fork 2=exec 3=exit

public Q_SLOTS:
    QString    Ping();
    void       SendSignal(uint pid, qulonglong starttime, int sig);
    void       Renice(uint pid, int nice);
    void       SetAffinity(uint pid, const QList<uint> &cpus);
    QByteArray ReadMem(uint pid, qulonglong addr, uint len);
    void       WriteMem(uint pid, qulonglong addr, const QByteArray &bytes);
    void       InjectLibrary(uint pid, qulonglong starttime, const QString &path);
    void       CgroupThrottle(uint pid, qulonglong starttime, int cpuPercent,
                              qulonglong memMaxBytes, qulonglong pidsMax, qulonglong ioMaxBps);
    void       CgroupRelease(uint pid, qulonglong starttime);
    void       SetScheduler(uint pid, qulonglong starttime, int policy, int rtPriority);
    void       SetIoPrio(uint pid, qulonglong starttime, int ioClass, int prio);
    void       SetRlimit(uint pid, qulonglong starttime, int resource,
                         qulonglong soft, qulonglong hard);
    QString    NsRun(uint pid, qulonglong starttime, const QString &program,
                     const QStringList &args);
    QString    BpfTrace(uint pid, qulonglong starttime, const QString &mode, int seconds);
    void       SubscribeProcEvents();     // eventos fork/exec/exit em push (cn_proc)
    void       UnsubscribeProcEvents();

private Q_SLOTS:
    void onSubscriberGone(const QString &name);

private:
    bool authorize(const QString &action);
    bool identityOk(uint pid, qulonglong starttime);
    void audit(const QString &action, uint pid, const QString &result);

    ProcConnector       *m_conn = nullptr;
    QDBusServiceWatcher *m_watcher = nullptr;
    QSet<QString>        m_subscribers;
};
