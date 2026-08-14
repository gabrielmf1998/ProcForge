#include "Helper.h"
#include "Polkit.h"
#include "ProcConnector.h"
#include "core/Procfs.h"
#include "inject/LibraryInjector.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QVector>

#include <functional>
#include <sys/wait.h>

#include <cerrno>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <syslog.h>
#include <cstdint>

#ifdef PROCFORGE_HAVE_RUST
// Núcleo de memória portado para Rust (rust/procforge-rmem), linkado estático.
extern "C" ssize_t pf_read_mem(int pid, std::uint64_t addr, std::uint8_t *buf, std::size_t len);
extern "C" ssize_t pf_write_mem(int pid, std::uint64_t addr, const std::uint8_t *buf, std::size_t len);
#endif

namespace {
// Escreve um valor num arquivo de controle (cgroup) com uma única write().
bool echoTo(const QString &path, const QByteArray &v, QString *err)
{
    const int fd = ::open(path.toLocal8Bit().constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) { if (err) *err = QString::fromUtf8(::strerror(errno)); return false; }
    const ssize_t n = ::write(fd, v.constData(), v.size());
    const int e = errno;
    ::close(fd);
    if (n != v.size()) { if (err) *err = QString::fromUtf8(::strerror(e)); return false; }
    return true;
}

QString originCgroup(uint pid)
{
    QFile f(QStringLiteral("/proc/%1/cgroup").arg(pid));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const auto lines = f.readAll().split('\n');
    for (const QByteArray &line : lines)
        if (line.startsWith("0::"))
            return QString::fromUtf8(line.mid(3)).trimmed();
    return {};
}

// Aplica os limites nos arquivos de controle que EXISTEM no leaf (io por dispositivo).
void applyCgroupLimits(const QString &leaf, int cpuPercent, qulonglong memMax,
                       qulonglong pidsMax, qulonglong ioMaxBps)
{
    QString err;
    if (QFile::exists(leaf + QStringLiteral("/cpu.max")))
        echoTo(leaf + QStringLiteral("/cpu.max"),
               cpuPercent > 0 ? QByteArray::number(qlonglong(cpuPercent) * 1000) + " 100000"
                              : QByteArray("max"), &err);
    if (QFile::exists(leaf + QStringLiteral("/memory.max")))
        echoTo(leaf + QStringLiteral("/memory.max"),
               memMax > 0 ? QByteArray::number(qulonglong(memMax)) : QByteArray("max"), &err);
    if (QFile::exists(leaf + QStringLiteral("/pids.max")))
        echoTo(leaf + QStringLiteral("/pids.max"),
               pidsMax > 0 ? QByteArray::number(qulonglong(pidsMax)) : QByteArray("max"), &err);
    if (ioMaxBps > 0 && QFile::exists(leaf + QStringLiteral("/io.max"))) {
        const QStringList devs = QDir(QStringLiteral("/sys/block"))
                                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &dev : devs) {
            QFile df(QStringLiteral("/sys/block/%1/dev").arg(dev));
            if (!df.open(QIODevice::ReadOnly)) continue;
            const QString majmin = QString::fromUtf8(df.readAll()).trimmed();
            echoTo(leaf + QStringLiteral("/io.max"),
                   QStringLiteral("%1 rbps=%2 wbps=%2").arg(majmin).arg(ioMaxBps).toUtf8(), &err);
        }
    }
}

// fork+exec de argv capturando stdout+stderr. childPre() roda no filho antes do
// exec (usado p/ setns). Retorna a saída (limitada). O filho herda o seccomp/caps
// da unit — daí precisarmos liberar setns/bpf/netlink no procforged.service.
QString forkCapture(char *const argv[], const std::function<void()> &childPre, int capMax = 1 << 20)
{
    int pfd[2];
    if (::pipe2(pfd, O_CLOEXEC) != 0)
        return QStringLiteral("(falha ao criar pipe)");
    const pid_t c = ::fork();
    if (c == 0) {
        ::dup2(pfd[1], 1);
        ::dup2(pfd[1], 2);
        if (childPre) childPre();
        ::execvp(argv[0], argv);
        ::_exit(127);
    }
    ::close(pfd[1]);
    QByteArray out;
    char buf[8192];
    ssize_t r;
    while ((r = ::read(pfd[0], buf, sizeof buf)) > 0) {
        out.append(buf, static_cast<int>(r));
        if (out.size() > capMax) break;
    }
    ::close(pfd[0]);
    int status = 0;
    ::waitpid(c, &status, 0);
    return QString::fromUtf8(out);
}
} // namespace

