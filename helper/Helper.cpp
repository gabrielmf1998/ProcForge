#include "Helper.h"
#include "Polkit.h"
#include "ProcConnector.h"
#include "core/Procfs.h"
#include "inject/LibraryInjector.h"
#include "inject/RemoteMem.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
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
#include <sys/mman.h>
#include <syslog.h>
#include <cstdint>
#include <pwd.h>
#include <grp.h>

#ifdef PROCFORGE_HAVE_RUST
// Núcleo de memória portado para Rust (rust/procforge-rmem), linkado estático.
extern "C" ssize_t pf_read_mem(int pid, std::uint64_t addr, std::uint8_t *buf, std::size_t len);
extern "C" ssize_t pf_write_mem(int pid, std::uint64_t addr, const std::uint8_t *buf, std::size_t len);
#endif

namespace {
// Escreve um valor num arquivo de controle (cgroup) com uma única write().
// O_CREAT é essencial pros marcadores em /run/procforge/<pid> (arquivos NOVOS,
// não pré-existentes como os control files de cgroup); é um no-op inofensivo
// quando o arquivo já existe.
bool echoTo(const QString &path, const QByteArray &v, QString *err)
{
    const int fd = ::open(path.toLocal8Bit().constData(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
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
    if (QFile::exists(leaf + QStringLiteral("/io.max"))) {
        const QStringList devs = QDir(QStringLiteral("/sys/block"))
                                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &dev : devs) {
            QFile df(QStringLiteral("/sys/block/%1/dev").arg(dev));
            if (!df.open(QIODevice::ReadOnly)) continue;
            const QString majmin = QString::fromUtf8(df.readAll()).trimmed();
            echoTo(leaf + QStringLiteral("/io.max"),
                   ioMaxBps > 0
                       ? QStringLiteral("%1 rbps=%2 wbps=%2").arg(majmin).arg(ioMaxBps).toUtf8()
                       : QStringLiteral("%1 rbps=max wbps=max").arg(majmin).toUtf8(),
                   &err);
        }
    }
}

// Acha (ou cria) o leaf procforge do pid, devolvendo o path ABSOLUTO. Reaproveita
// um leaf já existente (de throttle OU de bloqueio de rede) em vez de mover o
// processo de novo — cgroup v2 só permite UM cgroup por processo por vez.
QString ensureLeaf(uint pid)
{
    const QString marker = QStringLiteral("/run/procforge/%1").arg(pid);
    {
        QFile f(marker);
        if (f.open(QIODevice::ReadOnly)) {
            const QString leaf = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')).value(0).trimmed();
            if (!leaf.isEmpty() && QFile::exists(leaf + QStringLiteral("/cgroup.procs")))
                return leaf;
        }
    }
    ::mkdir("/run/procforge", 0755);
    const QString origin    = originCgroup(pid);
    const QString originAbs = origin.isEmpty() ? QString() : (QStringLiteral("/sys/fs/cgroup") + origin);
    const QString leaf      = QStringLiteral("/sys/fs/cgroup/procforge_%1").arg(pid);
    if (::mkdir(leaf.toLocal8Bit().constData(), 0755) != 0 && errno != EEXIST)
        return {};
    QString err;
    if (!echoTo(leaf + QStringLiteral("/cgroup.procs"), QByteArray::number(pid), &err)) {
        ::rmdir(leaf.toLocal8Bit().constData());
        return {};
    }
    echoTo(marker, (leaf + QLatin1Char('\n') + originAbs).toUtf8(), nullptr);
    return leaf;
}

// Devolve o processo à origem (ou à raiz) e remove o leaf + marcador. Mesma
// lógica que o CgroupRelease sempre teve; extraída pra ser reaproveitada quando
// o bloqueio de rede solta o leaf também (e nenhum limite de recurso está ativo).
void releaseLeaf(uint pid)
{
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
        leaf = QStringLiteral("/sys/fs/cgroup/procforge_%1").arg(pid);

    if (!originAbs.isEmpty() && QFile::exists(originAbs + QStringLiteral("/cgroup.procs")))
        echoTo(originAbs + QStringLiteral("/cgroup.procs"), QByteArray::number(pid), nullptr);
    bool stillThere = false;
    {
        QFile lp(leaf + QStringLiteral("/cgroup.procs"));
        if (lp.open(QIODevice::ReadOnly))
            stillThere = !lp.readAll().trimmed().isEmpty();
    }
    if (stillThere)
        echoTo(QStringLiteral("/sys/fs/cgroup/cgroup.procs"), QByteArray::number(pid), nullptr);

    // rmdir pode perder uma corrida: um filho do processo que ainda não saiu
    // do leaf no instante exato, ou memória do cgroup ainda sendo liberada
    // pelo kernel (cgroup.events "populated" cai um pouco depois do exit),
    // deixam o diretório momentaneamente não-vazio/ocupado — na prática isso
    // levou bem mais que 20ms em teste ao vivo. Tenta por até ~2s antes de
    // desistir e deixar o diretório vazio pra trás (assíncrono: não trava a GUI).
    for (int i = 0; i < 50; ++i) {
        if (::rmdir(leaf.toLocal8Bit().constData()) == 0 || errno == ENOENT)
            break;
        ::usleep(40000); // 40ms
    }
    ::unlink(QStringLiteral("/run/procforge/%1").arg(pid).toLocal8Bit().constData());
}

// Algum limite de recurso (cpu/mem/pids) ainda ativo nesse leaf? io.max fica de
// fora (formato por dispositivo, raro ser o único limite ativo sozinho).
bool leafHasActiveLimits(const QString &leaf)
{
    auto limited = [](const QString &path, bool firstTokenOnly) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return false;
        QByteArray v = f.readAll().trimmed();
        if (firstTokenOnly) v = v.split(' ').value(0);
        return !v.isEmpty() && v != "max";
    };
    return limited(leaf + QStringLiteral("/cpu.max"), true)
        || limited(leaf + QStringLiteral("/memory.max"), false)
        || limited(leaf + QStringLiteral("/pids.max"), false);
}

// Path relativo à raiz do cgroupfs (o que `iptables -m cgroup --path` espera),
// funciona tanto pro leaf achatado (procforge_<pid>) quanto pro aninhado
// (dentro do scope de origem).
QString relCgroupPath(const QString &absLeaf)
{
    QString rel = absLeaf;
    if (rel.startsWith(QLatin1String("/sys/fs/cgroup")))
        rel = rel.mid(14);
    while (rel.startsWith(QLatin1Char('/')))
        rel.remove(0, 1);
    return rel;
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

// Insere/remove uma regra DROP casando pelo cgroup v2 (iptables E ip6tables,
// melhor esforço — falta de suporte a v6 não é erro). op: "-I" ou "-D".
void fwRule(const char *op, const char *chain, const QString &relPath)
{
    for (const char *bin : {"iptables", "ip6tables"}) {
        QList<QByteArray> a = { QByteArray(bin), QByteArray(op), QByteArray(chain),
                                QByteArray("-m"), QByteArray("cgroup"),
                                QByteArray("--path"), relPath.toUtf8(),
                                QByteArray("-j"), QByteArray("DROP") };
        QVector<char *> argv;
        for (QByteArray &b : a) argv.append(b.data());
        argv.append(nullptr);
        forkCapture(argv.data(), nullptr);
    }
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

void Helper::Renice(uint pid, qulonglong starttime, int nice)
{
    if (!authorize(QStringLiteral("org.procforge.renice.raise"))) return;
    if (!identityOk(pid, starttime)) return;
    errno = 0;
    if (::setpriority(PRIO_PROCESS, static_cast<id_t>(pid), nice) != 0 || errno != 0) {
        const int e = errno;
        audit(QStringLiteral("Renice"), pid, QStringLiteral("errno=%1").arg(e));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(e)));
        return;
    }
    audit(QStringLiteral("Renice(%1)").arg(nice), pid, QStringLiteral("ok"));
}

void Helper::SetAffinity(uint pid, qulonglong starttime, const QList<uint> &cpus)
{
    if (!authorize(QStringLiteral("org.procforge.affinity.other"))) return;
    if (!identityOk(pid, starttime)) return;
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

QByteArray Helper::ReadMem(uint pid, qulonglong starttime, qulonglong addr, uint len)
{
    if (!authorize(QStringLiteral("org.procforge.mem.read"))) return {};
    if (!identityOk(pid, starttime)) return {};
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

void Helper::WriteMem(uint pid, qulonglong starttime, qulonglong addr, const QByteArray &bytes)
{
    if (!authorize(QStringLiteral("org.procforge.mem.write"))) return;
    if (!identityOk(pid, starttime)) return;
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

// ---- manipulação de páginas de memória (mmap/mprotect/munmap) ----
// Um só direito: "mexer no mapa de memória alheio" cobre alocar/proteger/liberar.

qulonglong Helper::AllocMem(uint pid, qulonglong starttime, qulonglong length, int prot)
{
    if (!authorize(QStringLiteral("org.procforge.mem.map"))) return 0;
    if (!identityOk(pid, starttime)) return 0;
    if (length == 0 || length > 1ull << 32) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Tamanho inválido."));
        return 0;
    }
    const inject::MemResult r = inject::remoteMmap(static_cast<int>(pid), length, prot);
    if (!r.ok) {
        audit(QStringLiteral("AllocMem"), pid, QStringLiteral("fail"));
        sendErrorReply(QDBusError::Failed, r.error);
        return 0;
    }
    audit(QStringLiteral("AllocMem(len=%1 prot=%2)@0x%3").arg(length).arg(prot).arg(r.ret, 0, 16),
          pid, QStringLiteral("ok"));
    return r.ret;
}

void Helper::ProtectMem(uint pid, qulonglong starttime, qulonglong addr,
                        qulonglong length, int prot)
{
    if (!authorize(QStringLiteral("org.procforge.mem.map"))) return;
    if (!identityOk(pid, starttime)) return;
    const inject::MemResult r = inject::remoteMprotect(static_cast<int>(pid), addr, length, prot);
    if (!r.ok) {
        audit(QStringLiteral("ProtectMem"), pid, QStringLiteral("fail"));
        sendErrorReply(QDBusError::Failed, r.error);
        return;
    }
    audit(QStringLiteral("ProtectMem(0x%1 len=%2 prot=%3)").arg(addr, 0, 16).arg(length).arg(prot),
          pid, QStringLiteral("ok"));
}

void Helper::FreeMem(uint pid, qulonglong starttime, qulonglong addr, qulonglong length)
{
    if (!authorize(QStringLiteral("org.procforge.mem.map"))) return;
    if (!identityOk(pid, starttime)) return;
    const inject::MemResult r = inject::remoteMunmap(static_cast<int>(pid), addr, length);
    if (!r.ok) {
        audit(QStringLiteral("FreeMem"), pid, QStringLiteral("fail"));
        sendErrorReply(QDBusError::Failed, r.error);
        return;
    }
    audit(QStringLiteral("FreeMem(0x%1 len=%2)").arg(addr, 0, 16).arg(length), pid, QStringLiteral("ok"));
}

// ---- "Executar como…": criar um novo processo (Run as) ----
QString Helper::LaunchProcess(const QString &program, const QStringList &args,
                              const QString &username, const QString &cwd,
                              int nice, const QList<uint> &affinity)
{
    if (!authorize(QStringLiteral("org.procforge.process.launch"))) return {};
    if (program.trimmed().isEmpty()) {
        sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Programa vazio."));
        return {};
    }

    // Resolve o usuário-alvo (vazio => root).
    uid_t uid = 0; gid_t gid = 0; QByteArray uname("root"), home("/");
    if (!username.trimmed().isEmpty()) {
        const struct passwd *pw = ::getpwnam(username.toLocal8Bit().constData());
        if (!pw) {
            sendErrorReply(QDBusError::InvalidArgs, QStringLiteral("Usuário desconhecido: %1").arg(username));
            return {};
        }
        uid = pw->pw_uid; gid = pw->pw_gid;
        uname = pw->pw_name; home = pw->pw_dir;
    }

    // argv
    QList<QByteArray> a;
    a.append(program.toLocal8Bit());
    for (const QString &s : args) a.append(s.toLocal8Bit());
    QVector<char *> argv;
    for (QByteArray &b : a) argv.append(b.data());
    argv.append(nullptr);

    const QByteArray cwdB = cwd.trimmed().isEmpty() ? home : cwd.toLocal8Bit();

    // Pipe para o neto devolver o PID (ou o errno de falha) ao helper.
    int pfd[2];
    if (::pipe2(pfd, O_CLOEXEC) != 0) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("pipe: %1").arg(QString::fromUtf8(::strerror(errno))));
        return {};
    }

    const pid_t mid = ::fork();
    if (mid == 0) {
        // Intermediário: solta o processo (setsid) e faz o segundo fork.
        ::close(pfd[0]);
        ::setsid();
        const pid_t g = ::fork();
        if (g == 0) {
            // Neto: aplica afinidade/nice/privilégios e executa.
            if (!affinity.isEmpty()) {
                cpu_set_t set; CPU_ZERO(&set);
                for (uint c : affinity) CPU_SET(c, &set);
                ::sched_setaffinity(0, sizeof set, &set);
            }
            if (uid != 0 || gid != 0) {
                if (::setgid(gid) != 0) { int e = errno; ::write(pfd[1], &e, sizeof e); ::_exit(126); }
                ::initgroups(uname.constData(), gid);
                if (::setuid(uid) != 0) { int e = errno; ::write(pfd[1], &e, sizeof e); ::_exit(126); }
            }
            if (::chdir(cwdB.constData()) != 0) { /* não fatal */ }
            ::setpriority(PRIO_PROCESS, 0, nice);
            ::setenv("HOME", home.constData(), 1);
            ::setenv("USER", uname.constData(), 1);
            ::setenv("LOGNAME", uname.constData(), 1);
            const int devnull = ::open("/dev/null", O_RDWR);
            if (devnull >= 0) { ::dup2(devnull, 0); ::dup2(devnull, 1); ::dup2(devnull, 2); }
            ::execvp(argv[0], argv.data());
            int e = errno; ::write(pfd[1], &e, sizeof e); ::_exit(127);
        }
        // Intermediário: reporta o PID do neto e sai (reparenta o neto ao init).
        int msg = -static_cast<int>(g);   // negativo = pid; positivo = errno
        ::write(pfd[1], &msg, sizeof msg);
        ::_exit(0);
    }
    ::close(pfd[1]);
    int msg = 0;
    const ssize_t rn = ::read(pfd[0], &msg, sizeof msg);
    ::close(pfd[0]);
    int st = 0; ::waitpid(mid, &st, 0);

    if (rn != static_cast<ssize_t>(sizeof msg)) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("Falha ao lançar (sem resposta do filho)."));
        return {};
    }
    if (msg > 0) { // errno reportado pelo neto (setuid/exec falharam)
        audit(QStringLiteral("LaunchProcess(%1)").arg(program), 0, QStringLiteral("errno=%1").arg(msg));
        sendErrorReply(QDBusError::Failed, QString::fromUtf8(::strerror(msg)));
        return {};
    }
    const int newPid = -msg;
    audit(QStringLiteral("LaunchProcess(%1 as %2)").arg(program, QString::fromUtf8(uname)),
          static_cast<uint>(newPid), QStringLiteral("ok"));
    return QStringLiteral("pid %1").arg(newPid);
}

