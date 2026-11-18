// Testes de integração: ações reais do ProcForge contra apps falsos (fakeapp).
#include "FakeApp.h"

#include "actions/ProcessActions.h"
#include "core/MemoryIO.h"
#include "core/MemoryScanner.h"
#include "core/ThreadController.h"
#include "inject/FdInjector.h"

#include <KLocalizedString>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTest>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>

class TestActions : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase() { KLocalizedString::setApplicationDomain(QByteArrayLiteral("procforge")); }

    void killTree()
    {
        FakeApp app({QStringLiteral("tree"), QStringLiteral("2"), QStringLiteral("2")});
        QVERIFY(app.ready());
        const int expected = app.value(QStringLiteral("descendants")).toInt();
        QCOMPARE(expected, 6);
        QTRY_COMPARE_WITH_TIMEOUT(int(descendantsOf(app.pid()).size()), expected, 3000);
        const QList<int> kids = descendantsOf(app.pid());
        QHash<int, quint64> starts;
        for (int k : kids) { procfs::StatFields st; procfs::readStat(k, st); starts[k] = st.starttime; }

        const actions::Result r = actions::killTree(app.pid());
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(app.proc().waitForFinished(3000));
        for (int k : kids)
            QVERIFY2(waitGone(k, starts[k]), qPrintable(QStringLiteral("descendente %1 sobreviveu").arg(k)));
    }

    void signalIdentity()
    {
        FakeApp app({QStringLiteral("stubborn")});
        QVERIFY(app.ready());
        // starttime errado: o ProcForge tem que recusar (proteção contra reuso de PID).
        const actions::Result wrong = actions::sendSignal(app.pid(), app.starttime() + 1, SIGKILL);
        QVERIFY(!wrong.ok);
        QTest::qWait(200);
        QVERIFY(!isGone(app.pid(), app.starttime()));

        // SIGTERM é ignorado pelo fakeapp stubborn: entregue, mas ele continua vivo.
        QVERIFY(actions::sendSignal(app.pid(), app.starttime(), SIGTERM).ok);
        QTest::qWait(300);
        QVERIFY(!isGone(app.pid(), app.starttime()));

        QVERIFY(actions::sendSignal(app.pid(), app.starttime(), SIGKILL).ok);
        QVERIFY(app.proc().waitForFinished(3000));
    }

    void suspendResume()
    {
        FakeApp app({QStringLiteral("cpu")});
        QVERIFY(app.ready());
        QVERIFY(actions::suspend(app.pid(), app.starttime()).ok);
        QTRY_COMPARE_WITH_TIMEOUT(FakeApp::stateOf(app.pid()), 'T', 2000);
        QVERIFY(actions::resume(app.pid(), app.starttime()).ok);
        QTRY_VERIFY_WITH_TIMEOUT(FakeApp::stateOf(app.pid()) == 'R' || FakeApp::stateOf(app.pid()) == 'S', 2000);
    }

    void threadSuspend()
    {
        FakeApp app({QStringLiteral("threads"), QStringLiteral("3")});
        QVERIFY(app.ready());
        QTRY_COMPARE_WITH_TIMEOUT(int(QDir(QStringLiteral("/proc/%1/task").arg(app.pid()))
                                          .entryList(QDir::Dirs | QDir::NoDotAndDotDot).size()), 4, 2000);
        QStringList tids = QDir(QStringLiteral("/proc/%1/task").arg(app.pid()))
                               .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        tids.removeAll(QString::number(app.pid()));
        const int tid = tids.first().toInt();
        QString err;
        QVERIFY2(ThreadController::instance().suspend(tid, &err), qPrintable(err));
        QVERIFY(ThreadController::instance().isSuspended(tid));
        QVERIFY2(ThreadController::instance().resume(tid, &err), qPrintable(err));
        QVERIFY(!ThreadController::instance().isSuspended(tid));
        QVERIFY(!isGone(app.pid(), app.starttime()));
    }

    void schedulingAndLimits()
    {
        FakeApp app({QStringLiteral("value"), QStringLiteral("1")});
        QVERIFY(app.ready());
        // Aumentar o nice (diminuir prioridade) é sempre permitido ao dono.
        QVERIFY(actions::renice(app.pid(), 7).ok);
        bool ok = false;
        QCOMPARE(actions::getNice(app.pid(), &ok), 7);
        QVERIFY(ok);
        // Baixar de novo exige privilégio sem CAP_SYS_NICE: tem que pedir o helper, não falhar mudo.
        const actions::Result back = actions::renice(app.pid(), 0);
        QVERIFY(back.ok || back.needsPrivilege());

        QVERIFY(actions::setAffinity(app.pid(), {0}).ok);
        QCOMPARE(actions::getAffinity(app.pid()), QList<int>{0});

        QVERIFY(actions::setRlimit(app.pid(), RLIMIT_NOFILE, 64, 128).ok);
        quint64 soft = 0, hard = 0;
        QVERIFY(actions::getRlimit(app.pid(), RLIMIT_NOFILE, &soft, &hard));
        QCOMPARE(soft, quint64(64));
        QCOMPARE(hard, quint64(128));

        QVERIFY(actions::setIoPrio(app.pid(), 3, 0).ok);    // classe Idle
        int cls = 0;
        actions::getIoPrio(app.pid(), &cls);
        QCOMPARE(cls, 3);
    }

    void memoryScanner()
    {
        FakeApp app({QStringLiteral("value"), QStringLiteral("271828")});
        QVERIFY(app.ready());
        const quint64 addr = app.value(QStringLiteral("addr")).toULongLong(nullptr, 16);
        QVERIFY(addr);

        MemoryScanner sc(app.pid());
        sc.setType(ScanType::Int32);
        int err = 0;
        QVERIFY(sc.firstScanExact(QStringLiteral("271828"), &err) > 0);
        QVERIFY2(sc.contains(addr), "primeira busca não achou o endereço do fakeapp");

        ::kill(app.pid(), SIGUSR1);   // fakeapp soma 1
        QTest::qWait(300);
        sc.refine(RefineMode::Increased, QString(), &err);
        QVERIFY(sc.contains(addr));
        sc.refine(RefineMode::Exact, QStringLiteral("271829"), &err);
        QVERIFY(sc.contains(addr));
        QVERIFY(sc.count() < 50);

        const int v = 4242;
        QCOMPARE(mem::writev(app.pid(), addr, &v, sizeof v), ssize_t(sizeof v));
        ::kill(app.pid(), SIGUSR2);   // fakeapp imprime o valor
        QCOMPARE(app.waitLine(QStringLiteral("VALUE")), QStringLiteral("VALUE 4242"));
        QCOMPARE(sc.formatValue(sc.readRaw(addr)), QStringLiteral("4242"));
    }

    void closeForeignFd()
    {
        const QString lock = QDir::tempPath() + QStringLiteral("/procforge-test-%1.lock").arg(::getpid());
        FakeApp app({QStringLiteral("lock"), lock});
        QVERIFY(app.ready());
        const int fd = app.value(QStringLiteral("fd")).toInt();
        QVERIFY(QFile::exists(QStringLiteral("/proc/%1/fd/%2").arg(app.pid()).arg(fd)));

        // Enquanto o fakeapp segura o flock, nós não conseguimos pegar.
        const int mine = ::open(QFile::encodeName(lock).constData(), O_RDWR | O_CLOEXEC);
        QVERIFY(mine >= 0);
        QVERIFY(::flock(mine, LOCK_EX | LOCK_NB) != 0);

        const inject::Result r = inject::closeForeignFd(app.pid(), fd);
        QVERIFY2(r.ok, qPrintable(r.error));
        QVERIFY(!QFile::exists(QStringLiteral("/proc/%1/fd/%2").arg(app.pid()).arg(fd)));
        QVERIFY2(::flock(mine, LOCK_EX | LOCK_NB) == 0, "a trava não foi liberada");
        QVERIFY(!isGone(app.pid(), app.starttime()));   // o dono continua vivo
        ::close(mine);
        QFile::remove(lock);
    }

    void dupForeignFd()
    {
        FakeApp app({QStringLiteral("listen")});
        QVERIFY(app.ready());
        const inject::Result r = inject::dupForeignFd(app.pid(), app.value(QStringLiteral("fd")).toInt());
        if (!r.ok && r.error.contains(QLatin1String("pidfd_getfd")))
            QSKIP("kernel sem pidfd_getfd");
        QVERIFY2(r.ok, qPrintable(r.error));
        struct stat sb{};
        QVERIFY(::fstat(int(r.retval), &sb) == 0);
        QVERIFY(S_ISSOCK(sb.st_mode));
        ::close(int(r.retval));
    }

    void ioRates()
    {
        FakeApp app({QStringLiteral("io"), QStringLiteral("2"), QStringLiteral(TEST_DISK_DIR)});
        QVERIFY(app.ready());
        quint64 rd = 0, wr = 0;
        QVERIFY(procfs::ioBytesOf(app.pid(), &rd, &wr));
        QVERIFY2(wr >= 2ull * 1024 * 1024, qPrintable(QStringLiteral("write_bytes=%1").arg(wr)));

        // O arquivo foi apagado mas segue aberto no fakeapp: dá para recuperar o conteúdo.
        const int fd = app.value(QStringLiteral("fd")).toInt();
        const inject::Result r = inject::dupForeignFd(app.pid(), fd);
        QVERIFY2(r.ok, qPrintable(r.error));
        struct stat sb{};
        QVERIFY(::fstat(int(r.retval), &sb) == 0);
        QCOMPARE(qint64(sb.st_size), qint64(2) * 1024 * 1024);
        char c = 0;
        QCOMPARE(::pread(int(r.retval), &c, 1, 12345), ssize_t(1));
        QCOMPARE(c, 'x');
        ::close(int(r.retval));
    }
};

QTEST_GUILESS_MAIN(TestActions)
#include "test_actions.moc"
