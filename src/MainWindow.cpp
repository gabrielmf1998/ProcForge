#include "MainWindow.h"
#include "model/ProcessModel.h"
#include "model/ProcessFilterProxy.h"
#include "model/ProcessCollector.h"
#include "actions/ProcessActions.h"
#include "inject/LibraryInjector.h"
#include "core/ThreadController.h"
#include "core/KWinControl.h"
#include "core/ThemeManager.h"
#include "core/Procfs.h"
#include "panels/DetailPanel.h"
#include "panels/ScannerWindow.h"
#include "panels/SystemTabs.h"
#include "panels/ProcessProperties.h"
#include "HelperClient.h"

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <KLocalizedString>
#include <KMessageBox>
#include <KGuiItem>
#include <KStandardGuiItem>
#include <KFormat>

#include <csignal>
#include <QApplication>
#include <QIcon>
#include <QKeySequence>
#include <QMap>
#include <QProcess>
#include <QSettings>
#include <QSizePolicy>
#include <QSysInfo>
#include <QTabWidget>
#include <QClipboard>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QTreeView>
#include <QVBoxLayout>
#include <QActionGroup>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    const QString host = QSysInfo::machineHostName();
    const QString user = QString::fromLocal8Bit(qgetenv("USER"));
    setWindowTitle(QStringLiteral("ProcForge  [%1@%2]").arg(user, host));
    setWindowIcon(QIcon(QStringLiteral(":/procforge.svg")));
    resize(1180, 780);

    // ---- modelo + proxy + árvore ----
    m_model = new ProcessModel(this);
    m_proxy = new ProcessFilterProxy(this);
    m_proxy->setSourceModel(m_model);

    m_tree = new QTreeView(this);
    m_tree->setModel(m_proxy);
    m_tree->setSortingEnabled(true);
    m_tree->sortByColumn(ProcessModel::ColCpu, Qt::DescendingOrder);
    m_tree->setUniformRowHeights(true);
    m_tree->setAllColumnsShowFocus(true);
    m_tree->setExpandsOnDoubleClick(false);  // duplo-clique abre Propriedades (estilo PH)
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setRootIsDecorated(true);
    m_tree->header()->setStretchLastSection(true);
    m_tree->setColumnWidth(ProcessModel::ColName, 320);
    m_tree->setColumnWidth(ProcessModel::ColPid, 58);
    m_tree->setColumnWidth(ProcessModel::ColCpu, 54);
    m_tree->setColumnWidth(ProcessModel::ColIo, 92);
    m_tree->setColumnWidth(ProcessModel::ColMem, 92);
    m_tree->setColumnWidth(ProcessModel::ColUser, 120);
    connect(m_tree, &QWidget::customContextMenuRequested,
            this, &MainWindow::showContextMenu);
    connect(m_tree->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &MainWindow::onCurrentChanged);
    connect(m_tree, &QAbstractItemView::doubleClicked, this,
            [this](const QModelIndex &) { propertiesCurrent(); });

    // ---- painel de detalhes sob a árvore (aba Processos) ----
    m_detail = new DetailPanel(this);
    auto *splitter = new QSplitter(Qt::Vertical, this);
    splitter->addWidget(m_tree);
    splitter->addWidget(m_detail);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);

    // ---- abas: Processos | Serviços | Rede | Disco ----
    m_tabs = new QTabWidget(this);
    m_tabs->addTab(splitter, i18n("Processos"));
    auto *services = new ServicesPanel(this);
    auto *network = new NetworkPanel(this);
    m_tabs->addTab(services, i18n("Serviços"));
    m_tabs->addTab(network, i18n("Rede"));
    m_tabs->addTab(new DiskPanel(this), i18n("Disco"));
    connect(services, &ServicesPanel::goToProcess, this, &MainWindow::goToProcess);
    connect(network, &NetworkPanel::goToProcess, this, &MainWindow::goToProcess);
    m_tabs->setCurrentIndex(QSettings().value(QStringLiteral("lastTab"), 0).toInt());
    connect(m_tabs, &QTabWidget::currentChanged, this,
            [](int i) { QSettings().setValue(QStringLiteral("lastTab"), i); });
    setCentralWidget(m_tabs);

    buildMenusAndToolbar();

    // ---- barra de status estilo Process Hacker ----
    m_cpuLabel   = new QLabel(this);
    m_memLabel   = new QLabel(this);
    m_countLabel = new QLabel(this);
    statusBar()->addWidget(m_cpuLabel);
    statusBar()->addWidget(new QLabel(QStringLiteral("   "), this));
    statusBar()->addWidget(m_memLabel);
    statusBar()->addWidget(new QLabel(QStringLiteral("   "), this));
    statusBar()->addWidget(m_countLabel);

    // ---- coletor em thread de fundo ----
    m_collectorThread = new QThread(this);
    m_collector = new ProcessCollector;          // sem parent: será movido de thread
    m_collector->moveToThread(m_collectorThread);
    connect(m_collectorThread, &QThread::finished,
            m_collector, &QObject::deleteLater);
    connect(m_collector, &ProcessCollector::snapshotReady,
            m_model, &ProcessModel::applySnapshot);
    connect(m_collector, &ProcessCollector::snapshotReady,
            this, &MainWindow::onSnapshot);
    m_collectorThread->start();
    const int intervalMs = QSettings().value(QStringLiteral("intervalMs"), 1000).toInt();
    QMetaObject::invokeMethod(m_collector, "start", Qt::QueuedConnection, Q_ARG(int, intervalMs));

    // ---- eventos de processo em push (cn_proc): atualização quase instantânea ----
    m_eventDebounce = new QTimer(this);
    m_eventDebounce->setSingleShot(true);
    m_eventDebounce->setInterval(60);
    connect(m_eventDebounce, &QTimer::timeout, this, [this]() {
        QMetaObject::invokeMethod(m_collector, "scanOnce", Qt::QueuedConnection);
    });
    if (QSettings().value(QStringLiteral("realtimeEvents"), true).toBool() && helper::available())
        m_subscribed = helper::subscribeProcEvents(this, SLOT(onProcEvent(uint,uint,uint)));
}

// Ícone de tema com fallback, para a toolbar/menus.
static QIcon ic(const char *name) { return QIcon::fromTheme(QString::fromLatin1(name)); }

