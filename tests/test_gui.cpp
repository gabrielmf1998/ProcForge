// Teste de fumaça da GUI (QT_QPA_PLATFORM=offscreen): sobe a janela principal,
// passa por todas as abas e janelas e confere que funcionam com apps falsos.
#include "FakeApp.h"

#include "MainWindow.h"
#include "model/ProcessModel.h"
#include "panels/FindHandlesDialog.h"
#include "panels/ProcessProperties.h"
#include "panels/ScannerWindow.h"
#include "panels/SysInfoWindow.h"
#include "panels/UpdateDialog.h"

#include <KLocalizedString>
#include <QAbstractItemModel>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTest>
#include <QTreeView>
#include <QHostAddress>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QFile>
#include <functional>

class TestGui : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        // Configurações isoladas: não mexe nas do usuário.
        QStandardPaths::setTestModeEnabled(true);
        QApplication::setOrganizationName(QStringLiteral("ProcForgeTest"));
        QApplication::setApplicationName(QStringLiteral("procforge-test"));
        QSettings().setValue(QStringLiteral("autoUpdateCheck"), false);
        KLocalizedString::setApplicationDomain(QByteArrayLiteral("procforge"));
        KLocalizedString::setLanguages({QStringLiteral("pt_BR")});
        qRegisterMetaType<ProcInfo>("ProcInfo");
        qRegisterMetaType<QList<ProcInfo>>("QList<ProcInfo>");
    }

    // Roda ANTES da janela principal: o caminho do executável fica fixo na 1ª
    // chamada, e aqui ele tem que ser o "AppImage" falso.
    void updateDialogAppImage()
    {
        QTemporaryDir tmp;
        const QString app = tmp.filePath(QStringLiteral("ProcForge-x86_64.AppImage"));
        auto mkapp = [](const QString &path, const QString &ver) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QStringLiteral("#!/bin/sh\necho \"procforge %1\"\n").arg(ver).toUtf8());
            f.close();
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        };
        mkapp(app, QStringLiteral("0.0.1"));
        QDir().mkpath(tmp.filePath(QStringLiteral("www/releases/tags")));
        mkapp(tmp.filePath(QStringLiteral("www/ProcForge-9.9.9-x86_64.AppImage")), QStringLiteral("9.9.9"));
        QProcess sum;
        sum.setWorkingDirectory(tmp.filePath(QStringLiteral("www")));
        sum.setStandardOutputFile(tmp.filePath(QStringLiteral("www/SHA256SUMS")));
        sum.start(QStringLiteral("sha256sum"), {QStringLiteral("ProcForge-9.9.9-x86_64.AppImage")});
        QVERIFY(sum.waitForFinished(5000));

        QTcpServer probe;
        QVERIFY(probe.listen(QHostAddress::LocalHost));
        const quint16 port = probe.serverPort();
        probe.close();
        const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);
        {
            QFile j(tmp.filePath(QStringLiteral("www/releases/tags/v9.9.9")));
            QVERIFY(j.open(QIODevice::WriteOnly));
            j.write(QStringLiteral("{\"tag_name\": \"v9.9.9\", \"assets\": ["
                                   "{\"browser_download_url\": \"%1/ProcForge-9.9.9-x86_64.AppImage\"},"
                                   "{\"browser_download_url\": \"%1/SHA256SUMS\"}]}").arg(base).toUtf8());
        }
        QProcess srv;
        srv.setWorkingDirectory(tmp.filePath(QStringLiteral("www")));
        srv.start(QStringLiteral("python3"), {QStringLiteral("-m"), QStringLiteral("http.server"),
                                              QString::number(port), QStringLiteral("--bind"), QStringLiteral("127.0.0.1")});
        if (!srv.waitForStarted(3000))
            QSKIP("python3 indisponível");
        QTest::qWait(700);

        qputenv("APPIMAGE", app.toUtf8());
        qputenv("PROCFORGE_RELEASES_API", (base + QStringLiteral("/releases")).toUtf8());
        qputenv("PROCFORGE_GITLAB_API", (base + QStringLiteral("/nada")).toUtf8());
        qputenv("PATH", QByteArray(PROCFORGE_SCRIPTS_DIR) + ':' + qgetenv("PATH"));
        QCoreApplication::setApplicationVersion(QStringLiteral("0.0.1"));
        QCOMPARE(UpdateDialog::selfExecutable(), app);
        QVERIFY(!UpdateDialog::updaterPath().isEmpty());

        auto *dlg = new UpdateDialog(QStringLiteral("v9.9.9"), QStringLiteral("http://x"), QStringLiteral("**notas**"));
        dlg->show();
        auto *btn = dlg->findChild<QPushButton*>(QStringLiteral("updateNow"));
        QVERIFY(btn && btn->isEnabled());
        btn->click();
        QTRY_VERIFY_WITH_TIMEOUT(btn->isEnabled(), 30000);
        QCOMPARE(btn->text(), QStringLiteral("Reiniciar agora"));
        QProcess run;
        run.start(app, {});
        QVERIFY(run.waitForFinished(3000));
        QCOMPARE(QString::fromUtf8(run.readAllStandardOutput()).trimmed(), QStringLiteral("procforge 9.9.9"));
        delete dlg;
        srv.kill();
        srv.waitForFinished(2000);
        qunsetenv("APPIMAGE");
        qunsetenv("PROCFORGE_RELEASES_API");
        qunsetenv("PROCFORGE_GITLAB_API");
        QCoreApplication::setApplicationVersion(QStringLiteral(PROCFORGE_VERSION));
    }

    void mainWindow()
    {
        FakeApp app({QStringLiteral("tree"), QStringLiteral("2"), QStringLiteral("1")});
        QVERIFY(app.ready());

        MainWindow w;
        w.show();
        auto *tree = w.findChild<QTreeView*>();
        QVERIFY(tree);
        QTRY_VERIFY_WITH_TIMEOUT(tree->model() && tree->model()->rowCount() > 0, 5000);

        // O fakeapp aparece no modelo, com os 2 filhos debaixo dele.
        QVERIFY(QMetaObject::invokeMethod(&w, "goToProcess", Q_ARG(int, app.pid())));
        QTRY_VERIFY_WITH_TIMEOUT(tree->currentIndex().isValid(), 4000);
        QCOMPARE(tree->currentIndex().data(ProcessModel::PidRole).toInt(), app.pid());
        QTRY_COMPARE_WITH_TIMEOUT(tree->model()->rowCount(tree->currentIndex().siblingAtColumn(0)), 2, 4000);

        // Busca: user:/pid: e texto.
        auto *search = w.findChild<QLineEdit*>(QStringLiteral("processSearch"));
        QVERIFY(search);
        search->setText(QStringLiteral("pid:%1").arg(app.pid()));
        QTest::qWait(400);
        int visible = 0;
        std::function<void(const QModelIndex &)> count = [&](const QModelIndex &p) {
            for (int r = 0; r < tree->model()->rowCount(p); ++r) {
                const QModelIndex i = tree->model()->index(r, 0, p);
                if (i.data(ProcessModel::PidRole).toInt() == app.pid()) ++visible;
                count(i);
            }
        };
        count({});
        QCOMPARE(visible, 1);
        search->clear();

        // Passa por todas as abas (Serviços e Rede rodam systemctl/ss assíncronos).
        auto *tabs = w.findChild<QTabWidget*>();
        QVERIFY(tabs);
        for (int i = 0; i < tabs->count(); ++i) {
            tabs->setCurrentIndex(i);
            QTest::qWait(1200);
        }
        tabs->setCurrentIndex(0);

        // Janela de informação do sistema (gráficos amostrando por 2,5 s).
        SysInfoWindow::showWindow(&w);
        QTest::qWait(2500);
        for (QWidget *tl : QApplication::topLevelWidgets())
            if (qobject_cast<SysInfoWindow*>(tl)) tl->close();
    }

    void scannerWindow()
    {
        FakeApp app({QStringLiteral("value"), QStringLiteral("918273")});
        QVERIFY(app.ready());
        ScannerWindow sw(app.pid(), app.starttime(), QStringLiteral("fakeapp"));
        sw.show();
        auto *value = sw.findChild<QLineEdit*>(QStringLiteral("scanValue"));
        auto *first = sw.findChild<QPushButton*>(QStringLiteral("scanFirst"));
        auto *refine = sw.findChild<QPushButton*>(QStringLiteral("scanRefine"));
        auto *count = sw.findChild<QLabel*>(QStringLiteral("scanCount"));
        auto *results = sw.findChild<QTableWidget*>(QStringLiteral("scanResults"));
        QVERIFY(value && first && refine && count && results);

        value->setText(QStringLiteral("918273"));
        first->click();
        QVERIFY(!first->isEnabled());   // busca em thread: controles travados
        QTRY_VERIFY_WITH_TIMEOUT(first->isEnabled(), 15000);
        QVERIFY(results->rowCount() >= 1);

        ::kill(app.pid(), SIGUSR1);
        QTest::qWait(300);
        value->setText(QStringLiteral("918274"));
        refine->click();
        QTRY_VERIFY_WITH_TIMEOUT(refine->isEnabled(), 15000);
        const quint64 addr = app.value(QStringLiteral("addr")).toULongLong(nullptr, 16);
        bool found = false;
        for (int r = 0; r < results->rowCount(); ++r)
            if (results->item(r, 0)->data(Qt::UserRole).toULongLong() == addr) found = true;
        QVERIFY2(found, "o scanner da janela não achou o endereço do fakeapp");
    }

    void propertiesExit()
    {
        auto *app = new FakeApp({QStringLiteral("threads"), QStringLiteral("2")});
        QVERIFY(app->ready());
        const int pid = app->pid();
        auto *props = new ProcessProperties(pid, app->starttime(), QStringLiteral("fakeapp"));
        props->show();
        QTest::qWait(1200);
        QVERIFY(!props->windowTitle().contains(QStringLiteral("encerrado")));
        delete app;   // mata o fakeapp
        QTRY_VERIFY_WITH_TIMEOUT(props->windowTitle().contains(QStringLiteral("encerrado")), 4000);
        props->close();
    }

    void findHandles()
    {
        const QString lock = QDir::tempPath() + QStringLiteral("/procforge-gui-%1.lock").arg(::getpid());
        FakeApp app({QStringLiteral("lock"), lock});
        QVERIFY(app.ready());
        FindHandlesDialog dlg;
        dlg.show();
        dlg.setTerm(QStringLiteral("procforge-gui-"));
        auto *tbl = dlg.findChild<QTableWidget*>(QStringLiteral("handlesTable"));
        QVERIFY(tbl);
        QTRY_VERIFY_WITH_TIMEOUT(tbl->rowCount() >= 1, 15000);
        bool mine = false;
        for (int r = 0; r < tbl->rowCount(); ++r)
            if (tbl->item(r, 0)->data(Qt::UserRole).toInt() == app.pid()
                && tbl->item(r, 3)->text() == lock)
                mine = true;
        QVERIFY(mine);
        QFile::remove(lock);
    }
};

QTEST_MAIN(TestGui)
#include "test_gui.moc"
