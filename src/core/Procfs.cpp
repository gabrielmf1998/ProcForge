#include "Procfs.h"

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QStringList>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>

namespace {

// Lê um arquivo inteiro num buffer de pilha. Retorna bytes lidos (>=0) ou -1.
// Feito com open/read cru para manter o hot-path barato (sem alocação).
ssize_t readFileRaw(const char *path, char *buf, size_t bufsz)
{
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t total = 0;
    while (total < bufsz - 1) {
        ssize_t n = ::read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return -1;
        }
        if (n == 0)
            break;
        total += static_cast<size_t>(n);
    }
    ::close(fd);
    buf[total] = '\0';
    return static_cast<ssize_t>(total);
}

long g_pageSize  = ::sysconf(_SC_PAGESIZE);

} // namespace

namespace procfs {

long clockTicksPerSec() { static long v = ::sysconf(_SC_CLK_TCK);          return v; }
long onlineCpus()       { static long v = ::sysconf(_SC_NPROCESSORS_ONLN); return v; }

QList<int> listPids()
{
    QList<int> pids;
    pids.reserve(512);
    DIR *d = ::opendir("/proc");
    if (!d)
        return pids;
    struct dirent *e;
    while ((e = ::readdir(d)) != nullptr) {
        const char *n = e->d_name;
        if (n[0] < '1' || n[0] > '9')
            continue;
        bool allDigits = true;
        for (const char *p = n; *p; ++p)
            if (*p < '0' || *p > '9') { allDigits = false; break; }
        if (allDigits)
            pids.append(::atoi(n));
    }
    ::closedir(d);
    return pids;
}

bool readStat(int pid, StatFields &out)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/stat", pid);
    char buf[8192];
    ssize_t n = readFileRaw(path, buf, sizeof buf);
    if (n <= 0)
        return false;

    // comm fica entre o primeiro '(' e o ÚLTIMO ')', pois pode conter espaços/parênteses.
    char *lp = std::strchr(buf, '(');
    char *rp = std::strrchr(buf, ')');
    if (!lp || !rp || rp < lp)
        return false;
    out.comm = QString::fromUtf8(lp + 1, static_cast<int>(rp - lp - 1));

    // Campos a partir de 'state' (campo 3). Pulamos tokens irrelevantes com %*s.
    // Ordem: state ppid <5..13 pulados> utime stime <16..19 pulados> num_threads
    //        <21 pulado> starttime vsize rss(páginas)
    const char *p = rp + 2; // pula ") "
    char        state = '?';
    long        ppid = 0, num_threads = 0, rss_pages = 0;
    unsigned long utime = 0, stime = 0, vsize = 0;
    unsigned long long starttime = 0;

    int got = std::sscanf(
        p,
        "%c %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %lu %lu %*s %*s %*s %*s %ld %*s %llu %lu %ld",
        &state, &ppid, &utime, &stime, &num_threads, &starttime, &vsize, &rss_pages);
    if (got < 8)
        return false;

    out.state      = state;
    out.ppid       = static_cast<int>(ppid);
    out.utime      = utime;
    out.stime      = stime;
    out.threads    = static_cast<int>(num_threads);
    out.starttime  = starttime;
    out.vsizeBytes = vsize;
    out.rssBytes   = static_cast<quint64>(rss_pages) * static_cast<quint64>(g_pageSize);
    return true;
}

quint64 totalCpuTicks()
{
    char buf[4096];
    ssize_t n = readFileRaw("/proc/stat", buf, sizeof buf);
    if (n <= 0)
        return 0;
    // Primeira linha: "cpu  u n s idle iowait irq softirq steal guest guest_nice"
    char *nl = std::strchr(buf, '\n');
    if (nl) *nl = '\0';
    quint64 total = 0;
    const char *p = buf + 3; // pula "cpu"
    while (*p) {
        while (*p == ' ') ++p;
        if (*p < '0' || *p > '9') break;
        total += std::strtoull(p, const_cast<char **>(&p), 10);
    }
    return total;
}

quint32 uidOf(int pid)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d", pid);
    struct stat st;
    if (::stat(path, &st) != 0)
        return 0;
    return st.st_uid;
}

QString cmdlineOf(int pid)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    char buf[16384];
    ssize_t n = readFileRaw(path, buf, sizeof buf);
    if (n <= 0)
        return QString();
    // args separados por NUL -> troca por espaço (menos o terminador final).
    for (ssize_t i = 0; i < n; ++i)
        if (buf[i] == '\0') buf[i] = ' ';
    while (n > 0 && buf[n - 1] == ' ') --n;
    return QString::fromUtf8(buf, static_cast<int>(n));
}

QString cgroupOf(int pid)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/cgroup", pid);
    char buf[4096];
    if (readFileRaw(path, buf, sizeof buf) <= 0)
        return QString();
    // v2 unificado: linha "0::/caminho"
    char *p = std::strstr(buf, "0::");
    if (!p)
        return QString();
    p += 3;
    char *nl = std::strchr(p, '\n');
    if (nl) *nl = '\0';
    return QString::fromUtf8(p);
}

bool ioBytesOf(int pid, quint64 *total)
{
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/io", pid);
    char buf[2048];
    if (readFileRaw(path, buf, sizeof buf) <= 0)
        return false; // sem permissão (uid alheio) ou inexistente
    quint64 sum = 0;
    for (const char *key : {"read_bytes:", "write_bytes:"}) {
        const char *p = std::strstr(buf, key);
        if (p) sum += std::strtoull(p + std::strlen(key), nullptr, 10);
    }
    if (total) *total = sum;
    return true;
}