void Helper::CgroupThrottle(uint pid, qulonglong starttime, int cpuPercent,
                            qulonglong memMaxBytes, qulonglong pidsMax, qulonglong ioMaxBps)
{
    if (!authorize(QStringLiteral("org.procforge.cgroup.move"))) return;
    if (!identityOk(pid, starttime)) return;

    // Já existe um leaf procforge pro pid (ex.: bloqueio de rede já ativo)? Reaproveita
    // em vez de tentar mover de novo — cgroup v2 só permite UM cgroup por processo.
    {
        QFile mf(QStringLiteral("/run/procforge/%1").arg(pid));
        if (mf.open(QIODevice::ReadOnly)) {
            const QString existingLeaf =
                QString::fromUtf8(mf.readAll()).split(QLatin1Char('\n')).value(0).trimmed();
            if (!existingLeaf.isEmpty() && QFile::exists(existingLeaf + QStringLiteral("/cgroup.procs"))) {
                applyCgroupLimits(existingLeaf, cpuPercent, memMaxBytes, pidsMax, ioMaxBps);
                audit(QStringLiteral("CgroupThrottle(leaf existente cpu=%1%% mem=%2 pids=%3 io=%4)")
                          .arg(cpuPercent).arg(memMaxBytes).arg(pidsMax).arg(ioMaxBps),
                      pid, QStringLiteral("ok"));
                return;
            }
        }
    }

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

    QString leaf;
    {
        QFile f(QStringLiteral("/run/procforge/%1").arg(pid));
        if (f.open(QIODevice::ReadOnly))
            leaf = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')).value(0).trimmed();
    }
    if (leaf.isEmpty())
        leaf = QStringLiteral("/sys/fs/cgroup/procforge_%1").arg(pid); // compat

    // Bloqueio de rede ainda ativo nesse leaf? A regra de firewall depende do
    // cgroup existir — só limpa os limites de recurso e mantém o processo lá.
    if (QFile::exists(QStringLiteral("/run/procforge/%1.netblock").arg(pid))) {
        applyCgroupLimits(leaf, 0, 0, 0, 0);
        audit(QStringLiteral("CgroupRelease(mantido p/ bloqueio de rede)"), pid, QStringLiteral("ok"));
        return;
    }

    releaseLeaf(pid);
    audit(QStringLiteral("CgroupRelease"), pid, QStringLiteral("ok"));
}

