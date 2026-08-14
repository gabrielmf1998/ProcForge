#include "ThreadController.h"

#include <QHash>

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

#include <cerrno>
#include <cstring>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct Entry {
    std::thread             tracer;
    std::mutex              m;
    std::condition_variable cv;
    bool                    resumeRequested = false;
};

std::mutex                              g_mapMutex;
QHash<int, std::shared_ptr<Entry>>      g_map;

// Roda na thread-tracer: anexa, congela a TID, e SEGURA até pedirem resume.
void tracerFn(int tid, std::shared_ptr<Entry> e, std::shared_ptr<std::promise<QString>> attached)
{
    if (::ptrace(PTRACE_SEIZE, tid, 0, 0) < 0) {
        attached->set_value(QString::fromUtf8(::strerror(errno)));
        return;
    }
    if (::ptrace(PTRACE_INTERRUPT, tid, 0, 0) < 0) {
        int er = errno; ::ptrace(PTRACE_DETACH, tid, 0, 0);
        attached->set_value(QString::fromUtf8(::strerror(er)));
        return;
    }
    int status = 0;
    if (::waitpid(tid, &status, __WALL) < 0 || !WIFSTOPPED(status)) {
        ::ptrace(PTRACE_DETACH, tid, 0, 0);
        attached->set_value(QStringLiteral("a thread não parou"));
        return;
    }
    attached->set_value(QString()); // sucesso: TID congelada

    std::unique_lock<std::mutex> lk(e->m);
    e->cv.wait(lk, [&] { return e->resumeRequested; });
    lk.unlock();

    ::ptrace(PTRACE_DETACH, tid, 0, 0); // retoma a thread
}

} // namespace

ThreadController &ThreadController::instance()
{
    static ThreadController inst;
    return inst;
}

ThreadController::~ThreadController()
{
    // Retoma tudo que ficou suspenso ao encerrar.
    std::lock_guard<std::mutex> g(g_mapMutex);
    for (auto &e : g_map) {
        { std::lock_guard<std::mutex> lk(e->m); e->resumeRequested = true; }
        e->cv.notify_all();
        if (e->tracer.joinable()) e->tracer.join();
    }
    g_map.clear();
}

bool ThreadController::suspend(int tid, QString *err)
{
    std::lock_guard<std::mutex> g(g_mapMutex);
    if (g_map.contains(tid))
        return true; // já suspensa

    auto e    = std::make_shared<Entry>();
    auto prom = std::make_shared<std::promise<QString>>();
    auto fut  = prom->get_future();
    e->tracer = std::thread(tracerFn, tid, e, prom);

    const QString res = fut.get(); // espera o attach+stop
    if (!res.isEmpty()) {
        if (e->tracer.joinable()) e->tracer.join();
        if (err) *err = res;
        return false;
    }
    g_map.insert(tid, e);
    return true;
}

bool ThreadController::resume(int tid, QString *err)
{
    std::shared_ptr<Entry> e;
    {
        std::lock_guard<std::mutex> g(g_mapMutex);
        e = g_map.take(tid);
    }
    if (!e) {
        if (err) *err = QStringLiteral("a thread não estava suspensa");
        return false;
    }
    { std::lock_guard<std::mutex> lk(e->m); e->resumeRequested = true; }
    e->cv.notify_all();
    if (e->tracer.joinable()) e->tracer.join();
    return true;
}

bool ThreadController::isSuspended(int tid)
{
    std::lock_guard<std::mutex> g(g_mapMutex);
    return g_map.contains(tid);
}
