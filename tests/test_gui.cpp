// Teste de fumaça da GUI (QT_QPA_PLATFORM=offscreen): sobe a janela principal,
// passa por todas as abas e janelas e confere que funcionam com apps falsos.
#include "FakeApp.h"

#include "MainWindow.h"
#include "model/ProcessModel.h"
#include "panels/FindHandlesDialog.h"
#include "panels/ProcessProperties.h"
#include "panels/ScannerWindow.h"
#include "panels/SysInfoWindow.h"

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