Helper::Helper(QObject *parent) : QObject(parent)
{
    m_conn = new ProcConnector(this);
    connect(m_conn, &ProcConnector::procEvent, this, &Helper::ProcEvent);

    m_watcher = new QDBusServiceWatcher(this);
    m_watcher->setConnection(QDBusConnection::systemBus());
    m_watcher->setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(m_watcher, &QDBusServiceWatcher::serviceUnregistered,
            this, &Helper::onSubscriberGone);
}

bool Helper::hasSubscriber() const { return !m_subscribers.isEmpty(); }

QString Helper::Ping()
{
    Q_EMIT activity();
    return QStringLiteral("procforged 0.1");
}

void Helper::audit(const QString &action, uint pid, const QString &result)
{
    const QString caller = calledFromDBus() ? message().service() : QStringLiteral("(local)");
    ::syslog(LOG_AUTHPRIV | LOG_NOTICE,
             "procforged: caller=%s action=%s target_pid=%u result=%s",
             qPrintable(caller), qPrintable(action), pid, qPrintable(result));
}

bool Helper::authorize(const QString &action)
{
    Q_EMIT activity();
    const QString caller = message().service();
    QString err;
    const Polkit::Result r = Polkit::check(caller, action, &err);
    if (r == Polkit::Yes)
        return true;
    audit(action, 0, r == Polkit::Error ? QStringLiteral("polkit-error") : QStringLiteral("denied"));
    sendErrorReply(QDBusError::AccessDenied,
                   r == Polkit::Error
                       ? QStringLiteral("Erro na verificação polkit: %1").arg(err)
                       : QStringLiteral("Não autorizado para a ação %1").arg(action));
    return false;
}

bool Helper::identityOk(uint pid, qulonglong starttime)
{
    if (pid <= 1) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("PID inválido: %1").arg(pid));
        return false;
    }
    if (starttime != 0) {
        procfs::StatFields st;
        if (!procfs::readStat(static_cast<int>(pid), st)) {
            sendErrorReply(QDBusError::Failed, QStringLiteral("O processo %1 não existe").arg(pid));
            return false;
        }
        if (st.starttime != starttime) {
            sendErrorReply(QDBusError::Failed,
                           QStringLiteral("PID %1 foi reutilizado (starttime diferente)").arg(pid));
            return false;
        }
    }
    return true;
}

