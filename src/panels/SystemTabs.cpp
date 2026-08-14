#include "SystemTabs.h"
#include "actions/ProcessActions.h"
#include "HelperClient.h"

#include <KFormat>
#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QFontDatabase>
#include <QHeaderView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <csignal>

namespace {

// RAII: preserva posição de scroll, seleção (por chave na coluna keyCol) e a
// ordenação escolhida durante um refresh que limpa e repovoa a tabela. Evita o
// "pulo" chato ao atualizar.
struct TableGuard {
    QTableWidget *t;
    int           keyCol;
    int           scroll;
    QString       selKey;

    explicit TableGuard(QTableWidget *tbl, int kc = 0) : t(tbl), keyCol(kc)
    {
        scroll = t->verticalScrollBar()->value();
        const int r = t->currentRow();
        if (r >= 0 && t->item(r, keyCol))
            selKey = t->item(r, keyCol)->text();
        t->setSortingEnabled(false);
        t->setRowCount(0);
    }
    ~TableGuard()
    {
        t->setSortingEnabled(true);   // reaplica a ordenação atual do cabeçalho
        if (!selKey.isEmpty()) {
            for (int r = 0; r < t->rowCount(); ++r)
                if (t->item(r, keyCol) && t->item(r, keyCol)->text() == selKey) {
                    t->selectRow(r);
                    break;
                }
        }
        t->verticalScrollBar()->setValue(scroll);
    }
};

QString runCmd(const QString &prog, const QStringList &args, int ms = 3000)
{
    QProcess p;
    p.start(prog, args);
    if (!p.waitForFinished(ms))
        return {};
    return QString::fromUtf8(p.readAllStandardOutput());
}

// separa "ip:porta" (v4 ou v6 com colchetes) no último ':'.
QPair<QString, QString> splitHostPort(const QString &s)
{
    const int c = s.lastIndexOf(QLatin1Char(':'));
    if (c < 0) return {s, {}};
    QString host = s.left(c);
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);
    return {host, s.mid(c + 1)};
}

} // namespace

// ===================== Serviços =====================

ServicesPanel::ServicesPanel(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_tbl = new QTableWidget(0, 6, this);
    m_tbl->setHorizontalHeaderLabels({i18n("Unidade"), i18n("Carregada"), i18n("Ativa"),
                                      i18n("Sub"), i18n("Ativação"), i18n("Descrição")});
    m_tbl->verticalHeader()->setVisible(false);
    m_tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tbl->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tbl->horizontalHeader()->setStretchLastSection(true);
    m_tbl->setSortingEnabled(true);
    m_tbl->sortByColumn(0, Qt::AscendingOrder);
    m_tbl->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_tbl);

    connect(m_tbl, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        if (currentUnit().isEmpty()) return;
        QMenu menu(this);
        connect(menu.addAction(i18n("Iniciar")),    &QAction::triggered, this, [this]{ runVerb(QStringLiteral("start")); });
        connect(menu.addAction(i18n("Parar")),      &QAction::triggered, this, [this]{ runVerb(QStringLiteral("stop")); });
        connect(menu.addAction(i18n("Reiniciar")),  &QAction::triggered, this, [this]{ runVerb(QStringLiteral("restart")); });
        connect(menu.addAction(i18n("Recarregar")), &QAction::triggered, this, [this]{ runVerb(QStringLiteral("reload")); });
        menu.addSeparator();
        connect(menu.addAction(i18n("Habilitar (no boot)")),  &QAction::triggered, this, [this]{ runVerb(QStringLiteral("enable")); });
        connect(menu.addAction(i18n("Desabilitar (no boot)")),&QAction::triggered, this, [this]{ runVerb(QStringLiteral("disable")); });
        connect(menu.addAction(i18n("Mascarar")),   &QAction::triggered, this, [this]{ runVerb(QStringLiteral("mask")); });
        connect(menu.addAction(i18n("Desmascarar")),&QAction::triggered, this, [this]{ runVerb(QStringLiteral("unmask")); });
        menu.addSeparator();
        connect(menu.addAction(i18n("Status / journal...")), &QAction::triggered, this, &ServicesPanel::showStatus);
        connect(menu.addAction(i18n("Ir para o processo")),  &QAction::triggered, this, &ServicesPanel::gotoOwner);
        menu.exec(m_tbl->viewport()->mapToGlobal(p));
    });

    auto *t = new QTimer(this);
    t->setInterval(3000);
    connect(t, &QTimer::timeout, this, &ServicesPanel::refresh);
    t->start();
    refresh();
}

