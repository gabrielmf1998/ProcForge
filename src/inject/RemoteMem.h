#pragma once
#include <QString>
#include <QtGlobal>

// Manipulação das PÁGINAS de memória de um processo alheio: aloca (mmap),
// muda proteção (mprotect) e libera (munmap) regiões — o que o Process Hacker
// oferece no tab de memória ("Protect…", "Free"). Diferente de ReadMem/WriteMem
// (que só tocam o conteúdo), aqui alteramos o mapa de memória do alvo.
//
// Mecânica: PTRACE_SEIZE + hijack de uma thread para CHAMAR a função libc
// correspondente DENTRO do alvo (mesma libc.so.6 -> mesmo offset), com retorno
// por sentinela (SIGSEGV em rip=0), e restauração de registradores. Requer poder
// ptrace sobre o alvo (mesmo uid com ptrace_scope=0, ou helper com CAP_SYS_PTRACE).
namespace inject {

struct MemResult {
    bool    ok = false;
    QString error;
    quint64 ret = 0;   // mmap: endereço da região; protect/free: 0 em sucesso
};

// prot = OR de PROT_READ(1)/PROT_WRITE(2)/PROT_EXEC(4). length em bytes.
MemResult remoteMmap(int pid, quint64 length, int prot);          // MAP_PRIVATE|ANON, fd=-1
MemResult remoteMprotect(int pid, quint64 addr, quint64 length, int prot);
MemResult remoteMunmap(int pid, quint64 addr, quint64 length);

} // namespace inject
