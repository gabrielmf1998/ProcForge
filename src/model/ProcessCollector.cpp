#include "ProcessCollector.h"
#include "core/Procfs.h"

#include <QTimer>
#include <QDateTime>

ProcessCollector::ProcessCollector(QObject *parent)
    : QObject(parent)
{
}

void ProcessCollector::start(int intervalMs)
{
    if (!m_timer) {
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, &ProcessCollector::scanOnce);
    }
    m_timer->start(intervalMs);
    scanOnce();
}

void ProcessCollector::setInterval(int intervalMs)
{
    if (!m_timer)
        return;
    if (intervalMs <= 0) {
        m_timer->stop();          // 0 = parado (manual): só via scanOnce() explícito
        return;
    }
    m_timer->setInterval(intervalMs);
    if (!m_timer->isActive())
        m_timer->start(intervalMs);
}

void ProcessCollector::scanOnce()
{
    const QList<procfs::CpuTimes> cpus = procfs::cpuTimes();
    const quint64 total  = cpus.isEmpty() ? 0 : cpus.first().total;
    const quint64 idle   = cpus.isEmpty() ? 0 : cpus.first().idle;
    const quint64 dTotal = (m_prevTotal && total > m_prevTotal) ? (total - m_prevTotal) : 0;
    const quint64 dIdle  = (m_prevIdle && idle >= m_prevIdle) ? (idle - m_prevIdle) : 0;

    const qint64  nowMs   = QDateTime::currentMSecsSinceEpoch();
    const double  dtSec   = (m_prevMs && nowMs > m_prevMs) ? (nowMs - m_prevMs) / 1000.0 : 0.0;

    const QList<int> pids = procfs::listPids();

    QList<ProcInfo> out;
    out.reserve(pids.size());

    QHash<int, Prev>   newPrev;
    newPrev.reserve(pids.size());
    QHash<int, Cached> newCache;
    newCache.reserve(pids.size());

    for (int pid : pids) {
        procfs::StatFields st;
        if (!procfs::readStat(pid, st))
            continue; // processo morreu no meio da varredura

        ProcInfo pi;
        pi.pid        = pid;
        pi.ppid       = st.ppid;
        pi.name       = st.comm;
        pi.state      = st.state;
        pi.rssBytes   = st.rssBytes;
        pi.vsizeBytes = st.vsizeBytes;
        pi.threads    = st.threads;
        pi.starttime  = st.starttime;
        pi.nice       = st.nice;
        pi.priority   = st.priority;
        pi.cpuTicks   = st.utime + st.stime;

        // Estáticos: só releem se pid novo ou reusado (starttime mudou).
        auto cit = m_cache.constFind(pid);
        if (cit != m_cache.constEnd() && cit->starttime == st.starttime) {
            pi.uid     = cit->uid;
            pi.user    = cit->user;
            pi.cmdline = cit->cmdline;
            pi.service = cit->service;
            pi.kernel  = cit->kernel;
        } else {
            pi.uid     = procfs::uidOf(pid);
            pi.user    = procfs::userName(pi.uid);
            pi.cmdline = procfs::cmdlineOf(pid);
            pi.kernel  = pi.cmdline.isEmpty();
            if (pi.kernel)
                pi.cmdline = QStringLiteral("[%1]").arg(pi.name); // kernel thread
            const QString cg = procfs::cgroupOf(pid);
            pi.service = cg.contains(QLatin1String(".service")) || cg.contains(QLatin1String(".socket"));
        }
        newCache.insert(pid, Cached{st.starttime, pi.uid, pi.user, pi.cmdline, pi.service, pi.kernel});

        // CPU em % do total (0..100, como o Process Hacker).
        const quint64 procTime = st.utime + st.stime;
        quint64 ioRead = 0, ioWrite = 0;
        const bool hasIo = procfs::ioBytesOf(pid, &ioRead, &ioWrite);

        auto pit = m_prev.constFind(pid);
        if (pit != m_prev.constEnd() && pit->starttime == st.starttime) {
            if (dTotal > 0) {
                const quint64 dProc = (procTime > pit->procTime) ? (procTime - pit->procTime) : 0;
                pi.cpuPercent = qMin(100.0, 100.0 * static_cast<double>(dProc) / static_cast<double>(dTotal));
            }
            if (hasIo && pit->hasIo && dtSec > 0.0) {
                if (ioRead >= pit->ioRead)
                    pi.ioReadRate = static_cast<double>(ioRead - pit->ioRead) / dtSec;
                if (ioWrite >= pit->ioWrite)
                    pi.ioWriteRate = static_cast<double>(ioWrite - pit->ioWrite) / dtSec;
                pi.ioRate = pi.ioReadRate + pi.ioWriteRate;
            }
        }
        newPrev.insert(pid, Prev{st.starttime, procTime, ioRead, ioWrite, hasIo});

        out.append(pi);
    }

    m_prev      = std::move(newPrev);
    m_cache     = std::move(newCache);
    m_prevTotal = total;
    m_prevIdle  = idle;
    m_prevMs    = nowMs;

    // CPU total antes do snapshot: quem trata o snapshot (status, bandeja) já vê o valor deste tick.
    if (dTotal > 0)
        Q_EMIT cpuUsage(qBound(0.0, 100.0 * double(dTotal - qMin(dIdle, dTotal)) / double(dTotal), 100.0));
    Q_EMIT snapshotReady(out);
}
