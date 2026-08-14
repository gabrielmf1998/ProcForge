#include "RemoteMem.h"
#include "core/Procfs.h"

#include <cerrno>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <elf.h>
#include <dlfcn.h>

namespace {

QString errs(int e) { return QString::fromUtf8(::strerror(e)); }

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

// Endereço da função libc `name` DENTRO do alvo: offset do nosso símbolo na
// NOSSA libc, aplicado sobre a base da libc DO ALVO (mesma libc.so.6).
quint64 remoteSym(int pid, const char *name)
{
    const quint64 ourBase = libcBase(::getpid());
    void *ourSym = ::dlsym(RTLD_DEFAULT, name);
    const quint64 tgtBase = libcBase(pid);
    if (!ourBase || !tgtBase || !ourSym)
        return 0;
    return tgtBase + (reinterpret_cast<quint64>(ourSym) - ourBase);
}

// Chama func(a0..a5) no alvo via ptrace e devolve rax. args não usados = 0.
inject::MemResult callRemote(int pid, quint64 func,
                             quint64 a0, quint64 a1, quint64 a2,
                             quint64 a3, quint64 a4, quint64 a5)
{
    if (pid <= 1 || pid == ::getpid())
        return {false, QStringLiteral("Alvo inválido."), 0};
    if (!func)
        return {false, QStringLiteral("Não localizei a função na libc do alvo (libc incompatível?)."), 0};

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

    // Retorno-sentinela: empurra 0 no stack -> ao retornar, SIGSEGV em rip=0.
    const quint64 newRsp  = (saved.rsp - 4096) & ~0xFULL;
    const quint64 callRsp = newRsp - 8;   // rsp%16==8 na entrada (ABI SysV)
    quint64 origRet = 0;
    vmRead(pid, callRsp, &origRet, 8);
    const quint64 sentinel = 0;
    vmWrite(pid, callRsp, &sentinel, 8);

    regs.rip = func;
    regs.rdi = a0; regs.rsi = a1; regs.rdx = a2;
    regs.rcx = a3; regs.r8 = a4;  regs.r9 = a5;
    regs.rsp = callRsp;
    regs.rax = 0;
    if (!setRegs(pid, regs)) {
        int e = errno; vmWrite(pid, callRsp, &origRet, 8); setRegs(pid, saved); detach();
        return {false, QStringLiteral("SETREGSET: %1").arg(errs(e)), 0};
    }

    ::ptrace(PTRACE_CONT, pid, 0, 0);
    quint64 ret = 0;
    QString ferr;
    bool got = false;
    for (;;) {
        if (::waitpid(pid, &status, 0) < 0) { ferr = QStringLiteral("waitpid falhou"); break; }
        if (WIFEXITED(status) || WIFSIGNALED(status))
            return {false, QStringLiteral("o alvo morreu durante a operação"), 0};
        if (!WIFSTOPPED(status)) continue;
        const int sig = WSTOPSIG(status);
        getRegs(pid, regs);
        if (sig == SIGSEGV && regs.rip == 0) { ret = regs.rax; got = true; break; }
        if (sig == SIGTRAP) { ::ptrace(PTRACE_CONT, pid, 0, 0); continue; }
        ferr = QStringLiteral("parou com sinal %1 em rip=0x%2").arg(sig).arg(regs.rip, 0, 16);
        break;
    }

    vmWrite(pid, callRsp, &origRet, 8);
    setRegs(pid, saved);
    detach();

    if (!got)
        return {false, ferr.isEmpty() ? QStringLiteral("Operação falhou.") : ferr, 0};
    // mmap/mprotect/munmap devolvem -errno em [-4095,-1] no caso de erro.
    if (ret > static_cast<quint64>(-4096)) {
        const int e = static_cast<int>(-static_cast<qint64>(ret));
        return {false, errs(e), ret};
    }
    return {true, {}, ret};
}

} // namespace

namespace inject {

MemResult remoteMmap(int pid, quint64 length, int prot)
{
    if (length == 0)
        return {false, QStringLiteral("Tamanho inválido."), 0};
    const quint64 f = remoteSym(pid, "mmap");
    MemResult r = callRemote(pid, f, 0 /*addr*/, length, static_cast<quint64>(prot),
                             MAP_PRIVATE | MAP_ANONYMOUS,
                             static_cast<quint64>(-1) /*fd*/, 0 /*offset*/);
    return r;
}

MemResult remoteMprotect(int pid, quint64 addr, quint64 length, int prot)
{
    const quint64 f = remoteSym(pid, "mprotect");
    return callRemote(pid, f, addr, length, static_cast<quint64>(prot), 0, 0, 0);
}

MemResult remoteMunmap(int pid, quint64 addr, quint64 length)
{
    const quint64 f = remoteSym(pid, "munmap");
    return callRemote(pid, f, addr, length, 0, 0, 0, 0);
}

} // namespace inject