QString ServicesPanel::currentUnit() const
{
    const int r = m_tbl->currentRow();
    return (r >= 0 && m_tbl->item(r, 0)) ? m_tbl->item(r, 0)->text() : QString();
}

void ServicesPanel::runVerb(const QString &verb)
{
    const QString unit = currentUnit();
    if (unit.isEmpty()) return;
    // systemctl dispara o polkit por conta própria para unidades de sistema.
    QProcess::startDetached(QStringLiteral("systemctl"), {verb, unit});
}

void ServicesPanel::showStatus()
{
    const QString unit = currentUnit();
    if (unit.isEmpty()) return;
    const QString out = runCmd(QStringLiteral("systemctl"),
                               {QStringLiteral("status"), QStringLiteral("--no-pager"),
                                QStringLiteral("--lines=50"), unit}, 4000);
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Status — %1", unit));
    dlg->resize(760, 520);
    auto *v = new QVBoxLayout(dlg);
    auto *te = new QPlainTextEdit(dlg);
    te->setReadOnly(true);
    te->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    te->setPlainText(out.isEmpty() ? i18n("(sem saída)") : out);
    v->addWidget(te);
    dlg->show();
}

void ServicesPanel::gotoOwner()
{
    const QString unit = currentUnit();
    if (unit.isEmpty()) return;
    const int pid = runCmd(QStringLiteral("systemctl"),
                           {QStringLiteral("show"), QStringLiteral("-p"),
                            QStringLiteral("MainPID"), QStringLiteral("--value"), unit}).trimmed().toInt();
    if (pid > 0)
        Q_EMIT goToProcess(pid);
    else
        KMessageBox::information(this, i18n("O serviço %1 não tem processo principal em execução.", unit));
}

void ServicesPanel::refresh()
{
    // Estado de ativação (enabled/disabled/static/masked) por unidade.
    QHash<QString, QString> enableState;
    for (const QString &line : runCmd(QStringLiteral("systemctl"),
             {QStringLiteral("list-unit-files"), QStringLiteral("--type=service"),
              QStringLiteral("--no-legend"), QStringLiteral("--plain"), QStringLiteral("--no-pager")})
             .split(QLatin1Char('\n'))) {
        const QStringList f = line.simplified().split(QLatin1Char(' '));
        if (f.size() >= 2) enableState.insert(f.at(0), f.at(1));
    }

    const QString out = runCmd(QStringLiteral("systemctl"),
             {QStringLiteral("list-units"), QStringLiteral("--type=service"),
              QStringLiteral("--all"), QStringLiteral("--no-legend"),
              QStringLiteral("--plain"), QStringLiteral("--no-pager")});
    if (out.isEmpty()) return;

    TableGuard g(m_tbl);
    int row = 0;
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        const QString l = line.trimmed();
        if (l.isEmpty()) continue;
        const QStringList f = l.split(QRegularExpression(QStringLiteral("\\s+")));
        if (f.size() < 4) continue;
        const QString unit = f.at(0), load = f.at(1), active = f.at(2), sub = f.at(3);
        m_tbl->insertRow(row);
        m_tbl->setItem(row, 0, new QTableWidgetItem(unit));
        m_tbl->setItem(row, 1, new QTableWidgetItem(load));
        m_tbl->setItem(row, 2, new QTableWidgetItem(active));
        m_tbl->setItem(row, 3, new QTableWidgetItem(sub));
        m_tbl->setItem(row, 4, new QTableWidgetItem(enableState.value(unit, QStringLiteral("-"))));
        m_tbl->setItem(row, 5, new QTableWidgetItem(f.mid(4).join(QLatin1Char(' '))));
        if (active != QLatin1String("active"))
            for (int c = 0; c < 6; ++c) m_tbl->item(row, c)->setForeground(QColor(0x9a, 0x9a, 0x9a));
        ++row;
    }
}