void Helper::NetBlock(uint pid, qulonglong starttime, bool block)
{
    if (!authorize(QStringLiteral("org.procforge.net.block"))) return;
    if (!identityOk(pid, starttime)) return;

    if (block) {
        if (QStandardPaths::findExecutable(QStringLiteral("iptables")).isEmpty()) {
            audit(QStringLiteral("NetBlock"), pid, QStringLiteral("fail(sem iptables)"));
            sendErrorReply(QDBusError::Failed,
                           QStringLiteral("iptables não encontrado (instale o pacote iptables)."));
            return;
        }
        const QString leaf = ensureLeaf(pid);
        if (leaf.isEmpty()) {
            audit(QStringLiteral("NetBlock"), pid, QStringLiteral("fail(leaf)"));
            sendErrorReply(QDBusError::Failed, QStringLiteral("Não consegui isolar o processo num cgroup."));
            return;
        }
        const QString rel = relCgroupPath(leaf);
        for (const char *chain : {"OUTPUT", "INPUT"}) {
            fwRule("-D", chain, rel);   // idempotente: some com duplicata antiga, se houver
            fwRule("-I", chain, rel);
        }
        echoTo(QStringLiteral("/run/procforge/%1.netblock").arg(pid), QByteArray("1"), nullptr);
        audit(QStringLiteral("NetBlock(on)"), pid, QStringLiteral("ok"));
        return;
    }

    QString leaf;
    {
        QFile f(QStringLiteral("/run/procforge/%1").arg(pid));
        if (f.open(QIODevice::ReadOnly))
            leaf = QString::fromUtf8(f.readAll()).split(QLatin1Char('\n')).value(0).trimmed();
    }
    if (leaf.isEmpty())
        leaf = QStringLiteral("/sys/fs/cgroup/procforge_%1").arg(pid); // compat: sem marcador, tenta o leaf padrão
    {
        const QString rel = relCgroupPath(leaf);
        for (const char *chain : {"OUTPUT", "INPUT"})
            fwRule("-D", chain, rel);
    }
    ::unlink(QStringLiteral("/run/procforge/%1.netblock").arg(pid).toLocal8Bit().constData());

    if (!leaf.isEmpty() && !leafHasActiveLimits(leaf))
        releaseLeaf(pid);
    audit(QStringLiteral("NetBlock(off)"), pid, QStringLiteral("ok"));
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
