#include "ProcessProperties.h"
#include "core/Procfs.h"
#include "core/ProcInfo.h"
#include "core/ThreadController.h"
#include "actions/ProcessActions.h"
#include "inject/FdInjector.h"

#include <KFormat>
#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <unistd.h>

namespace {

QString readLinkOf(int pid, const char *what)
{
    char buf[4096];
    const QByteArray p = QStringLiteral("/proc/%1/%2").arg(pid).arg(QLatin1String(what)).toLocal8Bit();
    const ssize_t n = ::readlink(p.constData(), buf, sizeof buf - 1);
    if (n <= 0)
        return QStringLiteral("—");
    buf[n] = '\0';
    return QString::fromLocal8Bit(buf);
}

QString slurp(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QHash<QString, QString> statusMap(int pid)
{
    QHash<QString, QString> m;
    const QStringList lines = slurp(QStringLiteral("/proc/%1/status").arg(pid)).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const int c = line.indexOf(QLatin1Char(':'));
        if (c > 0) m.insert(line.left(c), line.mid(c + 1).trimmed());
    }
    return m;
}

QString decodeCaps(const QString &hex)
{
    if (hex.isEmpty() || hex == QLatin1String("0000000000000000"))
        return QStringLiteral("(nenhuma)");
    QProcess p;
    p.start(QStringLiteral("capsh"), {QStringLiteral("--decode=0x") + hex});
    if (!p.waitForFinished(2000))
        return hex;
    const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
    const int eq = out.indexOf(QLatin1Char('='));
    return eq >= 0 ? out.mid(eq + 1).trimmed() : hex;
}

void addRow(QFormLayout *form, const QString &k, const QString &v)
{
    auto *val = new QLabel(v.isEmpty() ? QStringLiteral("—") : v);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    val->setToolTip(v);
    form->addRow(new QLabel(QStringLiteral("<b>%1</b>").arg(k)), val);
}

} // namespace

ProcessProperties::ProcessProperties(int pid, quint64 starttime, const QString &name, QWidget *parent)
    : QDialog(parent), m_pid(pid), m_starttime(starttime), m_name(name)
{
    setWindowTitle(i18n("Propriedades — %1 (PID %2)", name, pid));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(720, 620);

    auto *v = new QVBoxLayout(this);
    m_header = new QLabel(i18n("<b>%1</b> (PID %2)", name, pid), this);
    v->addWidget(m_header);

    auto *tabs = new QTabWidget(this);
    tabs->addTab(buildGeneral(),     i18n("Geral"));
    tabs->addTab(buildStatistics(),  i18n("Estatísticas"));
    tabs->addTab(buildThreads(),     i18n("Threads"));
    tabs->addTab(buildToken(),       i18n("Token"));
    tabs->addTab(buildEnvironment(), i18n("Ambiente"));
    tabs->addTab(buildHandles(),     i18n("Handles"));
    tabs->addTab(buildMemory(),      i18n("Memória"));
    v->addWidget(tabs);

    auto *timer = new QTimer(this);
    timer->setInterval(1000);
    connect(timer, &QTimer::timeout, this, &ProcessProperties::refreshLive);
    timer->start();
    refreshLive();
}

QWidget *ProcessProperties::buildGeneral()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    static const KFormat fmt;

    procfs::StatFields st;
    procfs::readStat(m_pid, st);
    const quint32 uid = procfs::uidOf(m_pid);

    // horário de início: btime (boot) + starttime/HZ
    QString started = QStringLiteral("—");
    {
        long btime = 0;
        for (const QString &line : slurp(QStringLiteral("/proc/stat")).split(QLatin1Char('\n')))
            if (line.startsWith(QLatin1String("btime "))) { btime = line.mid(6).toLong(); break; }
        if (btime && procfs::clockTicksPerSec() > 0) {
            const qint64 secs = btime + qint64(m_starttime) / procfs::clockTicksPerSec();
            started = QDateTime::fromSecsSinceEpoch(secs).toString(Qt::TextDate);
        }
    }
    QStringList aff;
    for (int c : actions::getAffinity(m_pid)) aff << QString::number(c);

    addRow(form, i18n("Nome"), m_name);
    addRow(form, i18n("PID"), QString::number(m_pid));
    addRow(form, i18n("PID pai"), QString::number(st.ppid));
    addRow(form, i18n("Estado"), ProcInfo::stateText(st.state));
    addRow(form, i18n("Usuário"), QStringLiteral("%1 (uid %2)").arg(procfs::userName(uid)).arg(uid));
    addRow(form, i18n("Iniciado"), started);
    addRow(form, i18n("Executável"), readLinkOf(m_pid, "exe"));
    addRow(form, i18n("Diretório de trabalho"), readLinkOf(m_pid, "cwd"));
    addRow(form, i18n("Raiz (chroot)"), readLinkOf(m_pid, "root"));
    addRow(form, i18n("Linha de comando"), procfs::cmdlineOf(m_pid));
    addRow(form, i18n("Threads"), QString::number(st.threads));
    bool ok = false;
    const int nice = actions::getNice(m_pid, &ok);
    addRow(form, i18n("Nice"), ok ? QString::number(nice) : QStringLiteral("—"));
    addRow(form, i18n("Afinidade de CPU"), aff.join(QStringLiteral(", ")));
    return w;
}