// ===================== Rede =====================

NetworkPanel::NetworkPanel(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_tbl = new QTableWidget(0, 5, this);
    m_tbl->setHorizontalHeaderLabels({i18n("Proto"), i18n("Estado"),
                                      i18n("Local"), i18n("Remoto"), i18n("Processo")});
    m_tbl->verticalHeader()->setVisible(false);
    m_tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tbl->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tbl->horizontalHeader()->setStretchLastSection(true);
    m_tbl->setSortingEnabled(true);
    m_tbl->sortByColumn(0, Qt::AscendingOrder);
    m_tbl->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_tbl);

    connect(m_tbl, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const int row = m_tbl->currentRow();
        if (row < 0 || !m_tbl->item(row, 4)) return;
        const int pid = m_tbl->item(row, 4)->data(Qt::UserRole).toInt();
        const QString proto = m_tbl->item(row, 0) ? m_tbl->item(row, 0)->text() : QString();
        const QString local = m_tbl->item(row, 2) ? m_tbl->item(row, 2)->text() : QString();
        const QString peer  = m_tbl->item(row, 3) ? m_tbl->item(row, 3)->text() : QString();
        auto killOwner = [this, pid](int sig) {
            if (pid <= 0) return;
            actions::Result r = actions::sendSignal(pid, 0, sig);
            if (!r.ok && r.needsPrivilege()) r = helper::sendSignal(pid, 0, sig);
            if (!r.ok) KMessageBox::error(this, r.error);
        };
        QMenu menu(this);
        auto *aClose = menu.addAction(i18n("Fechar esta conexão (ss -K)"));
        connect(aClose, &QAction::triggered, this, [this, proto, local, peer]() {
            const auto l = splitHostPort(local);
            const auto r = splitHostPort(peer);
            if (l.second.isEmpty() || r.second.isEmpty()) {
                KMessageBox::information(this, i18n("Não consegui montar o filtro da conexão."));
                return;
            }
            const QString filter = QStringLiteral("src %1 sport = %2 dst %3 dport = %4")
                                       .arg(l.first, l.second, r.first, r.second);
            // ss -K precisa de root; pkexec dispara o polkit.
            QProcess::startDetached(QStringLiteral("pkexec"), {QStringLiteral("ss"),
                                    QStringLiteral("-K"), filter});
        });
        aClose->setEnabled(proto.startsWith(QLatin1String("tcp")) || proto.startsWith(QLatin1String("udp")));
        menu.addSeparator();
        connect(menu.addAction(i18n("Matar processo (SIGTERM)")), &QAction::triggered, this, [killOwner]{ killOwner(SIGTERM); });
        connect(menu.addAction(i18n("Matar processo (SIGKILL)")), &QAction::triggered, this, [killOwner]{ killOwner(SIGKILL); });
        if (pid > 0) {
            connect(menu.addAction(i18n("Ir para o processo")), &QAction::triggered, this, [this, pid]{ Q_EMIT goToProcess(pid); });
            connect(menu.addAction(i18n("Copiar PID")), &QAction::triggered, this, [pid]{ QApplication::clipboard()->setText(QString::number(pid)); });
        }
        menu.exec(m_tbl->viewport()->mapToGlobal(p));
    });

    auto *t = new QTimer(this);
    t->setInterval(3000);
    connect(t, &QTimer::timeout, this, &NetworkPanel::refresh);
    t->start();
    refresh();
}

