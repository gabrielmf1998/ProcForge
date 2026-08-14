#include "ProcessActions.h"
#include "core/Syscalls.h"
#include "core/Procfs.h"

#include <KLocalizedString>
#include <QHash>

#include <cerrno>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>

namespace {

QString errStr(int e) { return QString::fromUtf8(::strerror(e)); }

// Confere que o PID ainda é o mesmo processo (starttime igual). 0 = não checar.
bool identityOk(int pid, quint64 starttime, QString *why)
{
    if (starttime == 0)
        return true;
    procfs::StatFields st;
    if (!procfs::readStat(pid, st)) {
        if (why) *why = i18n("O processo %1 não existe mais.", pid);
        return false;
    }
    if (st.starttime != starttime) {
        if (why) *why = i18n("O PID %1 foi reutilizado por outro processo.", pid);
        return false;
    }
    return true;
}

} // namespace

namespace actions {

Result sendSignal(int pid, quint64 starttime, int sig)
{
    QString why;
    if (!identityOk(pid, starttime, &why))
        return Result::fail(why);

    // Caminho preferido: pidfd (imune a corrida de reuso de PID).
    int pfd = sc::pidfd_open(pid, 0);
    if (pfd >= 0) {
        int r = sc::pidfd_send_signal(pfd, sig, nullptr, 0);
        int e = errno;
        ::close(pfd);
        if (r == 0)
            return Result::good();
        return Result::failErr(i18n("Falha ao enviar sinal: %1", errStr(e)), e);
    }

    // Fallback: kill() clássico.
    if (::kill(pid, sig) == 0)
        return Result::good();
    return Result::failErr(i18n("Falha ao enviar sinal: %1", errStr(errno)), errno);
}

Result suspend(int pid, quint64 starttime) { return sendSignal(pid, starttime, SIGSTOP); }
Result resume(int pid, quint64 starttime)  { return sendSignal(pid, starttime, SIGCONT); }

Result killTree(int rootPid)
{
    if (rootPid <= 1)
        return Result::fail(i18n("Recusado: não vou matar o PID %1.", rootPid));

    // Congela o topo para impedir que a árvore escape forkando durante a coleta.
    ::kill(rootPid, SIGSTOP);

    // Mapa ppid -> filhos, a partir de uma varredura fresca de /proc.
    QHash<int, QList<int>> childrenOf;
    for (int pid : procfs::listPids()) {
        procfs::StatFields st;
        if (procfs::readStat(pid, st))
            childrenOf[st.ppid].append(pid);
    }

    // BFS coletando toda a descendência (inclui a raiz).
    QList<int> order;
    QList<int> queue{rootPid};
    while (!queue.isEmpty()) {
        int pid = queue.takeFirst();
        order.append(pid);
        for (int c : childrenOf.value(pid))
            queue.append(c);
    }

    // Mata filhos antes dos pais.
    int killed = 0;
    for (int i = order.size() - 1; i >= 0; --i) {
        int pid = order.at(i);
        // descongela para o SIGKILL ser processado, depois mata.
        ::kill(pid, SIGCONT);
        if (::kill(pid, SIGKILL) == 0)
            ++killed;
    }
    return Result::good(i18n("%1 processo(s) da árvore encerrado(s).", killed));
}

Result renice(int pid, int nice)
{
    errno = 0;
    if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), nice) == 0 && errno == 0)
        return Result::good();
    return Result::failErr(i18n("Falha ao ajustar prioridade (nice %1): %2", nice, errStr(errno)), errno);
}

int getNice(int pid, bool *ok)
{
    errno = 0;
    int v = ::getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
    if (ok) *ok = (errno == 0);
    return v;
}

QList<int> onlineCpus()
{
    QList<int> l;
    long n = procfs::onlineCpus();
    for (long i = 0; i < n; ++i)
        l.append(static_cast<int>(i));
    return l;
}

QList<int> getAffinity(int pid)
{
    QList<int> l;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (::sched_getaffinity(pid, sizeof set, &set) != 0)
        return l;
    long n = procfs::onlineCpus();
    for (long i = 0; i < n; ++i)
        if (CPU_ISSET(i, &set))
            l.append(static_cast<int>(i));
    return l;
}

Result setAffinity(int pid, const QList<int> &cpus)
{
    if (cpus.isEmpty())
        return Result::fail(i18n("Selecione ao menos uma CPU."));
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus)
        CPU_SET(c, &set);
    if (::sched_setaffinity(pid, sizeof set, &set) == 0)
        return Result::good();
    return Result::failErr(i18n("Falha ao definir afinidade: %1", errStr(errno)), errno);
}

Result setScheduler(int pid, int policy, int rtPriority)
{
    struct sched_param p;
    p.sched_priority = (policy == SCHED_FIFO || policy == SCHED_RR) ? rtPriority : 0;
    if (::sched_setscheduler(pid, policy, &p) == 0)
        return Result::good();
    return Result::failErr(i18n("Falha ao definir escalonador: %1", errStr(errno)), errno);
}

int getScheduler(int pid, int *rtPriority)
{
    const int pol = ::sched_getscheduler(pid);
    if (rtPriority) {
        struct sched_param p;
        *rtPriority = (::sched_getparam(pid, &p) == 0) ? p.sched_priority : 0;
    }
    return pol;
}

// ioprio: (class << 13) | prio ; who=IOPRIO_WHO_PROCESS(1)
static constexpr int kIoprioClassShift = 13;
static constexpr int kIoprioWhoProcess = 1;

Result setIoPrio(int pid, int ioClass, int prio)
{
    const int ioprio = (ioClass << kIoprioClassShift) | (prio & 0x7);
    if (::syscall(__NR_ioprio_set, kIoprioWhoProcess, pid, ioprio) == 0)
        return Result::good();
    return Result::failErr(i18n("Falha ao definir prioridade de I/O: %1", errStr(errno)), errno);
}

int getIoPrio(int pid, int *ioClass)
{
    const long v = ::syscall(__NR_ioprio_get, kIoprioWhoProcess, pid);
    if (v < 0) { if (ioClass) *ioClass = 0; return 0; }
    if (ioClass) *ioClass = (v >> kIoprioClassShift) & 0x7;
    return v & 0x7;
}

Result setRlimit(int pid, int resource, quint64 soft, quint64 hard)
{
    struct rlimit nl;
    nl.rlim_cur = (soft == ~0ull) ? RLIM_INFINITY : soft;
    nl.rlim_max = (hard == ~0ull) ? RLIM_INFINITY : hard;
    if (::prlimit(pid, static_cast<__rlimit_resource>(resource), &nl, nullptr) == 0)
        return Result::good();
    return Result::failErr(i18n("Falha ao definir limite: %1", errStr(errno)), errno);
}

bool getRlimit(int pid, int resource, quint64 *soft, quint64 *hard)
{
    struct rlimit ol;
    if (::prlimit(pid, static_cast<__rlimit_resource>(resource), nullptr, &ol) != 0)
        return false;
    if (soft) *soft = (ol.rlim_cur == RLIM_INFINITY) ? ~0ull : ol.rlim_cur;
    if (hard) *hard = (ol.rlim_max == RLIM_INFINITY) ? ~0ull : ol.rlim_max;
    return true;
}

} // namespace actions
