#pragma once
#include "core/ProcInfo.h"
#include <QObject>
#include <QHash>

class QTimer;

// Roda numa thread de fundo. A cada tick varre /proc, calcula CPU% por delta e
// emite um snapshot. Campos estáticos (uid/user/cmdline) são cacheados por
// (pid, starttime) para não reler a cada tick (custo de CPU imperceptível).
class ProcessCollector : public QObject {
    Q_OBJECT
public:
    explicit ProcessCollector(QObject *parent = nullptr);

public Q_SLOTS:
    void start(int intervalMs = 1000);
    void setInterval(int intervalMs);
    void scanOnce();

Q_SIGNALS:
    void snapshotReady(const QList<ProcInfo> &procs);
    // Uso total de CPU da máquina (0..100) medido em /proc/stat (não é a soma
    // dos processos, que perde quem nasceu e morreu entre dois ticks).
    void cpuUsage(double percent);

private:
    struct Prev {
        quint64 starttime = 0;
        quint64 procTime  = 0; // utime+stime anterior
        quint64 ioRead    = 0;
        quint64 ioWrite   = 0;
        bool    hasIo     = false;
    };
    struct Cached {
        quint64 starttime = 0;
        quint32 uid = 0;
        QString user;
        QString cmdline;
        bool    service = false;
        bool    kernel  = false;
    };

    QTimer                 *m_timer = nullptr;
    QHash<int, Prev>        m_prev;
    QHash<int, Cached>      m_cache;
    quint64                 m_prevTotal = 0;
    quint64                 m_prevIdle  = 0;
    qint64                  m_prevMs    = 0;
};