QWidget *ProcessProperties::buildStatistics()
{
    auto *w = new QWidget(this);
    auto *l = new QVBoxLayout(w);
    m_stats = new QTableWidget(0, 2, w);
    m_stats->setHorizontalHeaderLabels({i18n("Métrica"), i18n("Valor")});
    m_stats->verticalHeader()->setVisible(false);
    m_stats->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_stats->horizontalHeader()->setStretchLastSection(true);
    l->addWidget(m_stats);
    return w;
}

QWidget *ProcessProperties::buildThreads()
{
    auto *w = new QWidget(this);
    auto *l = new QVBoxLayout(w);
    m_threads = new QTableWidget(0, 3, w);
    m_threads->setHorizontalHeaderLabels({i18n("TID"), i18n("Nome"), i18n("Estado")});
    m_threads->verticalHeader()->setVisible(false);
    m_threads->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_threads->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_threads->horizontalHeader()->setStretchLastSection(true);
    l->addWidget(m_threads);
    auto *h = new QHBoxLayout;
    auto *bSus = new QPushButton(i18n("Suspender thread"), w);
    auto *bRes = new QPushButton(i18n("Retomar thread"), w);
    h->addWidget(bSus); h->addWidget(bRes); h->addStretch();
    l->addLayout(h);
    auto tid = [this]() -> int {
        const int r = m_threads->currentRow();
        return (r >= 0 && m_threads->item(r, 0)) ? m_threads->item(r, 0)->text().toInt() : 0;
    };
    connect(bSus, &QPushButton::clicked, this, [this, tid]() {
        const int t = tid(); if (!t) return; QString e;
        if (!ThreadController::instance().suspend(t, &e)) KMessageBox::error(this, e);
    });
    connect(bRes, &QPushButton::clicked, this, [this, tid]() {
        const int t = tid(); if (!t) return; QString e;
        if (!ThreadController::instance().resume(t, &e)) KMessageBox::error(this, e);
    });
    return w;
}

QWidget *ProcessProperties::buildToken()
{
    auto *w = new QWidget(this);
    auto *form = new QFormLayout(w);
    const auto s = statusMap(m_pid);

    const QStringList uid = s.value(QStringLiteral("Uid")).split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    const QStringList gid = s.value(QStringLiteral("Gid")).split(QLatin1Char('\t'), Qt::SkipEmptyParts);
    auto quad = [](const QStringList &q) {
        return q.size() >= 4 ? i18n("real %1 · efetivo %2 · salvo %3 · fs %4", q[0], q[1], q[2], q[3])
                             : q.join(QLatin1Char(' '));
    };
    addRow(form, i18n("UID"), quad(uid));
    addRow(form, i18n("GID"), quad(gid));
    addRow(form, i18n("Grupos"), s.value(QStringLiteral("Groups")));
    addRow(form, i18n("Capabilities efetivas"), decodeCaps(s.value(QStringLiteral("CapEff"))));
    addRow(form, i18n("Capabilities permitidas"), decodeCaps(s.value(QStringLiteral("CapPrm"))));
    addRow(form, i18n("Capabilities herdáveis"), decodeCaps(s.value(QStringLiteral("CapInh"))));
    addRow(form, i18n("Capabilities do bounding set"), decodeCaps(s.value(QStringLiteral("CapBnd"))));
    addRow(form, i18n("Capabilities ambientais"), decodeCaps(s.value(QStringLiteral("CapAmb"))));
    const QString sec = s.value(QStringLiteral("Seccomp"));
    addRow(form, i18n("Seccomp"), sec == QLatin1String("2") ? i18n("filtro (BPF)")
                                : sec == QLatin1String("1") ? i18n("estrito") : i18n("desativado"));
    addRow(form, i18n("NoNewPrivs"), s.value(QStringLiteral("NoNewPrivs")) == QLatin1String("1") ? i18n("sim") : i18n("não"));
    addRow(form, i18n("Contexto de segurança"),
           slurp(QStringLiteral("/proc/%1/attr/current").arg(m_pid)).trimmed());
    return w;
}

