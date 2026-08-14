#pragma once
// Wrappers finos sobre syscalls que a glibc nem sempre expõe de forma estável
// entre versões. Chamamos direto por syscall() para não depender de wrapper.
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/types.h>
#include <csignal>

namespace sc {

inline int pidfd_open(pid_t pid, unsigned int flags) {
    return static_cast<int>(::syscall(__NR_pidfd_open, pid, flags));
}

inline int pidfd_send_signal(int pidfd, int sig, siginfo_t *info, unsigned int flags) {
    return static_cast<int>(::syscall(__NR_pidfd_send_signal, pidfd, sig, info, flags));
}

// Duplica o fd 'targetfd' do processo apontado por 'pidfd' para DENTRO de nós.
// NÃO fecha o fd do dono — só rouba/inspeciona (memfd, socket, eventfd...).
inline int pidfd_getfd(int pidfd, int targetfd, unsigned int flags) {
    return static_cast<int>(::syscall(__NR_pidfd_getfd, pidfd, targetfd, flags));
}

} // namespace sc