void MainWindow::buildMenusAndToolbar()
{
    // ===================== Menu bar =====================
    // -- Hacker --
    auto *mHacker = menuBar()->addMenu(i18n("&Hacker"));
    connect(mHacker->addAction(ic("system-run"), i18n("Executar...")), &QAction::triggered,
            this, &MainWindow::runCommand);
    mHacker->addSeparator();
    connect(mHacker->addAction(ic("edit-find"), i18n("Localizar handles ou DLLs...")),
            &QAction::triggered, this, &MainWindow::findHandlesOrDlls);
    connect(mHacker->addAction(ic("utilities-system-monitor"), i18n("Informação do sistema...")),
            &QAction::triggered, this, &MainWindow::systemInformation);
    mHacker->addSeparator();
    connect(mHacker->addAction(ic("configure"), i18n("Opções...")), &QAction::triggered,
            this, &MainWindow::openOptions);
    mHacker->addSeparator();
    auto *actQuit = mHacker->addAction(ic("application-exit"), i18n("Sair"));
    actQuit->setShortcut(QKeySequence::Quit);
    connect(actQuit, &QAction::triggered, this, &QWidget::close);

    // -- Ver (View) --
    auto *mView = menuBar()->addMenu(i18n("&Ver"));
    auto *actRefresh = mView->addAction(ic("view-refresh"), i18n("Atualizar"));
    actRefresh->setShortcut(QKeySequence::Refresh);
    connect(actRefresh, &QAction::triggered, this, [this]() {
        QMetaObject::invokeMethod(m_collector, "scanOnce", Qt::QueuedConnection);
    });
    auto *intervalMenu = mView->addMenu(i18n("Intervalo de atualização"));
    auto *intervalGroup = new QActionGroup(this);
    struct { QString label; int ms; } opts[] = {
        {i18n("0,5 s"), 500}, {i18n("1 s"), 1000}, {i18n("2 s"), 2000}, {i18n("5 s"), 5000},
    };
    for (auto &o : opts) {
        auto *a = intervalMenu->addAction(o.label);
        a->setCheckable(true);
        a->setActionGroup(intervalGroup);
        if (o.ms == 1000) a->setChecked(true);
        const int ms = o.ms;
        connect(a, &QAction::triggered, this, [this, ms]() { setInterval(ms); });
    }
    mView->addSeparator();
    connect(mView->addAction(i18n("Expandir tudo")), &QAction::triggered,
            this, [this]() { m_tree->expandAll(); });
    connect(mView->addAction(i18n("Recolher tudo")), &QAction::triggered,
            this, [this]() { m_tree->collapseAll(); });

    // -- Tema (aplica ao vivo) --
    mView->addSeparator();
    auto *themeMenu = mView->addMenu(i18n("Tema"));
    auto *themeGroup = new QActionGroup(this);
    QSettings settings;
    const int curTheme = settings.value(QStringLiteral("theme"), int(theme::System)).toInt();
    struct { QString label; theme::Theme t; } themes[] = {
        {i18n("Sistema (Breeze)"), theme::System},
        {i18n("Claro"), theme::Light},
        {i18n("Escuro"), theme::Dark},
        {i18n("Clássico (Process Hacker)"), theme::Classic},
    };
    for (auto &th : themes) {
        auto *a = themeMenu->addAction(th.label);
        a->setCheckable(true);
        a->setActionGroup(themeGroup);
        if (int(th.t) == curTheme) a->setChecked(true);
        const theme::Theme t = th.t;
        connect(a, &QAction::triggered, this, [this, t]() {
            theme::apply(t);
            QSettings().setValue(QStringLiteral("theme"), int(t));
            m_tree->viewport()->update();
        });
    }

    // -- Idioma / Language (reinicia p/ aplicar) --
    auto *langMenu = mView->addMenu(i18n("Idioma / Language"));
    auto *langGroup = new QActionGroup(this);
    const QString curLang = settings.value(QStringLiteral("language"), QStringLiteral("system")).toString();
    struct { QString label; QString code; } langs[] = {
        {i18n("Automático (sistema)"), QStringLiteral("system")},
        {QStringLiteral("Português (Brasil)"), QStringLiteral("pt_BR")},
        {QStringLiteral("English (US)"), QStringLiteral("en")},
    };
    for (auto &lg : langs) {
        auto *a = langMenu->addAction(lg.label);
        a->setCheckable(true);
        a->setActionGroup(langGroup);
        if (lg.code == curLang) a->setChecked(true);
        const QString code = lg.code;
        connect(a, &QAction::triggered, this, [this, code]() {
            QSettings().setValue(QStringLiteral("language"), code);
            const auto btn = KMessageBox::questionTwoActions(
                this, i18n("O idioma muda ao reiniciar o ProcForge. Reiniciar agora?"),
                i18n("Idioma"), KGuiItem(i18n("Reiniciar")), KStandardGuiItem::cancel());
            if (btn == KMessageBox::PrimaryAction) {
                QProcess::startDetached(QApplication::applicationFilePath(), {});
                QApplication::quit();
            }
        });
    }

    // -- Ferramentas (Tools) -- agem sobre o processo selecionado
    auto *mTools = menuBar()->addMenu(i18n("&Ferramentas"));
    connect(mTools->addAction(i18n("Scanner de memória...")), &QAction::triggered, this, [this]() {
        ProcInfo pi; if (!currentInfo(pi)) return;
        auto *w = new ScannerWindow(pi.pid, pi.starttime, pi.name);
        w->setAttribute(Qt::WA_DeleteOnClose); w->show();
    });
    connect(mTools->addAction(i18n("Injetar biblioteca (.so)...")), &QAction::triggered,
            this, [this]() { injectCurrent(); });
    connect(mTools->addAction(i18n("Rastrear com eBPF...")), &QAction::triggered,
            this, [this]() { ebpfCurrent(); });
    connect(mTools->addAction(i18n("Namespaces / containers...")), &QAction::triggered,
            this, [this]() { namespacesCurrent(); });
    mTools->addSeparator();
    connect(mTools->addAction(i18n("Limitar recursos (cgroup)...")), &QAction::triggered,
            this, [this]() { cgroupLimitCurrent(); });
    connect(mTools->addAction(i18n("Escalonamento avançado...")), &QAction::triggered,
            this, [this]() { schedCurrent(); });

    // -- Usuários (Users) -- preenchido ao abrir
    m_usersMenu = menuBar()->addMenu(i18n("&Usuários"));
    connect(m_usersMenu, &QMenu::aboutToShow, this, &MainWindow::populateUsersMenu);

    // -- Ajuda (Help) --
    auto *mHelp = menuBar()->addMenu(i18n("Aj&uda"));
    connect(mHelp->addAction(ic("help-about"), i18n("Sobre o ProcForge")), &QAction::triggered,
            this, &MainWindow::aboutDialog);

    // ===================== Toolbar =====================
    auto *tb = addToolBar(i18n("Principal"));
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    connect(tb->addAction(ic("view-refresh"), i18n("Atualizar")), &QAction::triggered, this, [this]() {
        QMetaObject::invokeMethod(m_collector, "scanOnce", Qt::QueuedConnection);
    });
    connect(tb->addAction(ic("configure"), i18n("Opções")), &QAction::triggered,
            this, &MainWindow::openOptions);
    connect(tb->addAction(ic("edit-find"), i18n("Localizar handles ou DLLs")), &QAction::triggered,
            this, &MainWindow::findHandlesOrDlls);
    connect(tb->addAction(ic("utilities-system-monitor"), i18n("Informação do sistema")),
            &QAction::triggered, this, &MainWindow::systemInformation);
    tb->addSeparator();
    auto *actTerm = tb->addAction(ic("process-stop"), i18n("Terminar"));
    actTerm->setToolTip(i18n("Terminar o processo selecionado (SIGTERM)"));
    connect(actTerm, &QAction::triggered, this, [this]() { signalCurrent(SIGTERM); });

    auto *spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    tb->addWidget(spacer);
    m_search = new QLineEdit(tb);
    m_search->setPlaceholderText(i18n("Buscar processos (Ctrl+K)"));
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(300);
    m_search->addAction(ic("edit-find"), QLineEdit::LeadingPosition);
    connect(m_search, &QLineEdit::textChanged, m_proxy, &ProcessFilterProxy::setFilterText);
    tb->addWidget(m_search);
    auto *sc = new QAction(this);
    sc->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    connect(sc, &QAction::triggered, this, [this]() { m_search->setFocus(); m_search->selectAll(); });
    addAction(sc);
}

