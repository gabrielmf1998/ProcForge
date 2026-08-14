#pragma once
#include "core/ProcInfo.h"
#include <QObject>
#include <QHash>

class QTimer;

// Roda numa thread de fundo. A cada tick varre /proc, calcula CPU% por delta e
// emite um snapshot. Campos estáticos (uid/user/cmdline) são cacheados por
// (pid, starttime) para não reler a cada tick — custo de CPU imperceptível.
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

private:
    struct Prev {
        quint64 starttime = 0;
        quint64 procTime  = 0; // utime+stime anterior
        quint64 io        = 0; // read_bytes+write_bytes anterior
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
    qint64                  m_prevMs    = 0;
    double                  m_ncpu      = 1.0;
    double                  m_ticks     = 100.0;
};
