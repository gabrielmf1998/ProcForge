// Self-test do núcleo de memória: escaneia a própria memória, refina e escreve.
#include "core/MemoryScanner.h"
#include "core/MemoryIO.h"

#include <QCoreApplication>
#include <cstdio>
#include <unistd.h>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int pid = ::getpid();

    // Valores-alvo no heap (região rw).
    int *target = new int(1337);
    int *decoy1 = new int(42);
    int *decoy2 = new int(1337);   // um segundo 1337, deve cair no refino
    const quint64 addr  = reinterpret_cast<quint64>(target);
    const quint64 addr2 = reinterpret_cast<quint64>(decoy2);
    (void)decoy1;

    MemoryScanner s(pid);
    s.setType(ScanType::Int32);

    int err = 0;
    const long n1 = s.firstScanExact(QStringLiteral("1337"), &err);
    const bool found = s.contains(addr) && s.contains(addr2);
    std::printf("1) primeira busca (=1337): %ld candidatos, err=%d, achou os dois alvos=%d\n",
                n1, err, int(found));

    // Muda o valor do alvo e refina por igualdade ao novo valor.
    *target = 4242;
    const long n2 = s.refine(RefineMode::Exact, QStringLiteral("4242"), &err);
    const bool refined = s.contains(addr) && !s.contains(addr2);
    std::printf("2) refino (=4242): %ld candidatos, sobrou só o alvo=%d\n", n2, int(refined));

    // Escreve 9999 via process_vm_writev e relê pelo ponteiro real.
    int val = 9999;
    const ssize_t w = mem::writev(pid, addr, &val, sizeof val);
    const bool wrote = (w == ssize_t(sizeof val)) && (*target == 9999);
    std::printf("3) escrita (9999) via process_vm_writev: w=%zd, *target=%d\n", w, *target);

    // Relê via process_vm_readv para fechar o ciclo R/W.
    int back = 0;
    mem::readv(pid, addr, &back, sizeof back);
    const bool readback = (back == 9999);
    std::printf("4) releitura via process_vm_readv: %d\n", back);

    const bool pass = found && refined && wrote && readback && n1 >= 2 && n2 >= 1;
    std::printf("\n%s\n", pass ? "SELFTEST PASS ✓" : "SELFTEST FAIL ✗");
    return pass ? 0 : 1;
}