MainWindow::~MainWindow()
{
    if (m_subscribed)
        helper::unsubscribeProcEvents();
    if (m_collectorThread) {
        m_collectorThread->quit();
        m_collectorThread->wait();
    }
}

void MainWindow::onProcEvent(uint what, uint pid, uint ppid)
{
    Q_UNUSED(what); Q_UNUSED(pid); Q_UNUSED(ppid);
    // Coalesce rajadas de fork/exec/exit num único rescan rápido (~instantâneo).
    m_eventDebounce->start();
}

void MainWindow::goToProcess(int pid)
{
    m_tabs->setCurrentIndex(0);
    const QModelIndex src = m_model->indexForPid(pid);
    if (!src.isValid()) {
        statusBar()->showMessage(i18n("Processo %1 não está na lista.", pid), 3000);
        return;
    }
    const QModelIndex idx = m_proxy->mapFromSource(src);
    if (!idx.isValid()) return;
    m_tree->setCurrentIndex(idx);
    m_tree->scrollTo(idx, QAbstractItemView::PositionAtCenter);
    m_tree->setFocus();
}

void MainWindow::onSnapshot(const QList<ProcInfo> &procs)
{
    double cpu = 0.0;
    m_userCounts.clear();
    for (const ProcInfo &p : procs) {
        cpu += p.cpuPercent;
        m_userCounts[p.user]++;
    }
    cpu = qBound(0.0, cpu, 100.0);
    m_cpuLabel->setText(i18n("CPU: %1%", QString::number(cpu, 'f', 2)));

    quint64 total = 0, avail = 0;
    QFile mi(QStringLiteral("/proc/meminfo"));
    if (mi.open(QIODevice::ReadOnly)) {
        const auto lines = mi.readAll().split('\n');
        for (const QByteArray &line : lines) {
            if (line.startsWith("MemTotal:"))     total = line.split(':').at(1).trimmed().split(' ').first().toULongLong() * 1024;
            else if (line.startsWith("MemAvailable:")) avail = line.split(':').at(1).trimmed().split(' ').first().toULongLong() * 1024;
        }
    }
    if (total > 0) {
        static const KFormat fmt;
        const quint64 used = (total > avail) ? total - avail : 0;
        m_memLabel->setText(i18n("Memória física: %1 (%2%)",
                                 fmt.formatByteSize(used, 2),
                                 QString::number(100.0 * used / total, 'f', 2)));
    }
    m_countLabel->setText(i18n("Processos: %1", procs.size()));

    if (!m_didInitialExpand && !procs.isEmpty()) {
        m_tree->expandAll();
        m_didInitialExpand = true;
    }
}

void MainWindow::populateUsersMenu()
{
    m_usersMenu->clear();
    if (m_userCounts.isEmpty()) {
        m_usersMenu->addAction(i18n("(sem dados)"))->setEnabled(false);
        return;
    }
    for (auto it = m_userCounts.constBegin(); it != m_userCounts.constEnd(); ++it) {
        auto *a = m_usersMenu->addAction(i18n("%1  (%2 processos)", it.key(), it.value()));
        const QString u = it.key();
        connect(a, &QAction::triggered, this, [this, u]() {
            m_search->setText(u);   // filtra pela busca
        });
    }
}

void MainWindow::runCommand()
{
    bool ok = false;
    const QString cmd = QInputDialog::getText(this, i18n("Executar"),
                                              i18n("Programa e argumentos:"),
                                              QLineEdit::Normal, QString(), &ok);
    if (!ok || cmd.trimmed().isEmpty())
        return;
    const QStringList parts = QProcess::splitCommand(cmd);
    if (parts.isEmpty())
        return;
    if (!QProcess::startDetached(parts.first(), parts.mid(1)))
        KMessageBox::error(this, i18n("Não consegui iniciar: %1", cmd));
}

void MainWindow::findHandlesOrDlls()
{
    bool ok = false;
    const QString term = QInputDialog::getText(this, i18n("Localizar handles ou DLLs"),
        i18n("Buscar em fds e bibliotecas mapeadas (nome de arquivo, socket:[inode], .so):"),
        QLineEdit::Normal, QString(), &ok);
    if (!ok || term.isEmpty())
        return;

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Resultados para \"%1\"", term));
    dlg->resize(760, 520);
    auto *v = new QVBoxLayout(dlg);
    auto *tbl = new QTableWidget(0, 4, dlg);
    tbl->setHorizontalHeaderLabels({i18n("PID"), i18n("Processo"), i18n("Tipo"), i18n("Alvo")});
    tbl->verticalHeader()->setVisible(false);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->horizontalHeader()->setStretchLastSection(true);
    v->addWidget(tbl);

    int rows = 0;
    for (int pid : procfs::listPids()) {
        if (rows > 4000) break;
        QString comm;
        { QFile f(QStringLiteral("/proc/%1/comm").arg(pid));
          if (f.open(QIODevice::ReadOnly)) comm = QString::fromUtf8(f.readAll()).trimmed(); }
        for (const procfs::FdEntry &e : procfs::listFds(pid)) {
            if (e.target.contains(term, Qt::CaseInsensitive)) {
                tbl->insertRow(rows);
                tbl->setItem(rows, 0, new QTableWidgetItem(QString::number(pid)));
                tbl->setItem(rows, 1, new QTableWidgetItem(comm));
                tbl->setItem(rows, 2, new QTableWidgetItem(i18n("fd %1 (%2)", e.fd, e.type)));
                tbl->setItem(rows, 3, new QTableWidgetItem(e.target));
                ++rows;
            }
        }
        for (const procfs::MapEntry &m : procfs::listMaps(pid)) {
            if (!m.path.isEmpty() && m.path.contains(term, Qt::CaseInsensitive)) {
                tbl->insertRow(rows);
                tbl->setItem(rows, 0, new QTableWidgetItem(QString::number(pid)));
                tbl->setItem(rows, 1, new QTableWidgetItem(comm));
                tbl->setItem(rows, 2, new QTableWidgetItem(i18n("mapa")));
                tbl->setItem(rows, 3, new QTableWidgetItem(m.path));
                ++rows;
            }
        }
    }
    tbl->resizeColumnsToContents();
    dlg->setWindowTitle(i18n("%1 resultado(s) para \"%2\"", rows, term));
    dlg->show();
}

