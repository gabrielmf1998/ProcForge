#include "FdInjector.h"
#include "core/Syscalls.h"

#include <KLocalizedString>

#include <cerrno>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/syscall.h>
#include <elf.h>

namespace {
QString err(int e) { return QString::fromUtf8(::strerror(e)); }
}

namespace inject {

// Técnica clássica de injeção de syscall via ptrace (x86_64):
//   SEIZE+INTERRUPT -> salva regs e a palavra em RIP -> escreve a instrução
//   `syscall` (0f 05) em RIP -> carrega rax=__NR_close, rdi=fd -> single-step
//   executa a syscall -> lê rax -> restaura tudo -> DETACH.
// Best-effort: se a thread estava dormindo dentro de uma syscall no momento da
// interrupção, o kernel pode reiniciá-la; o helper (fase 2) fará isto mais robusto.
Result closeForeignFd(int pid, int fd)
{
    if (pid <= 1 || pid == ::getpid())
        return {false, i18n("Alvo inválido."), 0};

    if (::ptrace(PTRACE_SEIZE, pid, 0, 0) < 0)
        return {false, i18n("PTRACE_SEIZE falhou: %1", err(errno)), 0};

    auto detach = [&]() { ::ptrace(PTRACE_DETACH, pid, 0, 0); };

    if (::ptrace(PTRACE_INTERRUPT, pid, 0, 0) < 0) {
        int e = errno; detach();
        return {false, i18n("PTRACE_INTERRUPT falhou: %1", err(e)), 0};
    }
    int status = 0;
    if (::waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
        detach();
        return {false, i18n("O alvo não parou como esperado."), 0};
    }

    user_regs_struct regs{}, saved{};
    struct iovec iov { &regs, sizeof regs };
    if (::ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) < 0) {
        int e = errno; detach();
        return {false, i18n("GETREGSET falhou: %1", err(e)), 0};
    }
    saved = regs;

    errno = 0;
    long savedWord = ::ptrace(PTRACE_PEEKTEXT, pid, saved.rip, 0);
    if (savedWord == -1 && errno) {
        int e = errno; detach();
        return {false, i18n("PEEKTEXT falhou: %1", err(e)), 0};
    }

    // Injeta a instrução `syscall` (bytes 0f 05) nos 2 bytes baixos de RIP.
    unsigned long long newWord =
        (static_cast<unsigned long long>(savedWord) & ~0xffffULL) | 0x050fULL;
    if (::ptrace(PTRACE_POKETEXT, pid, saved.rip,
                 reinterpret_cast<void*>(newWord)) < 0) {
        int e = errno; detach();
        return {false, i18n("POKETEXT falhou: %1", err(e)), 0};
    }

    regs.rax = static_cast<unsigned long long>(__NR_close);
    regs.rdi = static_cast<unsigned long long>(fd);
    iov = { &regs, sizeof regs };
    if (::ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) < 0) {
        int e = errno;
        ::ptrace(PTRACE_POKETEXT, pid, saved.rip,
                 reinterpret_cast<void*>(static_cast<unsigned long long>(savedWord)));
        detach();
        return {false, i18n("SETREGSET falhou: %1", err(e)), 0};
    }

    if (::ptrace(PTRACE_SINGLESTEP, pid, 0, 0) < 0) {
        int e = errno; detach();
        return {false, i18n("SINGLESTEP falhou: %1", err(e)), 0};
    }
    ::waitpid(pid, &status, 0);

    // Lê o retorno da syscall em rax.
    iov = { &regs, sizeof regs };
    ::ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov);
    long ret = static_cast<long>(regs.rax);

    // Restaura texto e registradores originais e destaca.
    ::ptrace(PTRACE_POKETEXT, pid, saved.rip,
             reinterpret_cast<void*>(static_cast<unsigned long long>(savedWord)));
    iov = { &saved, sizeof saved };
    ::ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov);
    detach();

    if (ret < 0)
        return {false, i18n("close() no alvo falhou: %1", err(static_cast<int>(-ret))), ret};
    return {true, {}, ret};
}

Result dupForeignFd(int pid, int fd)
{
    if (pid <= 1)
        return {false, i18n("Alvo inválido."), 0};
    int pfd = sc::pidfd_open(pid, 0);
    if (pfd < 0)
        return {false, i18n("pidfd_open falhou: %1", err(errno)), 0};
    int local = sc::pidfd_getfd(pfd, fd, 0);
    int e = errno;
    ::close(pfd);
    if (local < 0)
        return {false, i18n("pidfd_getfd falhou: %1", err(e)), 0};
    // Duplicado com sucesso; fechamos a cópia local (ainda sem consumidor).
    long dup = local;
    ::close(local);
    return {true, {}, dup};
}

} // namespace inject
