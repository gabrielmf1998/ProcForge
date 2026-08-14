#pragma once
#include <QString>
#include <QList>
#include <QMetaType>
#include <cstdint>

// Snapshot de um processo, montado a partir de /proc/<pid>/stat + estáticos cacheados.
struct ProcInfo {
    int      pid        = 0;
    int      ppid       = 0;
    quint32  uid        = 0;
    QString  name;                 // comm (2º campo do stat)
    QString  user;                 // username resolvido do uid
    char     state      = '?';     // R S D Z T t X I
    quint64  rssBytes   = 0;
    quint64  vsizeBytes = 0;
    int      threads    = 0;
    quint64  starttime  = 0;       // ticks desde o boot (identidade anti-reuso de PID)
    double   cpuPercent = 0.0;     // % do total de CPU (0..100, como o Process Hacker)
    double   ioRate     = 0.0;     // bytes/s de I/O (read+write), delta de /proc/PID/io
    bool     service    = false;   // processo de um .service do systemd (cor verde)
    bool     kernel     = false;   // kernel thread (sem cmdline)
    QString  cmdline;              // linha de comando completa (args separados por espaço)

    // Descrição legível do estado do processo.
    static QString stateText(char s);
};

Q_DECLARE_METATYPE(ProcInfo)
Q_DECLARE_METATYPE(QList<ProcInfo>)