void MainWindow::systemInformation()
{
    static const KFormat fmt;
    quint64 total = 0, avail = 0;
    QFile mi(QStringLiteral("/proc/meminfo"));
    if (mi.open(QIODevice::ReadOnly))
        for (const QByteArray &line : mi.readAll().split('\n')) {
            if (line.startsWith("MemTotal:"))      total = line.split(':').at(1).trimmed().split(' ').first().toULongLong() * 1024;
            else if (line.startsWith("MemAvailable:")) avail = line.split(':').at(1).trimmed().split(' ').first().toULongLong() * 1024;
        }
    QString loadavg, uptime, model;
    { QFile f(QStringLiteral("/proc/loadavg")); if (f.open(QIODevice::ReadOnly)) loadavg = QString::fromUtf8(f.readAll()).section(' ', 0, 2); }
    { QFile f(QStringLiteral("/proc/uptime")); if (f.open(QIODevice::ReadOnly)) {
        const double up = QString::fromUtf8(f.readAll()).section(' ', 0, 0).toDouble();
        uptime = fmt.formatDuration(qint64(up) * 1000); } }
    { QFile f(QStringLiteral("/proc/cpuinfo")); if (f.open(QIODevice::ReadOnly))
        for (const QByteArray &line : f.readAll().split('\n'))
            if (line.startsWith("model name")) { model = QString::fromUtf8(line).section(':', 1).trimmed(); break; } }

    const QString text = i18n(
        "Kernel: %1\nDistribuição: %2\nCPU: %3 (%4 núcleos)\nMemória: %5 usada de %6\n"
        "Carga (1/5/15 min): %7\nUptime: %8",
        QSysInfo::kernelVersion(), QSysInfo::prettyProductName(),
        model.isEmpty() ? QSysInfo::currentCpuArchitecture() : model,
        int(actions::onlineCpus().size()),
        fmt.formatByteSize(total > avail ? total - avail : 0, 2), fmt.formatByteSize(total, 2),
        loadavg, uptime);
    KMessageBox::information(this, text, i18n("Informação do sistema"));
}

void MainWindow::aboutDialog()
{
    KMessageBox::information(this,
        i18n("ProcForge — clone do Process Hacker para Linux.\n\n"
             "Manipulação ativa do sistema sobre primitivas nativas: procfs, pidfd, "
             "ptrace, process_vm_readv/writev, cgroups v2, netlink, eBPF, KWin, "
             "com helper D-Bus privilegiado e polkit por ação.\n\nQt6 · KDE Frameworks 6 · Wayland."),
        i18n("Sobre o ProcForge"));
}

void MainWindow::openOptions()
{
    QSettings s;
    QDialog dlg(this);
    dlg.setWindowTitle(i18n("Opções"));
    auto *form = new QFormLayout(&dlg);

    auto *interval = new QComboBox(&dlg);
    struct { QString l; int ms; } iv[] = {
        {i18n("0,5 s"), 500}, {i18n("1 s"), 1000}, {i18n("2 s"), 2000}, {i18n("5 s"), 5000}};
    for (auto &o : iv) interval->addItem(o.l, o.ms);
    if (int i = interval->findData(s.value(QStringLiteral("intervalMs"), 1000).toInt()); i >= 0)
        interval->setCurrentIndex(i);
    form->addRow(i18n("Intervalo de atualização:"), interval);

    auto *rt = new QCheckBox(i18n("Atualização em tempo real (eventos cn_proc via helper)"), &dlg);
    rt->setChecked(s.value(QStringLiteral("realtimeEvents"), true).toBool());
    form->addRow(QString(), rt);

    auto *themeCb = new QComboBox(&dlg);
    themeCb->addItem(i18n("Sistema (Breeze)"), int(theme::System));
    themeCb->addItem(i18n("Claro"), int(theme::Light));
    themeCb->addItem(i18n("Escuro"), int(theme::Dark));
    themeCb->addItem(i18n("Clássico (Process Hacker)"), int(theme::Classic));
    if (int i = themeCb->findData(s.value(QStringLiteral("theme"), int(theme::System)).toInt()); i >= 0)
        themeCb->setCurrentIndex(i);
    form->addRow(i18n("Tema:"), themeCb);

    auto *langCb = new QComboBox(&dlg);
    langCb->addItem(i18n("Automático (sistema)"), QStringLiteral("system"));
    langCb->addItem(QStringLiteral("Português (Brasil)"), QStringLiteral("pt_BR"));
    langCb->addItem(QStringLiteral("English (US)"), QStringLiteral("en"));
    if (int i = langCb->findData(s.value(QStringLiteral("language"), QStringLiteral("system")).toString()); i >= 0)
        langCb->setCurrentIndex(i);
    form->addRow(i18n("Idioma / Language:"), langCb);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const int ms = interval->currentData().toInt();
    s.setValue(QStringLiteral("intervalMs"), ms);
    setInterval(ms);

    const bool rtOn = rt->isChecked();
    s.setValue(QStringLiteral("realtimeEvents"), rtOn);
    if (rtOn && !m_subscribed && helper::available())
        m_subscribed = helper::subscribeProcEvents(this, SLOT(onProcEvent(uint,uint,uint)));
    else if (!rtOn && m_subscribed) {
        helper::unsubscribeProcEvents();
        m_subscribed = false;
    }

    const int th = themeCb->currentData().toInt();
    s.setValue(QStringLiteral("theme"), th);
    theme::apply(theme::Theme(th));
    m_tree->viewport()->update();

    const QString newLang = langCb->currentData().toString();
    if (newLang != s.value(QStringLiteral("language"), QStringLiteral("system")).toString()) {
        s.setValue(QStringLiteral("language"), newLang);
        const auto btn = KMessageBox::questionTwoActions(
            this, i18n("O idioma muda ao reiniciar o ProcForge. Reiniciar agora?"),
            i18n("Idioma"), KGuiItem(i18n("Reiniciar")), KStandardGuiItem::cancel());
        if (btn == KMessageBox::PrimaryAction) {
            QProcess::startDetached(QApplication::applicationFilePath(), {});
            QApplication::quit();
        }
    }
}

void MainWindow::onCurrentChanged(const QModelIndex &current, const QModelIndex &)
{
    if (!current.isValid()) {
        m_detail->setProcess(0, 0, {});
        return;
    }
    const QModelIndex src = m_proxy->mapToSource(current);
    const ProcInfo pi = m_model->infoForIndex(src);
    m_detail->setProcess(pi.pid, pi.starttime, pi.name);
}

void MainWindow::setInterval(int ms)
{
    QMetaObject::invokeMethod(m_collector, "setInterval", Qt::QueuedConnection, Q_ARG(int, ms));
}

