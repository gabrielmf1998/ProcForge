#pragma once
#include <QString>

// Suspensão de UMA thread (TID) via ptrace. SIGSTOP é por-processo; para congelar
// uma thead isolada usamos PTRACE_SEIZE+PTRACE_INTERRUPT e MANTEMOS o attach vivo
// (a thread só volta quando o tracer faz PTRACE_DETACH). Cada TID suspensa fica
// segurada por uma thread-tracer dedicada aqui dentro. Mesmo uid (ptrace_scope=0).
// Singleton: as suspensões persistem enquanto a GUI viver.
class ThreadController {
public:
    static ThreadController &instance();

    bool suspend(int tid, QString *err);
    bool resume(int tid, QString *err);
    bool isSuspended(int tid);

    ThreadController(const ThreadController &) = delete;
    ThreadController &operator=(const ThreadController &) = delete;

private:
    ThreadController() = default;
    ~ThreadController();
};