void NetworkPanel::refresh()
{
    const QString out = runCmd(QStringLiteral("ss"), {QStringLiteral("-tunapH")});
    static const QRegularExpression procRe(QStringLiteral("\"([^\"]+)\",pid=(\\d+)"));

    TableGuard g(m_tbl, 2); // chave de seleção pela coluna "Local"
    int row = 0;
    for (const QString &line : out.split(QLatin1Char('\n'))) {
        const QStringList f = line.simplified().split(QLatin1Char(' '));
        if (f.size() < 5) continue;
        const QString proto = f.at(0), state = f.at(1), local = f.value(4), peer = f.value(5);
        QString proc;
        int pidNum = 0;
        const auto m = procRe.match(line);
        if (m.hasMatch()) {
            proc = QStringLiteral("%1 (pid %2)").arg(m.captured(1), m.captured(2));
            pidNum = m.captured(2).toInt();
        }
        m_tbl->insertRow(row);
        m_tbl->setItem(row, 0, new QTableWidgetItem(proto));
        m_tbl->setItem(row, 1, new QTableWidgetItem(state));
        m_tbl->setItem(row, 2, new QTableWidgetItem(local));
        m_tbl->setItem(row, 3, new QTableWidgetItem(peer));
        auto *procItem = new QTableWidgetItem(proc);
        procItem->setData(Qt::UserRole, pidNum);
        m_tbl->setItem(row, 4, procItem);
        ++row;
    }
}

// ===================== Disco =====================

DiskPanel::DiskPanel(QWidget *parent) : QWidget(parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    m_tbl = new QTableWidget(0, 5, this);
    m_tbl->setHorizontalHeaderLabels({i18n("Dispositivo"), i18n("Leitura/s"),
                                      i18n("Escrita/s"), i18n("Lido (total)"), i18n("Escrito (total)")});
    m_tbl->verticalHeader()->setVisible(false);
    m_tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tbl->horizontalHeader()->setStretchLastSection(true);
    m_tbl->setSortingEnabled(true);
    m_tbl->sortByColumn(0, Qt::AscendingOrder);
    v->addWidget(m_tbl);

    auto *t = new QTimer(this);
    t->setInterval(1500);
    connect(t, &QTimer::timeout, this, &DiskPanel::refresh);
    t->start();
    refresh();
}

void DiskPanel::refresh()
{
    QFile f(QStringLiteral("/proc/diskstats"));
    if (!f.open(QIODevice::ReadOnly)) return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double dt = (m_prevMs && nowMs > m_prevMs) ? (nowMs - m_prevMs) / 1000.0 : 0.0;
    static const KFormat fmt;

    QHash<QString, QPair<quint64, quint64>> cur;
    TableGuard g(m_tbl);
    int row = 0;
    for (const QByteArray &raw : f.readAll().split('\n')) {
        const QList<QByteArray> t = raw.simplified().split(' ');
        if (t.size() < 10) continue;
        const QString name = QString::fromLatin1(t.at(2));
        if (name.startsWith(QLatin1String("loop")) || name.startsWith(QLatin1String("ram"))
            || name.startsWith(QLatin1String("dm-")) || name.startsWith(QLatin1String("zram")))
            continue;
        const quint64 sr = t.at(5).toULongLong();
        const quint64 sw = t.at(9).toULongLong();
        cur.insert(name, {sr, sw});
        double rrate = 0, wrate = 0;
        const auto it = m_prev.constFind(name);
        if (it != m_prev.constEnd() && dt > 0) {
            rrate = (sr >= it->first)  ? (sr - it->first)  * 512.0 / dt : 0;
            wrate = (sw >= it->second) ? (sw - it->second) * 512.0 / dt : 0;
        }
        m_tbl->insertRow(row);
        m_tbl->setItem(row, 0, new QTableWidgetItem(name));
        m_tbl->setItem(row, 1, new QTableWidgetItem(rrate >= 1 ? fmt.formatByteSize(rrate, 1) + QStringLiteral("/s") : QString()));
        m_tbl->setItem(row, 2, new QTableWidgetItem(wrate >= 1 ? fmt.formatByteSize(wrate, 1) + QStringLiteral("/s") : QString()));
        m_tbl->setItem(row, 3, new QTableWidgetItem(fmt.formatByteSize(sr * 512.0, 1)));
        m_tbl->setItem(row, 4, new QTableWidgetItem(fmt.formatByteSize(sw * 512.0, 1)));
        ++row;
    }
    m_prev = cur;
    m_prevMs = nowMs;
}