bool MainWindow::currentInfo(ProcInfo &out) const
{
    const QModelIndex cur = m_tree->currentIndex();
    if (!cur.isValid())
        return false;
    out = m_model->infoForIndex(m_proxy->mapToSource(cur));
    return out.pid > 0;
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    QMenu menu(this);
    menu.addSection(i18n("%1 (PID %2)", pi.name, pi.pid));

    connect(menu.addAction(i18n("Propriedades...")), &QAction::triggered,
            this, [this]() { propertiesCurrent(); });
    menu.addSeparator();

    auto *sigMenu = menu.addMenu(i18n("Enviar sinal"));
    struct { QString label; int sig; } sigs[] = {
        {QStringLiteral("SIGTERM (15)"), SIGTERM},
        {QStringLiteral("SIGKILL (9)"),  SIGKILL},
        {QStringLiteral("SIGINT (2)"),   SIGINT},
        {QStringLiteral("SIGHUP (1)"),   SIGHUP},
        {QStringLiteral("SIGSTOP (19)"), SIGSTOP},
        {QStringLiteral("SIGCONT (18)"), SIGCONT},
        {QStringLiteral("SIGUSR1 (10)"), SIGUSR1},
        {QStringLiteral("SIGUSR2 (12)"), SIGUSR2},
    };
    for (auto &s : sigs) {
        int sig = s.sig;
        connect(sigMenu->addAction(s.label), &QAction::triggered,
                this, [this, sig]() { signalCurrent(sig); });
    }

    connect(menu.addAction(i18n("Suspender (STOP)")), &QAction::triggered,
            this, [this]() { signalCurrent(SIGSTOP); });
    connect(menu.addAction(i18n("Retomar (CONT)")), &QAction::triggered,
            this, [this]() { signalCurrent(SIGCONT); });

    menu.addSeparator();
    connect(menu.addAction(i18n("Matar árvore inteira")), &QAction::triggered,
            this, [this]() { killTreeCurrent(); });

    menu.addSeparator();
    connect(menu.addAction(i18n("Prioridade (nice)...")), &QAction::triggered,
            this, [this]() { reniceCurrent(); });
    connect(menu.addAction(i18n("Afinidade de CPU...")), &QAction::triggered,
            this, [this]() { affinityCurrent(); });
    connect(menu.addAction(i18n("Escalonamento avançado...")), &QAction::triggered,
            this, [this]() { schedCurrent(); });
    connect(menu.addAction(i18n("Limites (prlimit)...")), &QAction::triggered,
            this, [this]() { prlimitCurrent(); });
    connect(menu.addAction(i18n("Threads (suspender/retomar)...")), &QAction::triggered,
            this, [this]() { threadsCurrent(); });

    auto *winMenu = menu.addMenu(i18n("Janela"));
    auto winAct = [this](kwin::WinAction a) {
        ProcInfo pi;
        if (!currentInfo(pi)) return;
        QString err;
        if (!kwin::runAction(pi.pid, a, &err))
            KMessageBox::error(this, err);
    };
    connect(winMenu->addAction(i18n("Trazer para frente")), &QAction::triggered,
            this, [winAct]() { winAct(kwin::WinAction::Activate); });
    connect(winMenu->addAction(i18n("Minimizar")), &QAction::triggered,
            this, [winAct]() { winAct(kwin::WinAction::Minimize); });
    connect(winMenu->addAction(i18n("Restaurar")), &QAction::triggered,
            this, [winAct]() { winAct(kwin::WinAction::Unminimize); });
    connect(winMenu->addAction(i18n("Fechar janela")), &QAction::triggered,
            this, [winAct]() { winAct(kwin::WinAction::Close); });

    connect(menu.addAction(i18n("Namespaces / containers...")), &QAction::triggered,
            this, [this]() { namespacesCurrent(); });
    connect(menu.addAction(i18n("Rastrear com eBPF...")), &QAction::triggered,
            this, [this]() { ebpfCurrent(); });

    menu.addSeparator();
    connect(menu.addAction(i18n("Scanner de memória...")), &QAction::triggered,
            this, [this]() {
                ProcInfo pi;
                if (!currentInfo(pi))
                    return;
                auto *w = new ScannerWindow(pi.pid, pi.starttime, pi.name);
                w->setAttribute(Qt::WA_DeleteOnClose);
                w->show();
            });

    menu.addSeparator();
    connect(menu.addAction(i18n("Injetar biblioteca (.so)...")), &QAction::triggered,
            this, [this]() { injectCurrent(); });
    connect(menu.addAction(i18n("Limitar recursos (cgroup)...")), &QAction::triggered,
            this, [this]() { cgroupLimitCurrent(); });
    connect(menu.addAction(i18n("Remover limites (cgroup)")), &QAction::triggered,
            this, [this]() { cgroupReleaseCurrent(); });

    menu.addSeparator();
    connect(menu.addAction(i18n("Copiar PID")), &QAction::triggered,
            this, [this]() { copyPidCurrent(); });

    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void MainWindow::signalCurrent(int sig)
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    auto rescan = [this]() { QMetaObject::invokeMethod(m_collector, "scanOnce", Qt::QueuedConnection); };
    actions::Result r = actions::sendSignal(pi.pid, pi.starttime, sig);
    if (r.ok) { rescan(); return; }
    if (!r.needsPrivilege()) { KMessageBox::error(this, r.error); rescan(); return; }
    // uid alheio -> helper + polkit, SEM travar a GUI (assíncrono).
    helper::sendSignalAsync(pi.pid, pi.starttime, sig, this, [this, rescan](actions::Result hr) {
        if (!hr.ok) KMessageBox::error(this, hr.error);
        rescan();
    });
}

void MainWindow::killTreeCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    const auto btn = KMessageBox::warningContinueCancel(
        this,
        i18n("Matar %1 (PID %2) e TODA a sua descendência com SIGKILL?",
             pi.name, pi.pid),
        i18n("Matar árvore"),
        KGuiItem(i18n("Matar árvore"), QStringLiteral("process-stop")),
        KStandardGuiItem::cancel());
    if (btn != KMessageBox::Continue)
        return;
    const actions::Result r = actions::killTree(pi.pid);
    if (!r.ok)
        KMessageBox::error(this, r.error);
    else if (!r.detail.isEmpty())
        statusBar()->showMessage(r.detail, 4000);
    QMetaObject::invokeMethod(m_collector, "scanOnce", Qt::QueuedConnection);
}

void MainWindow::reniceCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    bool okNice = false;
    int cur = actions::getNice(pi.pid, &okNice);
    bool ok = false;
    int nice = QInputDialog::getInt(
        this, i18n("Prioridade de %1 (PID %2)", pi.name, pi.pid),
        i18n("nice (-20 = mais prioritário, 19 = menos):"),
        okNice ? cur : 0, -20, 19, 1, &ok);
    if (!ok)
        return;
    actions::Result r = actions::renice(pi.pid, nice);
    if (r.ok) return;
    if (!r.needsPrivilege()) { KMessageBox::error(this, r.error); return; }
    helper::reniceAsync(pi.pid, pi.starttime, nice, this, [this](actions::Result hr) {
        if (!hr.ok) KMessageBox::error(this, hr.error);
    });
}

