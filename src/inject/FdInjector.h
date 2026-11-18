#pragma once
#include <QString>

// Manipulação de descritores de arquivo de OUTRO processo.
// closeForeignFd: injeta close(fd) no alvo via ptrace — libera a trava
//   (lockfile, socket abstrato do singleton, eventfd) sem matar o dono.
//   Requer poder ptrace sobre o alvo (mesmo uid com ptrace_scope=0, ou helper).
// dupForeignFd: duplica o fd para dentro de nós via pidfd_getfd (não fecha o
//   do dono). Em caso de sucesso retval é o fd LOCAL aberto: quem chama fecha.
//   Usado para salvar uma cópia do arquivo (inclusive apagado ou memfd).
namespace inject {

struct Result {
    bool    ok = false;
    QString error;
    long    retval = 0;
};

Result closeForeignFd(int pid, int fd);
Result dupForeignFd(int pid, int fd);

} // namespace inject