QString userName(quint32 uid)
{
    static QHash<quint32, QString> cache;
    static QMutex mutex;
    QMutexLocker lock(&mutex);
    auto it = cache.constFind(uid);
    if (it != cache.constEnd())
        return *it;

    QString name = QString::number(uid);
    char pbuf[4096];
    struct passwd pw;
    struct passwd *res = nullptr;
    if (::getpwuid_r(uid, &pw, pbuf, sizeof pbuf, &res) == 0 && res)
        name = QString::fromUtf8(pw.pw_name);
    cache.insert(uid, name);
    return name;
}

static QString classifyFdTarget(const QString &t)
{
    if (t.startsWith(QLatin1String("socket:")))            return QStringLiteral("socket");
    if (t.startsWith(QLatin1String("pipe:")))              return QStringLiteral("pipe");
    if (t.startsWith(QLatin1String("anon_inode:[eventfd]"))) return QStringLiteral("eventfd");
    if (t.startsWith(QLatin1String("anon_inode:[eventpoll]"))) return QStringLiteral("epoll");
    if (t.startsWith(QLatin1String("anon_inode:[signalfd]"))) return QStringLiteral("signalfd");
    if (t.startsWith(QLatin1String("anon_inode:[timerfd]")))  return QStringLiteral("timerfd");
    if (t.startsWith(QLatin1String("anon_inode:inotify")))    return QStringLiteral("inotify");
    if (t.contains(QLatin1String("memfd:")))               return QStringLiteral("memfd");
    if (t.startsWith(QLatin1String("anon_inode:")))        return QStringLiteral("anon_inode");
    if (t.startsWith(QLatin1String("/memfd:")))            return QStringLiteral("memfd");
    if (t.startsWith(QLatin1Char('/')))                    return QStringLiteral("arquivo");
    return QStringLiteral("outro");
}

QList<FdEntry> listFds(int pid)
{
    QList<FdEntry> out;
    char dirp[64];
    std::snprintf(dirp, sizeof dirp, "/proc/%d/fd", pid);
    DIR *d = ::opendir(dirp);
    if (!d)
        return out;

    struct dirent *e;
    while ((e = ::readdir(d)) != nullptr) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;
        FdEntry fe;
        fe.fd = ::atoi(e->d_name);

        char link[64];
        std::snprintf(link, sizeof link, "/proc/%d/fd/%d", pid, fe.fd);
        char tgt[4096];
        ssize_t l = ::readlink(link, tgt, sizeof tgt - 1);
        if (l >= 0) {
            tgt[l] = '\0';
            fe.target = QString::fromUtf8(tgt, static_cast<int>(l));
        }
        fe.type = classifyFdTarget(fe.target);

        // extrai inode de socket:[N] / pipe:[N] / anon_inode:[...]:[N]
        int lb = fe.target.lastIndexOf(QLatin1Char('['));
        int rb = fe.target.lastIndexOf(QLatin1Char(']'));
        if (lb >= 0 && rb > lb)
            fe.inode = fe.target.mid(lb + 1, rb - lb - 1).toULongLong();

        // flags de /proc/<pid>/fdinfo/<n>
        char fip[80];
        std::snprintf(fip, sizeof fip, "/proc/%d/fdinfo/%d", pid, fe.fd);
        char fbuf[1024];
        if (readFileRaw(fip, fbuf, sizeof fbuf) > 0) {
            QString info = QString::fromUtf8(fbuf);
            for (const QString &line : info.split(QLatin1Char('\n'))) {
                if (line.startsWith(QLatin1String("flags:"))) {
                    bool ok = false;
                    int oct = line.mid(6).trimmed().toInt(&ok, 8);
                    if (ok) {
                        QStringList f;
                        if ((oct & 03) == 0) f << QStringLiteral("O_RDONLY");
                        if ((oct & 03) == 1) f << QStringLiteral("O_WRONLY");
                        if ((oct & 03) == 2) f << QStringLiteral("O_RDWR");
                        if (oct & 04000)  f << QStringLiteral("O_NONBLOCK");
                        if (oct & 02000)  f << QStringLiteral("O_APPEND");
                        if (oct & 02000000) f << QStringLiteral("O_CLOEXEC");
                        fe.flags = f.join(QLatin1Char('|'));
                    }
                }
            }
        }
        out.append(fe);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const FdEntry &a, const FdEntry &b) { return a.fd < b.fd; });
    return out;
}

QList<MapEntry> listMaps(int pid)
{
    QList<MapEntry> out;
    char path[64];
    std::snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = ::fopen(path, "re");
    if (!f)
        return out;

    char line[8192];
    while (std::fgets(line, sizeof line, f)) {
        MapEntry m;
        char perms[8] = {0};
        char pathbuf[4096] = {0};
        unsigned long long start = 0, end = 0, off = 0;
        int got = std::sscanf(line, "%llx-%llx %7s %llx %*s %*u %4095[^\n]",
                              &start, &end, perms, &off, pathbuf);
        if (got < 4)
            continue;
        m.start  = start;
        m.end    = end;
        m.perms  = QString::fromLatin1(perms);
        m.offset = off;
        if (got >= 5) {
            QString p = QString::fromUtf8(pathbuf).trimmed();
            m.path = p;
        }
        out.append(m);
    }
    ::fclose(f);
    return out;
}

} // namespace procfs
