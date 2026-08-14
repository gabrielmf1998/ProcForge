// CLI de teste da injeção de biblioteca: ./procforge_inject <pid> <caminho.so>
#include "inject/LibraryInjector.h"
#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::fprintf(stderr, "uso: %s <pid> <caminho.so>\n", argv[0]);
        return 2;
    }
    const int pid = std::atoi(argv[1]);
    const inject::Result r = inject::injectLibrary(pid, QString::fromUtf8(argv[2]));
    if (r.ok) {
        std::printf("OK: dlopen handle=0x%lx\n", static_cast<unsigned long>(r.retval));
        return 0;
    }
    std::printf("FALHOU: %s\n", qPrintable(r.error));
    return 1;
}