QWidget *ProcessProperties::buildEnvironment()
{
    auto *w = new QWidget(this);
    auto *l = new QVBoxLayout(w);
    auto *tbl = new QTableWidget(0, 2, w);
    tbl->setHorizontalHeaderLabels({i18n("Variável"), i18n("Valor")});
    tbl->verticalHeader()->setVisible(false);
    tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tbl->horizontalHeader()->setStretchLastSection(true);
    tbl->setSortingEnabled(true);
    l->addWidget(tbl);

    QFile f(QStringLiteral("/proc/%1/environ").arg(m_pid));
    if (f.open(QIODevice::ReadOnly)) {
        int row = 0;
        for (const QByteArray &e : f.readAll().split('\0')) {
            if (e.isEmpty()) continue;
            const QString s = QString::fromUtf8(e);
            const int eq = s.indexOf(QLatin1Char('='));
            tbl->insertRow(row);
            tbl->setItem(row, 0, new QTableWidgetItem(eq >= 0 ? s.left(eq) : s));
            tbl->setItem(row, 1, new QTableWidgetItem(eq >= 0 ? s.mid(eq + 1) : QString()));
            ++row;
        }
    } else {
        l->addWidget(new QLabel(i18n("Sem permissão para ler o ambiente (uid diferente)."), w));
    }
    tbl->sortByColumn(0, Qt::AscendingOrder);
    return w;
}

QWidget *ProcessProperties::buildHandles()
{
    auto *w = new QWidget(this);
    auto *l = new QVBoxLayout(w);
    m_handles = new QTableWidget(0, 4, w);
    m_handles->setHorizontalHeaderLabels({i18n("FD"), i18n("Tipo"), i18n("Alvo"), i18n("Flags")});
    m_handles->verticalHeader()->setVisible(false);
    m_handles->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_handles->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_handles->setSortingEnabled(true);
    m_handles->horizontalHeader()->setStretchLastSection(true);
    l->addWidget(m_handles);
    auto *h = new QHBoxLayout;
    auto *bR = new QPushButton(i18n("Atualizar"), w);
    auto *bC = new QPushButton(i18n("Fechar no dono"), w);
    h->addWidget(bR); h->addWidget(bC); h->addStretch();
    l->addLayout(h);
    connect(bR, &QPushButton::clicked, this, &ProcessProperties::reloadHandles);
    connect(bC, &QPushButton::clicked, this, [this]() {
        const int r = m_handles->currentRow();
        if (r < 0 || !m_handles->item(r, 0)) return;
        const int fd = m_handles->item(r, 0)->text().toInt();
        const inject::Result res = inject::closeForeignFd(m_pid, fd);
        if (!res.ok) KMessageBox::error(this, res.error);
        reloadHandles();
    });
    reloadHandles();
    return w;
}

QWidget *ProcessProperties::buildMemory()
{
    auto *w = new QWidget(this);
    auto *l = new QVBoxLayout(w);
    m_maps = new QTableWidget(0, 4, w);
    m_maps->setHorizontalHeaderLabels({i18n("Base"), i18n("Perms"), i18n("Tamanho"), i18n("Arquivo")});
    m_maps->verticalHeader()->setVisible(false);
    m_maps->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_maps->setSortingEnabled(true);
    m_maps->horizontalHeader()->setStretchLastSection(true);
    l->addWidget(m_maps);
    auto *bR = new QPushButton(i18n("Atualizar"), w);
    l->addWidget(bR, 0, Qt::AlignLeft);
    connect(bR, &QPushButton::clicked, this, &ProcessProperties::reloadMemory);
    reloadMemory();
    return w;
}

void ProcessProperties::reloadHandles()
{
    m_handles->setSortingEnabled(false);
    m_handles->setRowCount(0);
    int row = 0;
    for (const procfs::FdEntry &e : procfs::listFds(m_pid)) {
        m_handles->insertRow(row);
        auto *fd = new QTableWidgetItem;
        fd->setData(Qt::DisplayRole, e.fd);
        m_handles->setItem(row, 0, fd);
        m_handles->setItem(row, 1, new QTableWidgetItem(e.type));
        m_handles->setItem(row, 2, new QTableWidgetItem(e.target));
        m_handles->setItem(row, 3, new QTableWidgetItem(e.flags));
        ++row;
    }
    m_handles->setSortingEnabled(true);
}

void ProcessProperties::reloadMemory()
{
    static const KFormat fmt;
    m_maps->setSortingEnabled(false);
    m_maps->setRowCount(0);
    int row = 0;
    for (const procfs::MapEntry &m : procfs::listMaps(m_pid)) {
        m_maps->insertRow(row);
        m_maps->setItem(row, 0, new QTableWidgetItem(QStringLiteral("0x%1").arg(m.start, 0, 16)));
        m_maps->setItem(row, 1, new QTableWidgetItem(m.perms));
        m_maps->setItem(row, 2, new QTableWidgetItem(fmt.formatByteSize(m.end - m.start, 1)));
        m_maps->setItem(row, 3, new QTableWidgetItem(m.path.isEmpty() ? QStringLiteral("[anônimo]") : m.path));
        ++row;
    }
    m_maps->setSortingEnabled(true);
}

