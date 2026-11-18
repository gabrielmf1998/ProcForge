#pragma once
// Sobe o tests/fake/fakeapp como FILHO do teste (com ptrace_scope=1 só
// descendentes podem ser lidos/escritos/injetados) e lê a linha READY.
#include "core/Procfs.h"

#include <QElapsedTimer>
#include <QHash>
#include <QProcess>
#include <QStringList>
#include <QTest>
#include <QThread>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

class FakeApp {
public:
    explicit FakeApp(const QStringList &args)
    {
        m_proc.setProgram(QStringLiteral(FAKEAPP_PATH));
        m_proc.setArguments(args);
        m_proc.setProcessChannelMode(QProcess::SeparateChannels);
        m_proc.start();
        if (!m_proc.waitForStarted(5000))
            return;
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 5000) {
            if (!m_proc.canReadLine())
                m_proc.waitForReadyRead(200);
            while (m_proc.canReadLine()) {
                const QString line = QString::fromUtf8(m_proc.readLine()).trimmed();
                if (!line.startsWith(QLatin1String("READY")))
                    continue;
                for (const QString &kv : line.mid(6).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                    const int eq = kv.indexOf(QLatin1Char('='));
                    if (eq > 0) m_kv.insert(kv.left(eq), kv.mid(eq + 1));
                }
                m_ready = true;
                procfs::StatFields st;
                if (procfs::readStat(pid(), st)) m_starttime = st.starttime;
                return;
            }
        }
    }
    ~FakeApp()
    {
        if (m_proc.state() != QProcess::NotRunning) {
            m_proc.kill();
            m_proc.waitForFinished(3000);
        }
    }

    bool    ready() const     { return m_ready; }
    int     pid() const       { return int(m_proc.processId()); }
    quint64 starttime() const { return m_starttime; }
    QString value(const QString &k) const { return m_kv.value(k); }
    QProcess &proc()          { return m_proc; }

    // Espera a próxima linha que começa com 'prefix' (ex.: "VALUE").
    QString waitLine(const QString &prefix, int ms = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms) {
            while (m_proc.canReadLine()) {
                const QString l = QString::fromUtf8(m_proc.readLine()).trimmed();
                if (l.startsWith(prefix)) return l;
            }
            m_proc.waitForReadyRead(100);
        }
        return {};
    }

    // Estado (campo 3 do stat) ou '\0' se sumiu.
    static char stateOf(int pid)
    {
        procfs::StatFields st;
        return procfs::readStat(pid, st) ? st.state : '\0';
    }

private:
    QProcess m_proc;
    QHash<QString, QString> m_kv;
    bool m_ready = false;
    quint64 m_starttime = 0;
};

// true se o pid não existe mais ou é zumbi (processo terminou).
inline bool isGone(int pid, quint64 starttime)
{
    procfs::StatFields st;
    if (!procfs::readStat(pid, st)) return true;
    if (starttime && st.starttime != starttime) return true;
    return st.state == 'Z' || st.state == 'X';
}

inline bool waitGone(int pid, quint64 starttime, int ms = 3000)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (isGone(pid, starttime)) return true;
        QTest::qWait(50);
    }
    return isGone(pid, starttime);
}

inline QList<int> descendantsOf(int root)
{
    QHash<int, int> parent;
    for (int p : procfs::listPids()) {
        procfs::StatFields st;
        if (procfs::readStat(p, st)) parent.insert(p, st.ppid);
    }
    QList<int> out, frontier{root};
    while (!frontier.isEmpty()) {
        const int cur = frontier.takeFirst();
        for (auto it = parent.cbegin(); it != parent.cend(); ++it)
            if (it.value() == cur) { out << it.key(); frontier << it.key(); }
    }
    return out;
}
