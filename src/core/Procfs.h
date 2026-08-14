#pragma once
#include <QString>
#include <QList>
#include <cstdint>

// Leitura crua e eficiente de /proc. Sem Qt no hot-path de parsing (buffers de pilha).
namespace procfs {

// Campos dinâmicos extraídos de /proc/<pid>/stat numa única leitura.
struct StatFields {
    QString comm;
    char    state      = '?';
    int     ppid       = 0;
    quint64 utime      = 0;   // ticks
    quint64 stime      = 0;   // ticks
    int     threads    = 0;
    quint64 starttime  = 0;   // ticks desde o boot
    quint64 vsizeBytes = 0;
    quint64 rssBytes   = 0;
};

// Um descritor de arquivo aberto por um processo (/proc/<pid>/fd/<n>).
struct FdEntry {
    int     fd = -1;
    QString target;   // alvo do readlink: /caminho, socket:[inode], anon_inode:[eventfd]...
    QString type;     // classificação legível: arquivo, socket, pipe, eventfd, memfd...
    QString flags;    // de /proc/<pid>/fdinfo: flags O_*, pos
    quint64 inode = 0;
};

// Uma região mapeada em /proc/<pid>/maps.
struct MapEntry {
    quint64 start  = 0;
    quint64 end    = 0;
    QString perms;    // rwxp
    quint64 offset = 0;
    QString path;     // caminho do arquivo mapeado, ou [heap]/[stack]/[anon]
};

QList<int>   listPids();                             // varre /proc por entradas numéricas
bool         readStat(int pid, StatFields &out);     // parse de /proc/<pid>/stat
quint64      totalCpuTicks();                        // soma da 1ª linha de /proc/stat
quint32      uidOf(int pid);                          // st_uid de /proc/<pid>
QString      cmdlineOf(int pid);                     // /proc/<pid>/cmdline (NUL -> espaço)
QString      cgroupOf(int pid);                      // caminho cgroup v2 (linha "0::/...")
bool         ioBytesOf(int pid, quint64 *total);     // read_bytes+write_bytes de /proc/<pid>/io
QString      userName(quint32 uid);                  // getpwuid com cache
long         clockTicksPerSec();                     // sysconf(_SC_CLK_TCK)
long         onlineCpus();                            // sysconf(_SC_NPROCESSORS_ONLN)

QList<FdEntry>  listFds(int pid);
QList<MapEntry> listMaps(int pid);

} // namespace procfs