void Helper::SendSignal(uint pid, qulonglong starttime, int sig)
{
    if (!authorize(QStringLiteral("org.procforge.signal.other"))) return;
    if (!identityOk(pid, starttime)) return;
    if (::kill(static_cast<pid_t>(pid), sig) != 0) {
        const int e = errno;
        audit(QStringLiteral("SendSignal"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("SendSignal(%1)").arg(sig), pid, QStringLiteral("ok"));
}

void Helper::Renice(uint pid, int nice)
{
    if (!authorize(QStringLiteral("org.procforge.renice.raise"))) return;
    if (!identityOk(pid, 0)) return;
    errno = 0;
    if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), nice) != 0 || errno != 0) {
        const int e = errno;
        audit(QStringLiteral("Renice"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("Renice(%1)").arg(nice), pid, QStringLiteral("ok"));
}

void Helper::SetAffinity(uint pid, const QList<uint> &cpus)
{
    if (!authorize(QStringLiteral("org.procforge.affinity.other"))) return;
    if (!identityOk(pid, 0)) return;
    if (cpus.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Lista de CPUs vazia"));
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (uint c : cpus)
        CPU_SET(c, &set);
    if (::sched_setaffinity(static_cast<pid_t>(pid), sizeof set, &set) != 0) {
        const int e = errno;
        audit(QStringLiteral("SetAffinity"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("SetAffinity"), pid, QStringLiteral("ok"));
}

QByteArray Helper::ReadMem(uint pid, qulonglong addr, uint len)
{
    if (!authorize(QStringLiteral("org.procforge.mem.read"))) return {};
    if (!identityOk(pid, 0)) return {};
    if (len == 0 || len > 16u * 1024u * 1024u) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Tamanho inválido: %1").arg(len));
        return {};
    }
    QByteArray buf(static_cast<int>(len), 0);
#ifdef PROCFORGE_HAVE_RUST
    const ssize_t n = pf_read_mem(static_cast<int>(pid), addr,
                                  reinterpret_cast<std::uint8_t *>(buf.data()), len);
#else
    struct iovec local  { buf.data(), len };
    struct iovec remote { reinterpret_cast<void *>(addr), len };
    const ssize_t n = ::process_vm_readv(static_cast<pid_t>(pid), &local, 1, &remote, 1, 0);
#endif
    if (n < 0) {
        const int e = errno;
        audit(QStringLiteral("ReadMem"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return {};
    }
    buf.resize(static_cast<int>(n));
    audit(QStringLiteral("ReadMem"), pid, QStringLiteral("ok(%1B)").arg(n));
    return buf;
}

void Helper::WriteMem(uint pid, qulonglong addr, const QByteArray &bytes)
{
    if (!authorize(QStringLiteral("org.procforge.mem.write"))) return;
    if (!identityOk(pid, 0)) return;
    if (bytes.isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Nada a escrever"));
        return;
    }
#ifdef PROCFORGE_HAVE_RUST
    const ssize_t n = pf_write_mem(static_cast<int>(pid), addr,
                                   reinterpret_cast<const std::uint8_t *>(bytes.constData()),
                                   static_cast<size_t>(bytes.size()));
#else
    struct iovec local  { const_cast<char *>(bytes.constData()), static_cast<size_t>(bytes.size()) };
    struct iovec remote { reinterpret_cast<void *>(addr), static_cast<size_t>(bytes.size()) };
    const ssize_t n = ::process_vm_writev(static_cast<pid_t>(pid), &local, 1, &remote, 1, 0);
#endif
    if (n != bytes.size()) {
        const int e = errno;
        audit(QStringLiteral("WriteMem"), pid, QStringLiteral("errno=%1 n=%2").arg(e).arg(n));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("WriteMem"), pid, QStringLiteral("ok(%1B)").arg(n));
}

void Helper::InjectLibrary(uint pid, qulonglong starttime, const QString &path)
{
    if (!authorize(QStringLiteral("org.procforge.inject.library"))) return;
    if (!identityOk(pid, starttime)) return;
    const inject::Result r = inject::injectLibrary(static_cast<int>(pid), path);
    if (!r.ok) {
        audit(QStringLiteral("InjectLibrary"), pid, QStringLiteral("fail"));
        sendErrorReply(QDBusError::Failed, r.error);
        return;
    }
    audit(QStringLiteral("InjectLibrary(%1)").arg(path), pid, QStringLiteral("ok"));
}

void Helper::CgroupThrottle(uint pid, qulonglong starttime, int cpuPercent,
                            qulonglong memMaxBytes, qulonglong pidsMax, qulonglong ioMaxBps)
{
    if (!authorize(QStringLiteral("org.procforge.cgroup.move"))) return;
    if (!identityOk(pid, starttime)) return;

    ::mkdir("/run/procforge", 0755);
    const QString origin    = originCgroup(pid);
    const QString originAbs = QStringLiteral("/sys/fs/cgroup") + origin;
    const QString marker    = QStringLiteral("/run/procforge/%1").arg(pid);

    // 1) NESTED: cria um leaf DENTRO do scope de origem. No release o processo
    //    volta EXATO à origem (o scope não é coletado porque tem nosso filho).
    if (!origin.isEmpty() && origin != QLatin1String("/")
        && QFile::exists(originAbs + QStringLiteral("/cgroup.procs"))) {
        const QString leaf = originAbs + QStringLiteral("/procforge");
        if (::mkdir(leaf.toLocal8Bit().constData(), 0755) == 0 || errno == EEXIST) {
            QString err;
            if (echoTo(leaf + QStringLiteral("/cgroup.procs"), QByteArray::number(pid), &err)) {
                // habilita controladores na origem (agora sem procs diretos, se só tínhamos nós)
                echoTo(originAbs + QStringLiteral("/cgroup.subtree_control"),
                       QByteArray("+cpu +memory +pids +io"), nullptr);
                // Só aninha se TODOS os controladores pedidos estiverem disponíveis aqui.
                const bool okCpu  = cpuPercent <= 0 || QFile::exists(leaf + QStringLiteral("/cpu.max"));
                const bool okMem  = memMaxBytes == 0 || QFile::exists(leaf + QStringLiteral("/memory.max"));
                const bool okPids = pidsMax == 0 || QFile::exists(leaf + QStringLiteral("/pids.max"));
                const bool okIo   = ioMaxBps == 0 || QFile::exists(leaf + QStringLiteral("/io.max"));
                if (okCpu && okMem && okPids && okIo) {
                    applyCgroupLimits(leaf, cpuPercent, memMaxBytes, pidsMax, ioMaxBps);
                    echoTo(marker, (leaf + QLatin1Char('\n') + originAbs).toUtf8(), nullptr);
                    audit(QStringLiteral("CgroupThrottle(nested cpu=%1%% mem=%2 pids=%3 io=%4)")
                              .arg(cpuPercent).arg(memMaxBytes).arg(pidsMax).arg(ioMaxBps),
                          pid, QStringLiteral("ok"));
                    return;
                }
                echoTo(originAbs + QStringLiteral("/cgroup.procs"), QByteArray::number(pid), nullptr);
            }
            ::rmdir(leaf.toLocal8Bit().constData());
        }
    }

    // 2) FLAT: leaf irmão da raiz (sempre funciona; no release volta à origem ou à raiz).
    const QString leaf = QStringLiteral("/sys/fs/cgroup/procforge_%1").arg(pid);
    if (::mkdir(leaf.toLocal8Bit().constData(), 0755) != 0 && errno != EEXIST) {
        const int e = errno;
        sendErrorReply(QDBusError::Failed, QStringLiteral("mkdir cgroup: %1").arg(QString::fromUtf8(::strerror(e))));
        return;
    }
    applyCgroupLimits(leaf, cpuPercent, memMaxBytes, pidsMax, ioMaxBps);
    echoTo(marker, (leaf + QLatin1Char('\n') + originAbs).toUtf8(), nullptr);
    QString err;
    if (!echoTo(leaf + QStringLiteral("/cgroup.procs"), QByteArray::number(pid), &err)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("mover para o cgroup: %1").arg(err));
        return;
    }
    audit(QStringLiteral("CgroupThrottle(flat cpu=%1%% mem=%2 pids=%3 io=%4)")
              .arg(cpuPercent).arg(memMaxBytes).arg(pidsMax).arg(ioMaxBps), pid, QStringLiteral("ok"));
}

void Helper::CgroupRelease(uint pid, qulonglong starttime)
{
    if (!authorize(QStringLiteral("org.procforge.cgroup.move"))) return;
    Q_UNUSED(starttime); // pode já ter mudado; apenas limpamos

    // Marcador: linha 1 = leaf (onde o processo está), linha 2 = origem (destino).
    QString leaf, originAbs;
    {
        QFile f(QStringLiteral("/run/procforge/%1").arg(pid));
        if (f.open(QIODevice::ReadOnly)) {
            const QStringList lines = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'));
            if (lines.size() >= 1) leaf = lines.at(0).trimmed();
            if (lines.size() >= 2) originAbs = lines.at(1).trimmed();
        }
    }
    if (leaf.isEmpty())
        leaf = QStringLiteral("/sys/fs/cgroup/procforge_%1").arg(pid); // compat

    QString err;
    // 1) devolve ao cgroup de origem (no caso nested, o scope ainda existe -> restauração exata).
    if (!originAbs.isEmpty() && QFile::exists(originAbs + QStringLiteral("/cgroup.procs")))
        echoTo(originAbs + QStringLiteral("/cgroup.procs"), QByteArray::number(pid), &err);
    // 2) se ainda estiver no leaf, cai na raiz (sempre aceita).
    bool stillThere = false;
    {
        QFile lp(leaf + QStringLiteral("/cgroup.procs"));
        if (lp.open(QIODevice::ReadOnly))
            stillThere = !lp.readAll().trimmed().isEmpty();
    }
    if (stillThere)
        echoTo(QStringLiteral("/sys/fs/cgroup/cgroup.procs"), QByteArray::number(pid), &err);

    const int rr = ::rmdir(leaf.toLocal8Bit().constData());
    ::unlink(QStringLiteral("/run/procforge/%1").arg(pid).toLocal8Bit().constData());

    if (rr != 0 && errno != ENOENT) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Não consegui remover o cgroup (processo ainda dentro?): %1")
                           .arg(QString::fromUtf8(::strerror(errno))));
        return;
    }
    audit(QStringLiteral("CgroupRelease"), pid, QStringLiteral("ok"));
}

void Helper::SetScheduler(uint pid, qulonglong starttime, int policy, int rtPriority)
{
    if (!authorize(QStringLiteral("org.procforge.sched.realtime"))) return;
    if (!identityOk(pid, starttime)) return;
    struct sched_param p;
    p.sched_priority = (policy == SCHED_FIFO || policy == SCHED_RR) ? rtPriority : 0;
    if (::sched_setscheduler(static_cast<pid_t>(pid), policy, &p) != 0) {
        const int e = errno;
        audit(QStringLiteral("SetScheduler"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("SetScheduler(%1)").arg(policy), pid, QStringLiteral("ok"));
}

void Helper::SetIoPrio(uint pid, qulonglong starttime, int ioClass, int prio)
{
    if (!authorize(QStringLiteral("org.procforge.sched.realtime"))) return;
    if (!identityOk(pid, starttime)) return;
    const int ioprio = (ioClass << 13) | (prio & 0x7);
    if (::syscall(__NR_ioprio_set, 1 /*IOPRIO_WHO_PROCESS*/, static_cast<int>(pid), ioprio) != 0) {
        const int e = errno;
        audit(QStringLiteral("SetIoPrio"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("SetIoPrio"), pid, QStringLiteral("ok"));
}

void Helper::SetRlimit(uint pid, qulonglong starttime, int resource, qulonglong soft, qulonglong hard)
{
    if (!authorize(QStringLiteral("org.procforge.prlimit.set"))) return;
    if (!identityOk(pid, starttime)) return;
    struct rlimit nl;
    nl.rlim_cur = (soft == ~0ull) ? RLIM_INFINITY : soft;
    nl.rlim_max = (hard == ~0ull) ? RLIM_INFINITY : hard;
    if (::prlimit(static_cast<pid_t>(pid), static_cast<__rlimit_resource>(resource), &nl, nullptr) != 0) {
        const int e = errno;
        audit(QStringLiteral("SetRlimit"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("SetRlimit(res=%1)").arg(resource), pid, QStringLiteral("ok"));
}

QString Helper::NsRun(uint pid, qulonglong starttime, const QString &program, const QStringList &args)
{
    if (!authorize(QStringLiteral("org.procforge.ns.enter"))) return {};
    if (!identityOk(pid, starttime)) return {};

    // Abre os fds de namespace ANTES de qualquer setns (mnt por último no filho).
    static const char *types[] = {"net", "uts", "ipc", "cgroup", "mnt"};
    QVector<int> fds;
    for (const char *t : types) {
        const QString p = QStringLiteral("/proc/%1/ns/%2").arg(pid).arg(QLatin1String(t));
        fds.append(::open(p.toLocal8Bit().constData(), O_RDONLY | O_CLOEXEC));
    }

    QList<QByteArray> a;
    a.append(program.toLocal8Bit());
    for (const QString &s : args) a.append(s.toLocal8Bit());
    QVector<char *> argv;
    for (QByteArray &b : a) argv.append(b.data());
    argv.append(nullptr);

    const QString out = forkCapture(argv.data(), [fds]() {
        for (int fd : fds) if (fd >= 0) ::setns(fd, 0);  // filho é single-thread (pós-fork)
    });
    for (int fd : fds) if (fd >= 0) ::close(fd);

    audit(QStringLiteral("NsRun(%1)").arg(program), pid, QStringLiteral("ok"));
    return out;
}

QString Helper::BpfTrace(uint pid, qulonglong starttime, const QString &mode, int seconds)
{
    if (!authorize(QStringLiteral("org.procforge.ebpf.trace"))) return {};
    if (!identityOk(pid, starttime)) return {};
    const int secs = (seconds < 1 || seconds > 30) ? 5 : seconds;

    QString prog;
    if (mode == QLatin1String("files"))
        prog = QStringLiteral("tracepoint:syscalls:sys_enter_openat /pid==%1/ "
                              "{ printf(\"%s\\n\", str(args->filename)); }").arg(pid);
    else if (mode == QLatin1String("tcp"))
        prog = QStringLiteral("tracepoint:syscalls:sys_enter_connect /pid==%1/ "
                              "{ printf(\"connect (fd %d)\\n\", args->fd); }").arg(pid);
    else // syscalls: contagem por nome (wildcard dá o nome no 'probe')
        prog = QStringLiteral("tracepoint:syscalls:sys_enter_* /pid==%1/ { @[probe]=count(); }").arg(pid);

    QList<QByteArray> a = { QByteArray("timeout"), QByteArray::number(secs),
                            QByteArray("bpftrace"), QByteArray("-e"), prog.toUtf8() };
    QVector<char *> argv;
    for (QByteArray &b : a) argv.append(b.data());
    argv.append(nullptr);

    QString out = forkCapture(argv.data(), nullptr);
    audit(QStringLiteral("BpfTrace(%1)").arg(mode), pid, QStringLiteral("ok"));
    if (out.trimmed().isEmpty())
        out = QStringLiteral("(sem eventos em %1s — o processo não fez essa atividade, "
                             "ou o bpftrace foi bloqueado pelo sandbox do helper)").arg(secs);
    return out;
}

// ---- eventos de processo em push (cn_proc), sem polkit (observacional) ----

void Helper::SubscribeProcEvents()
{
    Q_EMIT activity();
    const QString caller = message().service();
    if (m_subscribers.isEmpty() && !m_conn->active() && !m_conn->start()) {
        sendErrorReply(QDBusError::Failed,
                       QStringLiteral("Não consegui abrir o conector de processos (CAP_NET_ADMIN?)."));
        return;
    }
    if (!m_subscribers.contains(caller)) {
        m_subscribers.insert(caller);
        m_watcher->addWatchedService(caller);
    }
    audit(QStringLiteral("SubscribeProcEvents"), 0, QStringLiteral("ok"));
}

void Helper::UnsubscribeProcEvents()
{
    Q_EMIT activity();
    const QString caller = message().service();
    m_subscribers.remove(caller);
    m_watcher->removeWatchedService(caller);
    if (m_subscribers.isEmpty())
        m_conn->stop();
}

void Helper::onSubscriberGone(const QString &name)
{
    m_subscribers.remove(name);
    m_watcher->removeWatchedService(name);
    if (m_subscribers.isEmpty())
        m_conn->stop();
    Q_EMIT activity(); // reinicia a contagem de idle p/ o helper poder sair
}