void MainWindow::affinityCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    const QList<int> all = actions::onlineCpus();
    const QList<int> set = actions::getAffinity(pi.pid);

    QDialog dlg(this);
    dlg.setWindowTitle(i18n("Afinidade de CPU — %1 (PID %2)", pi.name, pi.pid));
    auto *lay = new QVBoxLayout(&dlg);
    auto *box = new QGroupBox(i18n("CPUs permitidas"), &dlg);
    auto *grid = new QGridLayout(box);
    QList<QCheckBox*> checks;
    int cols = 4;
    for (int i = 0; i < all.size(); ++i) {
        auto *cb = new QCheckBox(i18n("CPU %1", all.at(i)), box);
        cb->setChecked(set.contains(all.at(i)));
        grid->addWidget(cb, i / cols, i % cols);
        checks.append(cb);
    }
    lay->addWidget(box);
    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted)
        return;
    QList<int> chosen;
    for (int i = 0; i < checks.size(); ++i)
        if (checks.at(i)->isChecked())
            chosen.append(all.at(i));
    actions::Result r = actions::setAffinity(pi.pid, chosen);
    if (r.ok) return;
    if (!r.needsPrivilege()) { KMessageBox::error(this, r.error); return; }
    helper::setAffinityAsync(pi.pid, pi.starttime, chosen, this, [this](actions::Result hr) {
        if (!hr.ok) KMessageBox::error(this, hr.error);
    });
}

void MainWindow::copyPidCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    QApplication::clipboard()->setText(QString::number(pi.pid));
}

void MainWindow::propertiesCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    (new ProcessProperties(pi.pid, pi.starttime, pi.name))->show();  // WA_DeleteOnClose
}

void MainWindow::injectCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    const QString so = QFileDialog::getOpenFileName(
        this, i18n("Escolher biblioteca para injetar"), QString(),
        i18n("Bibliotecas compartilhadas (*.so *.so.*)"));
    if (so.isEmpty())
        return;
    const auto btn = KMessageBox::warningContinueCancel(
        this,
        i18n("Injetar '%1' em %2 (PID %3) forçando dlopen via ptrace?\n\n"
             "Isto EXECUTA CÓDIGO dentro do processo alvo e pode travá-lo se a "
             "biblioteca for incompatível. Use só em processos que você controla.",
             so, pi.name, pi.pid),
        i18n("Injetar biblioteca"),
        KGuiItem(i18n("Injetar"), QStringLiteral("dialog-warning")),
        KStandardGuiItem::cancel());
    if (btn != KMessageBox::Continue)
        return;

    // Mesmo uid -> direto (ptrace_scope=0); uid alheio -> helper (polkit).
    if (static_cast<uint>(::getuid()) == pi.uid) {
        const inject::Result r = inject::injectLibrary(pi.pid, so);
        if (r.ok)
            KMessageBox::information(this, i18n("Biblioteca injetada (handle 0x%1).",
                                                QString::number(r.retval, 16)));
        else
            KMessageBox::error(this, r.error);
    } else {
        helper::injectLibraryAsync(pi.pid, pi.starttime, so, this, [this](actions::Result r) {
            if (r.ok) KMessageBox::information(this, i18n("Biblioteca injetada via helper."));
            else KMessageBox::error(this, r.error);
        });
    }
}

void MainWindow::cgroupLimitCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(i18n("Limitar recursos — %1 (PID %2)", pi.name, pi.pid));
    auto *form = new QFormLayout(&dlg);

    const int ncpu = int(actions::onlineCpus().size());
    auto *cpu = new QSpinBox(&dlg);
    cpu->setRange(0, ncpu * 100);
    cpu->setValue(50);
    cpu->setSuffix(i18n(" % de um núcleo (0 = sem limite)"));
    form->addRow(i18n("CPU:"), cpu);

    auto *mem = new QSpinBox(&dlg);
    mem->setRange(0, 1024 * 1024);
    mem->setValue(0);
    mem->setSuffix(i18n(" MB (0 = sem limite)"));
    form->addRow(i18n("Memória máx.:"), mem);

    auto *pids = new QSpinBox(&dlg);
    pids->setRange(0, 100000);
    pids->setValue(0);
    pids->setSpecialValueText(i18n("sem limite"));
    form->addRow(i18n("Máx. de PIDs:"), pids);

    auto *io = new QSpinBox(&dlg);
    io->setRange(0, 100000);
    io->setValue(0);
    io->setSuffix(i18n(" MB/s (0 = sem limite)"));
    form->addRow(i18n("Máx. I/O de disco:"), io);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const quint64 memBytes = quint64(mem->value()) * 1024ull * 1024ull;
    const quint64 ioBps    = quint64(io->value()) * 1000000ull;
    const QString name = pi.name;
    const int pid = pi.pid;
    helper::cgroupThrottleAsync(pi.pid, pi.starttime, cpu->value(), memBytes,
                                quint64(pids->value()), ioBps, this,
                                [this, name, pid](actions::Result r) {
        if (!r.ok) KMessageBox::error(this, r.error);
        else statusBar()->showMessage(i18n("Limites aplicados a %1 (PID %2) via cgroup.", name, pid), 4000);
    });
}

void MainWindow::cgroupReleaseCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    const QString name = pi.name;
    const int pid = pi.pid;
    helper::cgroupReleaseAsync(pi.pid, pi.starttime, this, [this, name, pid](actions::Result r) {
        if (!r.ok) KMessageBox::error(this, r.error);
        else statusBar()->showMessage(i18n("Limites removidos de %1 (PID %2).", name, pid), 4000);
    });
}

