// fakeapp: processo falso e inofensivo usado pelos testes do ProcForge.
// Cada modo imprime uma linha "READY chave=valor ..." no stdout e espera.
//
//   fakeapp tree N D       árvore com N filhos por nível e D níveis (todos dormem)
//   fakeapp stubborn       ignora SIGTERM/SIGINT/SIGHUP (só SIGKILL mata)
//   fakeapp value V        guarda um int32 V no heap; SIGUSR1 soma 1, SIGUSR2 imprime
//   fakeapp cpu            queima CPU num loop (para testar %CPU/afinidade/nice)
//   fakeapp lock ARQ       abre ARQ, pega flock exclusivo e segura o fd
//   fakeapp listen         escuta TCP em 127.0.0.1 numa porta livre
//   fakeapp threads N      cria N threads que dormem
//   fakeapp io MB [DIR]    escreve MB megabytes num arquivo temporário em DIR e espera
//
// Só escreve nos caminhos que o teste passar e encerra sozinho após 10 minutos.
#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

volatile int *g_value = nullptr;
volatile sig_atomic_t g_print = 0;

void onUsr1(int) { if (g_value) *g_value = *g_value + 1; }
void onUsr2(int) { g_print = 1; }

void ready(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ready(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf("READY pid=%d ", int(::getpid()));
    std::vprintf(fmt, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}

[[noreturn]] void idle()
{
    for (int i = 0; i < 600; ++i) {
        if (g_print && g_value) {
            g_print = 0;
            std::printf("VALUE %d\n", *g_value);
            std::fflush(stdout);
        }
        ::sleep(1);
    }
    std::_Exit(0);
}

// Cria D níveis com N filhos cada. Os filhos morrem junto com o pai (PDEATHSIG)
// para nunca sobrar processo órfão caso o teste falhe no meio.
void spawnTree(int n, int depth)
{
    if (depth <= 0) return;
    for (int i = 0; i < n; ++i) {
        const pid_t c = ::fork();
        if (c == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            ::prctl(PR_SET_NAME, "fakeapp-child");
            spawnTree(n, depth - 1);
            idle();
        }
    }
}

void *sleeper(void *) { for (;;) ::sleep(60); return nullptr; }

} // namespace

int main(int argc, char **argv)
{
    std::signal(SIGPIPE, SIG_IGN);
    const char *mode = argc > 1 ? argv[1] : "";

    if (!std::strcmp(mode, "tree")) {
        const int n = argc > 2 ? std::atoi(argv[2]) : 2;
        const int d = argc > 3 ? std::atoi(argv[3]) : 2;
        spawnTree(n, d);
        // Total de descendentes: n + n^2 + ... + n^d.
        int total = 0, lvl = 1;
        for (int i = 0; i < d; ++i) { lvl *= n; total += lvl; }
        ready("descendants=%d", total);
        idle();
    }
    if (!std::strcmp(mode, "stubborn")) {
        std::signal(SIGTERM, SIG_IGN);
        std::signal(SIGINT, SIG_IGN);
        std::signal(SIGHUP, SIG_IGN);
        ready("ignores=TERM,INT,HUP");
        idle();
    }
    if (!std::strcmp(mode, "value")) {
        g_value = new int(argc > 2 ? std::atoi(argv[2]) : 1337);
        std::signal(SIGUSR1, onUsr1);
        std::signal(SIGUSR2, onUsr2);
        ready("addr=0x%llx value=%d", (unsigned long long)(uintptr_t)g_value, *g_value);
        idle();
    }
    if (!std::strcmp(mode, "cpu")) {
        ready("burning=1");
        volatile unsigned long x = 0;
        for (;;) {
            for (int i = 0; i < 50'000'000; ++i) x += i;
            if (::getppid() == 1) return int(x & 1);   // pai sumiu: não fica queimando CPU
        }
    }
    if (!std::strcmp(mode, "lock")) {
        const char *path = argc > 2 ? argv[2] : "/tmp/fakeapp.lock";
        const int fd = ::open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0 || ::flock(fd, LOCK_EX | LOCK_NB) != 0) { std::perror("lock"); return 2; }
        ready("fd=%d path=%s", fd, path);
        idle();
    }
    if (!std::strcmp(mode, "listen")) {
        const int s = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (s < 0 || ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 4) != 0) {
            std::perror("listen");
            return 2;
        }
        socklen_t len = sizeof a;
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        ready("fd=%d port=%d", s, int(ntohs(a.sin_port)));
        idle();
    }
    if (!std::strcmp(mode, "threads")) {
        const int n = argc > 2 ? std::atoi(argv[2]) : 4;
        for (int i = 0; i < n; ++i) {
            pthread_t t;
            pthread_create(&t, nullptr, sleeper, nullptr);
            pthread_detach(t);
        }
        ready("threads=%d", n + 1);
        idle();
    }
    if (!std::strcmp(mode, "io")) {
        const int mb = argc > 2 ? std::atoi(argv[2]) : 4;
        // /tmp costuma ser tmpfs (não conta write_bytes): o teste passa uma pasta em disco.
        std::string tmpl = std::string(argc > 3 ? argv[3] : "/tmp") + "/fakeapp-io-XXXXXX";
        const int fd = ::mkstemp(tmpl.data());
        if (fd < 0) { std::perror("mkstemp"); return 2; }
        ::unlink(tmpl.c_str());
        static char buf[1 << 20];
        std::memset(buf, 'x', sizeof buf);
        for (int i = 0; i < mb; ++i)
            if (::write(fd, buf, sizeof buf) < 0) break;
        ::fdatasync(fd);
        ready("written_mb=%d fd=%d", mb, fd);
        idle();
    }
    std::fprintf(stderr, "uso: fakeapp tree|stubborn|value|cpu|lock|listen|threads|io ...\n");
    return 1;
}