void ProcessProperties::refreshLive()
{
    static const KFormat fmt;
    const auto s = statusMap(m_pid);
    procfs::StatFields st;
    const bool alive = procfs::readStat(m_pid, st);
    const double hz = double(procfs::clockTicksPerSec());

    quint64 io = 0;
    procfs::ioBytesOf(m_pid, &io);
    QString ioRead = QStringLiteral("—"), ioWrite = QStringLiteral("—");
    {
        for (const QString &line : slurp(QStringLiteral("/proc/%1/io").arg(m_pid)).split(QLatin1Char('\n'))) {
            if (line.startsWith(QLatin1String("read_bytes:")))  ioRead  = fmt.formatByteSize(line.mid(11).trimmed().toULongLong(), 2);
            if (line.startsWith(QLatin1String("write_bytes:"))) ioWrite = fmt.formatByteSize(line.mid(12).trimmed().toULongLong(), 2);
        }
    }

    // stat: minflt=campo10 majflt=campo12 (após comm). Reusamos utime/stime já parseados.
    const QList<QPair<QString, QString>> rows = {
        {i18n("Tempo de CPU (usuário)"), QStringLiteral("%1 s").arg(alive ? st.utime / hz : 0, 0, 'f', 2)},
        {i18n("Tempo de CPU (sistema)"), QStringLiteral("%1 s").arg(alive ? st.stime / hz : 0, 0, 'f', 2)},
        {i18n("Memória (RSS)"),   s.value(QStringLiteral("VmRSS"))},
        {i18n("Memória virtual"), s.value(QStringLiteral("VmSize"))},
        {i18n("Dados/heap"),      s.value(QStringLiteral("VmData"))},
        {i18n("Pilha"),           s.value(QStringLiteral("VmStk"))},
        {i18n("Swap"),            s.value(QStringLiteral("VmSwap"))},
        {i18n("Disco lido"),      ioRead},
        {i18n("Disco escrito"),   ioWrite},
        {i18n("Trocas de contexto (voluntárias)"),   s.value(QStringLiteral("voluntary_ctxt_switches"))},
        {i18n("Trocas de contexto (involuntárias)"), s.value(QStringLiteral("nonvoluntary_ctxt_switches"))},
        {i18n("Threads"),         QString::number(st.threads)},
    };
    if (m_stats->rowCount() != rows.size()) m_stats->setRowCount(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        if (!m_stats->item(i, 0)) m_stats->setItem(i, 0, new QTableWidgetItem);
        if (!m_stats->item(i, 1)) m_stats->setItem(i, 1, new QTableWidgetItem);
        m_stats->item(i, 0)->setText(rows[i].first);
        m_stats->item(i, 1)->setText(rows[i].second.isEmpty() ? QStringLiteral("—") : rows[i].second);
    }

    // Threads ao vivo (preserva seleção por TID).
    const int keep = (m_threads->currentRow() >= 0 && m_threads->item(m_threads->currentRow(), 0))
                         ? m_threads->item(m_threads->currentRow(), 0)->text().toInt() : 0;
    const QStringList tids = QDir(QStringLiteral("/proc/%1/task").arg(m_pid))
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    m_threads->setRowCount(tids.size());
    int r = 0, keepRow = -1;
    for (const QString &t : tids) {
        const int tid = t.toInt();
        const QString comm = slurp(QStringLiteral("/proc/%1/task/%2/comm").arg(m_pid).arg(tid)).trimmed();
        char stc = '?';
        { const QByteArray b = slurp(QStringLiteral("/proc/%1/task/%2/stat").arg(m_pid).arg(tid)).toUtf8();
          const int rp = b.lastIndexOf(')'); if (rp > 0 && rp + 2 < b.size()) stc = b[rp + 2]; }
        QString state = ProcInfo::stateText(stc);
        if (ThreadController::instance().isSuspended(tid)) state += i18n("  [suspensa]");
        if (!m_threads->item(r, 0)) for (int c = 0; c < 3; ++c) m_threads->setItem(r, c, new QTableWidgetItem);
        m_threads->item(r, 0)->setText(QString::number(tid));
        m_threads->item(r, 1)->setText(comm);
        m_threads->item(r, 2)->setText(state);
        if (tid == keep) keepRow = r;
        ++r;
    }
    if (keepRow >= 0) m_threads->selectRow(keepRow);
}