void MainWindow::schedCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(i18n("Escalonamento — %1 (PID %2)", pi.name, pi.pid));
    auto *form = new QFormLayout(&dlg);

    auto *pol = new QComboBox(&dlg);
    pol->addItem(i18n("Normal (OTHER)"), SCHED_OTHER);
    pol->addItem(i18n("Batch"),          SCHED_BATCH);
    pol->addItem(i18n("Ocioso (IDLE)"),  SCHED_IDLE);
    pol->addItem(i18n("Tempo real FIFO"), SCHED_FIFO);
    pol->addItem(i18n("Tempo real RR"),   SCHED_RR);
    int curRt = 0;
    const int curPol = actions::getScheduler(pi.pid, &curRt);
    if (int i = pol->findData(curPol); i >= 0) pol->setCurrentIndex(i);
    form->addRow(i18n("Política:"), pol);

    auto *rt = new QSpinBox(&dlg);
    rt->setRange(1, 99);
    rt->setValue(curRt > 0 ? curRt : 1);
    form->addRow(i18n("Prioridade RT (FIFO/RR):"), rt);

    auto *io = new QComboBox(&dlg);
    io->addItem(i18n("I/O: Nenhuma"),     0);
    io->addItem(i18n("I/O: Tempo real"),  1);
    io->addItem(i18n("I/O: Melhor esforço"), 2);
    io->addItem(i18n("I/O: Ocioso"),      3);
    int curIoClass = 0;
    actions::getIoPrio(pi.pid, &curIoClass);
    if (int i = io->findData(curIoClass); i >= 0) io->setCurrentIndex(i);
    form->addRow(i18n("Classe de I/O:"), io);

    auto *ioPrio = new QSpinBox(&dlg);
    ioPrio->setRange(0, 7);
    form->addRow(i18n("Prioridade de I/O (0-7):"), ioPrio);

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const int policy = pol->currentData().toInt();
    const int rtVal = rt->value();
    const int ioClass = io->currentData().toInt();
    const int ioVal = ioPrio->value();
    const int pid = pi.pid;
    const quint64 st = pi.starttime;
    const QString name = pi.name;

    auto applyIo = [this, pid, st, ioClass, ioVal, name]() {
        actions::Result ri = actions::setIoPrio(pid, ioClass, ioVal);
        if (ri.ok) { statusBar()->showMessage(i18n("Escalonamento aplicado a %1.", name), 3000); return; }
        if (!ri.needsPrivilege()) { KMessageBox::error(this, ri.error); return; }
        helper::setIoPrioAsync(pid, st, ioClass, ioVal, this, [this, name](actions::Result r) {
            if (!r.ok) KMessageBox::error(this, r.error);
            else statusBar()->showMessage(i18n("Escalonamento aplicado a %1.", name), 3000);
        });
    };

    actions::Result rs = actions::setScheduler(pid, policy, rtVal);
    if (rs.ok) { applyIo(); return; }
    if (!rs.needsPrivilege()) { KMessageBox::error(this, rs.error); return; }
    helper::setSchedulerAsync(pid, st, policy, rtVal, this, [this, applyIo](actions::Result r) {
        if (!r.ok) { KMessageBox::error(this, r.error); return; }
        applyIo();
    });
}

void MainWindow::prlimitCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    struct { const char *name; int res; } kinds[] = {
        {"NOFILE (arquivos abertos)", RLIMIT_NOFILE},
        {"AS (espaço de endereço)",   RLIMIT_AS},
        {"NPROC (processos)",         RLIMIT_NPROC},
        {"CPU (segundos)",            RLIMIT_CPU},
        {"FSIZE (tamanho de arquivo)",RLIMIT_FSIZE},
        {"STACK (pilha)",             RLIMIT_STACK},
        {"CORE (core dump)",          RLIMIT_CORE},
    };

    QDialog dlg(this);
    dlg.setWindowTitle(i18n("Limites (prlimit) — %1 (PID %2)", pi.name, pi.pid));
    auto *form = new QFormLayout(&dlg);
    auto *res = new QComboBox(&dlg);
    for (auto &k : kinds)
        res->addItem(QString::fromUtf8(k.name), k.res);
    form->addRow(i18n("Recurso:"), res);

    auto *soft = new QLineEdit(&dlg);
    auto *hard = new QLineEdit(&dlg);
    form->addRow(i18n("Soft ('ilimitado' ou número):"), soft);
    form->addRow(i18n("Hard ('ilimitado' ou número):"), hard);

    auto fill = [&]() {
        quint64 s = 0, h = 0;
        if (actions::getRlimit(pi.pid, res->currentData().toInt(), &s, &h)) {
            soft->setText(s == ~0ull ? QStringLiteral("ilimitado") : QString::number(s));
            hard->setText(h == ~0ull ? QStringLiteral("ilimitado") : QString::number(h));
        }
    };
    fill();
    connect(res, &QComboBox::currentIndexChanged, &dlg, [&]() { fill(); });

    auto *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(bb);
    if (dlg.exec() != QDialog::Accepted)
        return;

    auto parse = [](const QString &t) -> quint64 {
        const QString s = t.trimmed().toLower();
        if (s.isEmpty() || s == QLatin1String("ilimitado") || s == QLatin1String("unlimited")
            || s == QLatin1String("max") || s == QLatin1String("-1"))
            return ~0ull;
        return s.toULongLong();
    };
    const int resource = res->currentData().toInt();
    const quint64 softV = parse(soft->text());
    const quint64 hardV = parse(hard->text());
    const QString name = pi.name;
    actions::Result r = actions::setRlimit(pi.pid, resource, softV, hardV);
    if (r.ok) { statusBar()->showMessage(i18n("Limite aplicado a %1.", name), 3000); return; }
    if (!r.needsPrivilege()) { KMessageBox::error(this, r.error); return; }
    helper::setRlimitAsync(pi.pid, pi.starttime, resource, softV, hardV, this, [this, name](actions::Result hr) {
        if (!hr.ok) KMessageBox::error(this, hr.error);
        else statusBar()->showMessage(i18n("Limite aplicado a %1.", name), 3000);
    });
}

void MainWindow::threadsCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;
    if (pi.uid != static_cast<uint>(::getuid())) {
        KMessageBox::information(this,
            i18n("Suspender uma thread isolada é suportado apenas para processos do "
                 "seu usuário (o tracer ptrace vive dentro da GUI)."));
        return;
    }

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Threads — %1 (PID %2)", pi.name, pi.pid));
    dlg->resize(480, 420);
    auto *v = new QVBoxLayout(dlg);
    auto *table = new QTableWidget(0, 4, dlg);
    table->setHorizontalHeaderLabels({i18n("TID"), i18n("Nome"), i18n("Estado"), i18n("Suspensa")});
    table->verticalHeader()->setVisible(false);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->horizontalHeader()->setStretchLastSection(true);
    v->addWidget(table);
    auto *hb = new QHBoxLayout;
    auto *bSus = new QPushButton(i18n("Suspender thread"), dlg);
    auto *bRes = new QPushButton(i18n("Retomar thread"), dlg);
    hb->addWidget(bSus); hb->addWidget(bRes); hb->addStretch();
    v->addLayout(hb);

    const int pid = pi.pid;
    auto populate = [table, pid]() {
        int keep = table->currentRow() >= 0 && table->item(table->currentRow(), 0)
                       ? table->item(table->currentRow(), 0)->data(Qt::UserRole).toInt() : 0;
        const QStringList tids = QDir(QStringLiteral("/proc/%1/task").arg(pid))
                                     .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        table->setRowCount(tids.size());
        int r = 0, keepRow = -1;
        for (const QString &t : tids) {
            const int tid = t.toInt();
            QString comm;
            { QFile f(QStringLiteral("/proc/%1/task/%2/comm").arg(pid).arg(tid));
              if (f.open(QIODevice::ReadOnly)) comm = QString::fromUtf8(f.readAll()).trimmed(); }
            char st = '?';
            { QFile f(QStringLiteral("/proc/%1/task/%2/stat").arg(pid).arg(tid));
              if (f.open(QIODevice::ReadOnly)) { const QByteArray b = f.readAll();
                  const int rp = b.lastIndexOf(')'); if (rp > 0 && rp + 2 < b.size()) st = b[rp + 2]; } }
            const bool sus = ThreadController::instance().isSuspended(tid);
            auto *it0 = new QTableWidgetItem(QString::number(tid));
            it0->setData(Qt::UserRole, tid);
            table->setItem(r, 0, it0);
            table->setItem(r, 1, new QTableWidgetItem(comm));
            table->setItem(r, 2, new QTableWidgetItem(ProcInfo::stateText(st)));
            table->setItem(r, 3, new QTableWidgetItem(sus ? i18n("sim") : QString()));
            if (tid == keep) keepRow = r;
            ++r;
        }
        if (keepRow >= 0) table->selectRow(keepRow);
    };
    populate();

    auto selTid = [table]() -> int {
        const int row = table->currentRow();
        return (row >= 0 && table->item(row, 0)) ? table->item(row, 0)->data(Qt::UserRole).toInt() : 0;
    };
    connect(bSus, &QPushButton::clicked, dlg, [=]() {
        const int tid = selTid(); if (!tid) return;
        QString err;
        if (!ThreadController::instance().suspend(tid, &err)) KMessageBox::error(dlg, err);
        populate();
    });
    connect(bRes, &QPushButton::clicked, dlg, [=]() {
        const int tid = selTid(); if (!tid) return;
        QString err;
        if (!ThreadController::instance().resume(tid, &err)) KMessageBox::error(dlg, err);
        populate();
    });
    auto *timer = new QTimer(dlg);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, dlg, [=]() { populate(); });
    timer->start();

    dlg->show();
}

