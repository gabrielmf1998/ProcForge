#include "LibraryInjector.h"
#include "core/Procfs.h"

#include <QFileInfo>

#include <cerrno>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <elf.h>
#include <dlfcn.h>

namespace {

QString errs(int e) { return QString::fromUtf8(::strerror(e)); }

// Base de carga (menor endereço) da libc no processo dado.
quint64 libcBase(int pid)
{
    quint64 best = 0;
    bool found = false;
    for (const procfs::MapEntry &m : procfs::listMaps(pid)) {
        if (m.path.contains(QLatin1String("/libc.so.6"))
            || m.path.contains(QLatin1String("/libc-"))) {
            if (!found || m.start < best) { best = m.start; found = true; }
        }
    }
    return found ? best : 0;
}

ssize_t vmRead(int pid, quint64 addr, void *buf, size_t len)
{
    struct iovec l { buf, len };
    struct iovec r { reinterpret_cast<void *>(addr), len };
    return ::process_vm_readv(pid, &l, 1, &r, 1, 0);
}
ssize_t vmWrite(int pid, quint64 addr, const void *buf, size_t len)
{
    struct iovec l { const_cast<void *>(buf), len };
    struct iovec r { reinterpret_cast<void *>(addr), len };
    return ::process_vm_writev(pid, &l, 1, &r, 1, 0);
}

bool getRegs(int pid, user_regs_struct &regs)
{
    struct iovec iov { &regs, sizeof regs };
    return ::ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) == 0;
}
bool setRegs(int pid, user_regs_struct &regs)
{
    struct iovec iov { &regs, sizeof regs };
    return ::ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) == 0;
}

} // namespace

namespace inject {

Result injectLibrary(int pid, const QString &soPath)
{
    if (pid <= 1 || pid == ::getpid())
        return {false, QStringLiteral("Alvo inválido."), 0};

    const QString abs = QFileInfo(soPath).absoluteFilePath();
    if (!QFileInfo::exists(abs))
        return {false, QStringLiteral("Arquivo não encontrado: %1").arg(abs), 0};
    QByteArray path = abs.toUtf8();
    path.append('\0');

    // Resolve dlopen do alvo: offset do nosso dlopen dentro da NOSSA libc,
    // aplicado sobre a base da libc DO ALVO (mesma libc.so.6 -> mesmo offset).
    const quint64 ourBase = libcBase(::getpid());
    void *ourDlopen = ::dlsym(RTLD_DEFAULT, "dlopen");
    if (!ourDlopen)
        ourDlopen = ::dlsym(RTLD_DEFAULT, "__libc_dlopen_mode");
    const quint64 tgtBase = libcBase(pid);
    if (!ourBase || !tgtBase || !ourDlopen)
        return {false, QStringLiteral("Não localizei libc/dlopen (alvo com libc incompatível?)."), 0};
    const quint64 tgtDlopen = tgtBase + (reinterpret_cast<quint64>(ourDlopen) - ourBase);

    if (::ptrace(PTRACE_SEIZE, pid, 0, 0) < 0)
        return {false, QStringLiteral("PTRACE_SEIZE falhou: %1").arg(errs(errno)), 0};
    auto detach = [&]() { ::ptrace(PTRACE_DETACH, pid, 0, 0); };

    if (::ptrace(PTRACE_INTERRUPT, pid, 0, 0) < 0) {
        int e = errno; detach();
        return {false, QStringLiteral("PTRACE_INTERRUPT falhou: %1").arg(errs(e)), 0};
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        detach();
        return {false, QStringLiteral("O alvo não parou."), 0};
    }

    user_regs_struct regs{}, saved{};
    if (!getRegs(pid, regs)) { int e = errno; detach(); return {false, QStringLiteral("GETREGSET: %1").arg(errs(e)), 0}; }
    saved = regs;

    // Áreas de rascunho bem abaixo do rsp atual (stack é gravável).
    const quint64 strAddr = (saved.rsp - 2048) & ~0xFULL;
    const quint64 newRsp  = (saved.rsp - 4096) & ~0xFULL;
    const quint64 callRsp = newRsp - 8;   // retorno-sentinela empurrado -> rsp%16==8 na entrada

    // Salva a memória que vamos sobrescrever, para restaurar depois.
    QByteArray origStr(path.size(), 0);
    vmRead(pid, strAddr, origStr.data(), path.size());
    quint64 origRet = 0;
    vmRead(pid, callRsp, &origRet, 8);

    if (vmWrite(pid, strAddr, path.constData(), path.size()) != path.size()) {
        int e = errno; setRegs(pid, saved); detach();
        return {false, QStringLiteral("Falha ao escrever o caminho no alvo: %1").arg(errs(e)), 0};
    }
    const quint64 sentinel = 0; // retornar aqui -> SIGSEGV que capturamos
    vmWrite(pid, callRsp, &sentinel, 8);

    regs.rip = tgtDlopen;
    regs.rdi = strAddr;
    regs.rsi = 0x102;   // RTLD_NOW | RTLD_GLOBAL
    regs.rsp = callRsp;
    regs.rax = 0;
    if (!setRegs(pid, regs)) {
        int e = errno; vmWrite(pid, strAddr, origStr.constData(), path.size());
        vmWrite(pid, callRsp, &origRet, 8); setRegs(pid, saved); detach();
        return {false, QStringLiteral("SETREGSET: %1").arg(errs(e)), 0};
    }

    // Executa dlopen no alvo e espera o retorno-sentinela (SIGSEGV em rip=0).
    ::ptrace(PTRACE_CONT, pid, 0, 0);
    long    handle = -1;
    QString ferr;
    for (;;) {
        if (::waitpid(pid, &status, 0) < 0) { ferr = QStringLiteral("waitpid falhou"); break; }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            ferr = QStringLiteral("o alvo morreu durante a injeção");
            return {false, ferr, 0}; // já não há o que restaurar
        }
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        getRegs(pid, regs);
        if (sig == SIGSEGV && regs.rip == 0) { handle = static_cast<long>(regs.rax); break; }
        if (sig == SIGTRAP) { ::ptrace(PTRACE_CONT, pid, 0, 0); continue; } // event-stop
        ferr = QStringLiteral("parou com sinal %1 em rip=0x%2 (possível crash no dlopen)")
                   .arg(sig).arg(regs.rip, 0, 16);
        break;
    }

    // Restaura memória e registradores originais e destaca.
    vmWrite(pid, strAddr, origStr.constData(), path.size());
    vmWrite(pid, callRsp, &origRet, 8);
    setRegs(pid, saved);
    detach();

    if (handle > 0)
        return {true, {}, handle};
    if (handle == 0)
        return {false, QStringLiteral("dlopen retornou NULL no alvo (.so não carregou; ABI/libc?)."), 0};
    return {false, ferr.isEmpty() ? QStringLiteral("Injeção falhou.") : ferr, 0};
}

} // namespace inject
