#pragma once
#include <QString>
#include <QList>
#include <cstdint>
#include <cerrno>

// Ações de manipulação executadas DIRETO pela GUI (mesmo-uid, sem privilégio).
// Cruzar uid/root fica para o helper D-Bus (fase 1). Cada função devolve o
// resultado com a mensagem de erro do sistema quando falha (ex.: EPERM).
namespace actions {

struct Result {
    bool    ok = false;
    QString error;
    QString detail;
    int     err = 0;   // errno quando falha (para decidir fallback ao helper)
    static Result good(const QString &d = {})            { return {true, {}, d, 0}; }
    static Result fail(const QString &e)                 { return {false, e, {}, 0}; }
    static Result failErr(const QString &e, int errcode) { return {false, e, {}, errcode}; }
    bool needsPrivilege() const { return err == EPERM || err == EACCES; }
};

// Verifica identidade (anti-reuso de PID) e envia sinal via pidfd.
Result sendSignal(int pid, quint64 starttime, int sig);
Result suspend(int pid, quint64 starttime);   // SIGSTOP
Result resume(int pid, quint64 starttime);    // SIGCONT

// Congela a árvore com SIGSTOP, coleta descendentes por PPID e SIGKILL em todos.
Result killTree(int rootPid);

// Escalonamento / prioridade ao vivo.
Result   renice(int pid, int nice);
int      getNice(int pid, bool *ok = nullptr);

QList<int> onlineCpus();          // [0 .. nproc-1]
QList<int> getAffinity(int pid);  // CPUs atualmente na máscara
Result     setAffinity(int pid, const QList<int> &cpus);

// Política de escalonamento (SCHED_OTHER/FIFO/RR/BATCH/IDLE) — rtPriority só p/ FIFO/RR.
Result setScheduler(int pid, int policy, int rtPriority);
int    getScheduler(int pid, int *rtPriority);       // retorna a policy atual

// Classe/prioridade de I/O (ionice): class 1=RT, 2=BestEffort, 3=Idle, 0=None.
Result setIoPrio(int pid, int ioClass, int prio);
int    getIoPrio(int pid, int *ioClass);             // retorna a prioridade

// Limites (rlimit) de um processo já rodando (prlimit). ~0ull = ilimitado.
Result   setRlimit(int pid, int resource, quint64 soft, quint64 hard);
bool     getRlimit(int pid, int resource, quint64 *soft, quint64 *hard);

} // namespace actions