void MainWindow::namespacesCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    auto readns = [](int pid, const char *t) -> QString {
        char buf[256];
        const QString p = QStringLiteral("/proc/%1/ns/%2").arg(pid).arg(QLatin1String(t));
        const ssize_t n = ::readlink(p.toLocal8Bit().constData(), buf, sizeof buf - 1);
        return n > 0 ? QString::fromUtf8(buf, n) : QString();
    };
    static const char *types[] = {"mnt", "net", "pid", "uts", "ipc", "user", "cgroup", "time"};

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Namespaces — %1 (PID %2)", pi.name, pi.pid));
    dlg->resize(640, 560);
    auto *v = new QVBoxLayout(dlg);

    auto *tbl = new QTableWidget(0, 3, dlg);
    tbl->setHorizontalHeaderLabels({i18n("Namespace"), i18n("Inode"), i18n("Compartilhado com você?")});
    tbl->verticalHeader()->setVisible(false);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->horizontalHeader()->setStretchLastSection(true);
    int diff = 0, r = 0;
    for (const char *t : types) {
        const QString mine = readns(::getpid(), t);
        const QString theirs = readns(pi.pid, t);
        if (theirs.isEmpty())
            continue;
        const bool shared = (mine == theirs);
        if (!shared) ++diff;
        tbl->insertRow(r);
        tbl->setItem(r, 0, new QTableWidgetItem(QString::fromLatin1(t)));
        tbl->setItem(r, 1, new QTableWidgetItem(theirs));
        tbl->setItem(r, 2, new QTableWidgetItem(shared ? i18n("sim") : i18n("NÃO (isolado)")));
        ++r;
    }
    auto *lbl = new QLabel(diff > 0
        ? i18n("Isolado em %1 namespace(s) — provável container/sandbox.", diff)
        : i18n("Compartilha todos os namespaces com você (mesmo ambiente)."), dlg);
    v->addWidget(lbl);
    v->addWidget(tbl);

    auto *h = new QHBoxLayout;
    h->addWidget(new QLabel(i18n("Rodar dentro (mnt/net/ipc/uts/cgroup):"), dlg));
    auto *cmd = new QComboBox(dlg);
    cmd->setEditable(true);
    cmd->addItems({QStringLiteral("ip -brief addr"), QStringLiteral("ip route"),
                   QStringLiteral("ss -tunlp"), QStringLiteral("ls -la /"),
                   QStringLiteral("cat /etc/os-release"), QStringLiteral("mount"),
                   QStringLiteral("ps -ef")});
    h->addWidget(cmd, 1);
    auto *run = new QPushButton(i18n("Executar"), dlg);
    h->addWidget(run);
    v->addLayout(h);

    auto *out = new QPlainTextEdit(dlg);
    out->setReadOnly(true);
    out->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(out, 1);

    const int pid = pi.pid;
    const quint64 st = pi.starttime;
    connect(run, &QPushButton::clicked, dlg, [=]() {
        const QStringList parts = cmd->currentText().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (parts.isEmpty()) return;
        out->setPlainText(i18n("executando (via helper, com polkit)..."));
        helper::nsRunAsync(pid, st, parts.first(), parts.mid(1), dlg,
                           [out](const QString &res, const QString &err) {
            out->setPlainText(err.isEmpty() ? res : i18n("Erro: %1", err));
        });
    });
    dlg->show();
}

void MainWindow::ebpfCurrent()
{
    ProcInfo pi;
    if (!currentInfo(pi))
        return;

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Rastreamento eBPF — %1 (PID %2)", pi.name, pi.pid));
    dlg->resize(640, 520);
    auto *v = new QVBoxLayout(dlg);

    auto *h = new QHBoxLayout;
    auto *mode = new QComboBox(dlg);
    mode->addItem(i18n("Syscalls (contagem)"), QStringLiteral("syscalls"));
    mode->addItem(i18n("Arquivos abertos"),    QStringLiteral("files"));
    mode->addItem(i18n("Conexões TCP"),        QStringLiteral("tcp"));
    h->addWidget(new QLabel(i18n("Modo:"), dlg));
    h->addWidget(mode);
    auto *secs = new QSpinBox(dlg);
    secs->setRange(1, 30);
    secs->setValue(5);
    secs->setSuffix(i18n(" s"));
    h->addWidget(new QLabel(i18n("Duração:"), dlg));
    h->addWidget(secs);
    auto *run = new QPushButton(i18n("Rastrear"), dlg);
    h->addWidget(run);
    h->addStretch();
    v->addLayout(h);

    auto *out = new QPlainTextEdit(dlg);
    out->setReadOnly(true);
    out->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    v->addWidget(out, 1);

    const int pid = pi.pid;
    const quint64 st = pi.starttime;
    connect(run, &QPushButton::clicked, dlg, [=]() {
        run->setEnabled(false);
        out->setPlainText(i18n("rastreando %1 s com eBPF (roda no helper como root)...", secs->value()));
        helper::bpfTraceAsync(pid, st, mode->currentData().toString(), secs->value(), dlg,
                              [out, run](const QString &res, const QString &err) {
            out->setPlainText(err.isEmpty() ? res : i18n("Erro: %1", err));
            run->setEnabled(true);
        });
    });
    dlg->show();
}
