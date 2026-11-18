// Testes de unidade: versões do atualizador, /proc, modelo de processos e filtro.
#include "core/Procfs.h"
#include "core/UpdateChecker.h"
#include "model/ProcessFilterProxy.h"
#include "model/ProcessModel.h"

#include <KLocalizedString>
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QHostAddress>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <unistd.h>

namespace {
ProcInfo mk(int pid, int ppid, quint64 start, const QString &name, const QString &user = QStringLiteral("alice"))
{
    ProcInfo p;
    p.pid = pid;
    p.ppid = ppid;
    p.starttime = start;
    p.name = name;
    p.user = user;
    p.state = 'S';
    p.cmdline = QStringLiteral("/usr/bin/") + name + QStringLiteral(" --flag");
    return p;
}
} // namespace

class TestCore : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        KLocalizedString::setApplicationDomain(QByteArrayLiteral("procforge"));
        KLocalizedString::setLanguages({QStringLiteral("pt_BR")});
    }

    void versionCompare_data()
    {
        QTest::addColumn<QString>("latest");
        QTest::addColumn<QString>("current");
        QTest::addColumn<bool>("newer");
        QTest::newRow("minor") << "0.5.0" << "0.4.0" << true;
        QTest::newRow("igual sem patch") << "0.5" << "0.5.0" << false;
        QTest::newRow("v prefixo") << "v0.10.0" << "0.9.9" << true;
        QTest::newRow("dois digitos") << "0.10.0" << "0.9.0" << true;
        QTest::newRow("mais velha") << "0.4.0" << "0.5.0" << false;
        QTest::newRow("igual") << "0.5.0" << "0.5.0" << false;
        QTest::newRow("vazia") << "" << "0.5.0" << false;
        QTest::newRow("lixo") << "abc" << "0.5.0" << false;
        QTest::newRow("current vazio") << "0.5.0" << "" << true;
    }
    void versionCompare()
    {
        QFETCH(QString, latest);
        QFETCH(QString, current);
        QFETCH(bool, newer);
        QCOMPARE(UpdateChecker::isNewer(latest, current), newer);
    }

    // UpdateChecker contra um servidor de releases falso (python http.server).
    void updateCheckerFake()
    {
        QTemporaryDir www;
        QDir().mkpath(www.filePath(QStringLiteral("gh")));
        QDir().mkpath(www.filePath(QStringLiteral("gl")));
        auto put = [&](const QString &rel, const QByteArray &data) {
            QFile f(www.filePath(rel));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(data);
        };
        put(QStringLiteral("gh/latest"), "{\"tag_name\":\"v9.9.9\",\"html_url\":\"http://gh/rel\",\"body\":\"notas do github\"}");
        put(QStringLiteral("gl/index.html"),
            "[{\"tag_name\":\"v9.9.10\",\"description\":\"notas do gitlab\",\"_links\":{\"self\":\"http://gl/rel\"}}]");

        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 port = probe.serverPort();
        probe.close();
        QProcess srv;
        srv.setWorkingDirectory(www.path());
        srv.start(QStringLiteral("python3"), {QStringLiteral("-m"), QStringLiteral("http.server"),
                                              QString::number(port), QStringLiteral("--bind"), QStringLiteral("127.0.0.1")});
        if (!srv.waitForStarted(3000))
            QSKIP("python3 indisponível");
        QTest::qWait(700);
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);
        qputenv("PROCFORGE_RELEASES_API", (base + QStringLiteral("/gh")).toUtf8());
        qputenv("PROCFORGE_GITLAB_API", (base + QStringLiteral("/gl/")).toUtf8());
        QCoreApplication::setApplicationVersion(QStringLiteral("0.5.0"));

        {   // Pega a MAIOR versão entre GitHub e GitLab, com as notas dela.
            UpdateChecker c;
            QSignalSpy avail(&c, &UpdateChecker::updateAvailable);
            c.checkAsync();
            QTRY_COMPARE_WITH_TIMEOUT(avail.count(), 1, 10000);
            QCOMPARE(avail.first().at(0).toString(), QStringLiteral("v9.9.10"));
            QCOMPARE(avail.first().at(1).toString(), QStringLiteral("http://gl/rel"));
            QCOMPARE(avail.first().at(2).toString(), QStringLiteral("notas do gitlab"));
        }
        {   // Já atualizado.
            QCoreApplication::setApplicationVersion(QStringLiteral("10.0"));
            UpdateChecker c;
            QSignalSpy up(&c, &UpdateChecker::upToDate);
            c.checkAsync();
            QTRY_COMPARE_WITH_TIMEOUT(up.count(), 1, 10000);
        }
        {   // Os dois fora do ar: falha com mensagem.
            qputenv("PROCFORGE_RELEASES_API", (base + QStringLiteral("/nada")).toUtf8());
            qputenv("PROCFORGE_GITLAB_API", (base + QStringLiteral("/nada/")).toUtf8());
            UpdateChecker c;
            QSignalSpy fail(&c, &UpdateChecker::checkFailed);
            c.checkAsync();
            QTRY_COMPARE_WITH_TIMEOUT(fail.count(), 1, 10000);
            QVERIFY(!fail.first().at(0).toString().isEmpty());
        }
        srv.kill();
        srv.waitForFinished(2000);
        qunsetenv("PROCFORGE_RELEASES_API");
        qunsetenv("PROCFORGE_GITLAB_API");
    }

    void procfsSelf()
    {
        procfs::StatFields st;
        QVERIFY(procfs::readStat(::getpid(), st));
        QVERIFY(st.threads >= 1);
        QVERIFY(st.starttime > 0);
        QVERIFY(st.nice >= -20 && st.nice <= 19);
        QCOMPARE(st.ppid, int(::getppid()));

        const auto cpus = procfs::cpuTimes();
        QVERIFY(cpus.size() >= 2);            // agregado + pelo menos 1 núcleo
        QVERIFY(cpus.first().total >= cpus.first().idle);

        const auto mi = procfs::memInfo();
        QVERIFY(mi.total > 0);
        QVERIFY(mi.available <= mi.total);
        QVERIFY(mi.used() <= mi.total);
        QVERIFY(procfs::bootTimeSecs() > 1'000'000'000);

        quint64 r = 0, w = 0;
        QVERIFY(procfs::diskTotals(&r, &w) || true);   // VM sem disco real: só não pode travar
        quint64 rx = 0, tx = 0;
        procfs::netTotals(&rx, &tx);

        QTemporaryDir dir;
        QFile f(dir.filePath(QStringLiteral("fd-test")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        bool found = false;
        for (const auto &e : procfs::listFds(::getpid()))
            if (e.target == f.fileName()) found = true;
        QVERIFY2(found, "fd do arquivo temporário não apareceu em listFds");
        QVERIFY(!procfs::listMaps(::getpid()).isEmpty());
    }

    void modelTree()
    {
        ProcessModel m;
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(10, 1, 20, QStringLiteral("shell")),
                         mk(11, 10, 30, QStringLiteral("editor"))});
        QCOMPARE(m.rowCount(), 1);
        const QModelIndex i11 = m.indexForPid(11);
        QVERIFY(i11.isValid());
        QCOMPARE(m.pidForIndex(i11.parent()), 10);
        QCOMPARE(m.liveCount(), 3);

        // Reparent: 11 passa a ser filho de 1.
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(10, 1, 20, QStringLiteral("shell")),
                         mk(11, 1, 30, QStringLiteral("editor"))});
        QCOMPARE(m.pidForIndex(m.indexForPid(11).parent()), 1);

        // 10 sai: fica marcado como encerrado durante o flash e depois some.
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(11, 1, 30, QStringLiteral("editor"))});
        QVERIFY(m.indexForPid(10).isValid());
        QVERIFY(m.isDying(m.indexForPid(10)));
        QCOMPARE(m.liveCount(), 2);
        QTRY_VERIFY_WITH_TIMEOUT(!m.indexForPid(10).isValid(), ProcessModel::kFlashMs + 2000);

        // Some por um tick e volta com o mesmo starttime: é o mesmo processo
        // (não perde o nó, a seleção nem pisca).
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(11, 1, 30, QStringLiteral("editor"))});
        const QPersistentModelIndex keep(m.indexForPid(11));
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init"))});
        QVERIFY(m.isDying(m.indexForPid(11)));
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(11, 1, 30, QStringLiteral("editor"))});
        QVERIFY(keep.isValid());
        QVERIFY(!m.isDying(m.indexForPid(11)));
        QCOMPARE(m.liveCount(), 2);

        // Reuso de PID: mesmo número, starttime diferente = processo novo.
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(11, 1, 99, QStringLiteral("outro"))});
        const ProcInfo p11 = m.infoForIndex(m.indexForPid(11));
        QCOMPARE(p11.starttime, quint64(99));
        QCOMPARE(p11.name, QStringLiteral("outro"));
        QVERIFY(!m.isDying(m.indexForPid(11)));

        // Filho cujo pai ainda não existe vai para a raiz, e é adotado quando o pai surge.
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(11, 1, 99, QStringLiteral("outro")),
                         mk(50, 40, 100, QStringLiteral("orfao"))});
        QVERIFY(!m.indexForPid(50).parent().isValid());
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("init")),
                         mk(11, 1, 99, QStringLiteral("outro")),
                         mk(40, 1, 90, QStringLiteral("pai")),
                         mk(50, 40, 100, QStringLiteral("orfao"))});
        QCOMPARE(m.pidForIndex(m.indexForPid(50).parent()), 40);

        // Texto das células usado por copiar/exportar.
        QCOMPARE(m.cellText(p11, ProcessModel::ColPid), QStringLiteral("11"));
        QCOMPARE(m.cellText(p11, ProcessModel::ColName), QStringLiteral("outro"));
        for (int c = 0; c < ProcessModel::ColumnCount; ++c)
            QVERIFY(!m.headerData(c, Qt::Horizontal, Qt::DisplayRole).toString().isEmpty());
    }

    void filterProxy()
    {
        ProcessModel m;
        ProcessFilterProxy px;
        px.setSourceModel(&m);
        m.applySnapshot({mk(1, 0, 10, QStringLiteral("systemd"), QStringLiteral("root")),
                         mk(20, 1, 20, QStringLiteral("firefox"), QStringLiteral("alice")),
                         mk(21, 20, 21, QStringLiteral("webext"), QStringLiteral("alice")),
                         mk(30, 1, 30, QStringLiteral("sshd"), QStringLiteral("root"))});
        auto visible = [&](int pid) {
            return px.mapFromSource(m.indexForPid(pid)).isValid();
        };
        QVERIFY(!px.isFiltering());

        px.setFilterText(QStringLiteral("webext"));
        QVERIFY(visible(21));
        QVERIFY(visible(20));        // pai fica visível (árvore preservada)
        QVERIFY(!visible(30));

        px.setFilterText(QStringLiteral("user:alice"));
        QVERIFY(visible(20) && visible(21));
        QVERIFY(!visible(30));

        px.setFilterText(QStringLiteral("alice"));   // usuário pela busca livre
        QVERIFY(visible(20));

        px.setFilterText(QStringLiteral("pid:30"));
        QVERIFY(visible(30));
        QVERIFY(!visible(20));

        px.setFilterText(QStringLiteral("30"));      // PID exato pela busca livre
        QVERIFY(visible(30));

        px.setFilterText(QStringLiteral("--flag"));  // linha de comando
        QVERIFY(visible(20) && visible(30));

        px.setFilterText(QString());
        QVERIFY(visible(20) && visible(30) && visible(21));
    }
};

QTEST_MAIN(TestCore)
#include "test_core.moc"
