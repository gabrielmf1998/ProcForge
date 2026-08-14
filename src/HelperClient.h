#pragma once
#include "actions/ProcessActions.h"
#include <QString>
#include <QList>
#include <QByteArray>
#include <functional>

class QObject;

// Cliente do helper privilegiado (org.procforge.Helper1). Cada chamada pode
// disparar o diálogo do polkit no KDE. Usado como FALLBACK quando a ação direta
// da GUI falha com EPERM/EACCES (uid alheio, prioridade negativa, etc.).
namespace helper {

bool available();   // helper instalado/ativável no barramento de sistema?

actions::Result sendSignal(int pid, quint64 starttime, int sig);
actions::Result renice(int pid, int nice);
actions::Result setAffinity(int pid, const QList<int> &cpus);

QByteArray      readMem(int pid, quint64 addr, int len, bool *ok);
actions::Result writeMem(int pid, quint64 addr, const QByteArray &bytes);

actions::Result injectLibrary(int pid, quint64 starttime, const QString &path);
actions::Result cgroupThrottle(int pid, quint64 starttime, int cpuPercent,
                               quint64 memMaxBytes, quint64 pidsMax, quint64 ioMaxBps);
actions::Result cgroupRelease(int pid, quint64 starttime);
actions::Result setScheduler(int pid, quint64 starttime, int policy, int rtPriority);
actions::Result setIoPrio(int pid, quint64 starttime, int ioClass, int prio);
actions::Result setRlimit(int pid, quint64 starttime, int resource, quint64 soft, quint64 hard);

// Manipulação de páginas de memória (síncrono; usado pelo scanner/hex).
quint64         allocMem(int pid, quint64 starttime, quint64 length, int prot, QString *err);
actions::Result protectMem(int pid, quint64 starttime, quint64 addr, quint64 length, int prot);
actions::Result freeMem(int pid, quint64 starttime, quint64 addr, quint64 length);

QString nsRun(int pid, quint64 starttime, const QString &program,
              const QStringList &args, QString *err);
QString bpfTrace(int pid, quint64 starttime, const QString &mode, int seconds, QString *err);

// Eventos de processo em push (cn_proc). Conecta o sinal ProcEvent ao receptor
// e ativa a assinatura no helper. slot: SLOT(onProcEvent(uint,uint,uint)).
bool subscribeProcEvents(QObject *receiver, const char *slot);
void unsubscribeProcEvents();

// ---- variantes ASSÍNCRONAS: não bloqueiam o event loop da GUI ----
// O resultado chega no callback (na thread da GUI); 'ctx' garante segurança de
// vida (se morrer, o callback não é chamado). Assim o diálogo de senha do polkit
// nunca congela a janela.
using ResultCb = std::function<void(actions::Result)>;
void sendSignalAsync(int pid, quint64 starttime, int sig, QObject *ctx, ResultCb cb);
void reniceAsync(int pid, quint64 starttime, int nice, QObject *ctx, ResultCb cb);
void setAffinityAsync(int pid, quint64 starttime, const QList<int> &cpus, QObject *ctx, ResultCb cb);
void setSchedulerAsync(int pid, quint64 starttime, int policy, int rtPriority, QObject *ctx, ResultCb cb);
void setIoPrioAsync(int pid, quint64 starttime, int ioClass, int prio, QObject *ctx, ResultCb cb);
void setRlimitAsync(int pid, quint64 starttime, int resource, quint64 soft, quint64 hard, QObject *ctx, ResultCb cb);
void injectLibraryAsync(int pid, quint64 starttime, const QString &path, QObject *ctx, ResultCb cb);
void cgroupThrottleAsync(int pid, quint64 starttime, int cpuPercent, quint64 memMaxBytes,
                         quint64 pidsMax, quint64 ioMaxBps, QObject *ctx, ResultCb cb);
void cgroupReleaseAsync(int pid, quint64 starttime, QObject *ctx, ResultCb cb);

// Retornam texto (err vazio em sucesso), assíncronos.
using StrCb = std::function<void(const QString &out, const QString &err)>;
void bpfTraceAsync(int pid, quint64 starttime, const QString &mode, int seconds, QObject *ctx, StrCb cb);
void nsRunAsync(int pid, quint64 starttime, const QString &program, const QStringList &args, QObject *ctx, StrCb cb);

// "Executar como…": cria um novo processo. out recebe "pid <n>" em sucesso.
void launchProcessAsync(const QString &program, const QStringList &args,
                        const QString &username, const QString &cwd,
                        int nice, const QList<int> &affinity, QObject *ctx, StrCb cb);

} // namespace helper
