// Teste do suspend por-thread: ./procforge_tsuspend <tid> [segundos]
// Suspende a thread, segura por N segundos (o tracer fica vivo aqui) e retoma.
#include "core/ThreadController.h"
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "uso: %s <tid> [segundos]\n", argv[0]);
        return 2;
    }
    const int tid  = std::atoi(argv[1]);
    const int secs = (argc >= 3) ? std::atoi(argv[2]) : 3;

    QString err;
    if (!ThreadController::instance().suspend(tid, &err)) {
        std::printf("suspend FALHOU: %s\n", qPrintable(err));
        return 1;
    }
    std::printf("SUSPENSA tid=%d\n", tid);
    std::fflush(stdout);

    ::sleep(secs);

    ThreadController::instance().resume(tid, &err);
    std::printf("RETOMADA tid=%d\n", tid);
    std::fflush(stdout);
    return 0;
}
