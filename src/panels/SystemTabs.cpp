#include "SystemTabs.h"
#include "actions/ProcessActions.h"
#include "core/ProcessIcons.h"
#include "core/Procfs.h"
#include "HelperClient.h"

#include <KFormat>
#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QShowEvent>
#include <QHeaderView>
#include <QMenu>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QTableWidget>
#include <QPointer>
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

// Item que ordena por um valor numérico guardado em SortKey, não pelo texto
// formatado ("9,5 MiB" < "10 KiB" em ordem alfabética seria errado).
constexpr int SortKey = Qt::UserRole + 7;
class NumItem : public QTableWidgetItem {
public:
    NumItem(const QString &text, double key) : QTableWidgetItem(text) {
        setData(SortKey, key);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem &o) const override {
        return data(SortKey).toDouble() < o.data(SortKey).toDouble();
    }
};

// Mostra só as linhas cujo texto (em qualquer coluna) contém o filtro.
int filterRows(QTableWidget *t, const QString &needle, const std::function<bool(int)> &extra = {})
{
    int shown = 0;
    for (int r = 0; r < t->rowCount(); ++r) {
        bool match = needle.isEmpty();
        for (int c = 0; !match && c < t->columnCount(); ++c)
            if (auto *it = t->item(r, c))
                match = it->text().contains(needle, Qt::CaseInsensitive);
        if (match && extra && !extra(r))
            match = false;
        t->setRowHidden(r, !match);
        if (match) ++shown;
    }
    return shown;
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

// Endereço no formato que o filtro do ss aceita: sem o "%interface" e com
// colchetes no IPv6 (senão o ':' do endereço confunde o parser de porta).
QString ssHost(QString host)
{
    const int pct = host.indexOf(QLatin1Char('%'));
    if (pct >= 0) host.truncate(pct);
    if (host.contains(QLatin1Char(':')))
        host = QLatin1Char('[') + host + QLatin1Char(']');
    return host;
}

// PIDs com "Cortar rede" ativo agora: um .netblock por pid em /run/procforge/
// (mundialmente legível — ver Helper::echoTo). Fonte da verdade é o próprio
// helper; a GUI só lê.
QList<int> blockedPids()
{
    QList<int> out;
    const QDir d(QStringLiteral("/run/procforge"));
    for (const QString &name : d.entryList({QStringLiteral("*.netblock")}, QDir::Files)) {
        bool ok = false;
        const int pid = name.chopped(9).toInt(&ok); // remove ".netblock" (9 chars)
        if (ok && pid > 0) out.append(pid);
    }
    return out;
}

} // namespace

// ===================== Base =====================

LivePanel::LivePanel(int intervalMs, QWidget *parent) : QWidget(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(intervalMs);
    connect(m_timer, &QTimer::timeout, this, [this]() { refresh(); });
}

void LivePanel::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (!m_timer->isActive()) {
        m_timer->start();
        refresh();
    }
}

void LivePanel::hideEvent(QHideEvent *e)
{
    QWidget::hideEvent(e);
    m_timer->stop();
}

LivePanel::~LivePanel()
{
    // Comandos ainda rodando (systemctl lento, pkexec esperando senha): solta
    // os callbacks antes de ~QWidget apagar os filhos, senão o finished do
    // ~QProcess chegaria num painel já meio destruído.
    for (QProcess *p : findChildren<QProcess*>(Qt::FindDirectChildrenOnly)) {
        QObject::disconnect(p, nullptr, this, nullptr);
        if (p->state() != QProcess::NotRunning)
            p->kill();
    }
}

void LivePanel::runAsync(const QString &prog, const QStringList &args,
                         std::function<void(const QString &, const QString &, int)> cb)
{
    auto *p = new QProcess(this);
    connect(p, &QProcess::finished, this, [p, cb](int code, QProcess::ExitStatus st) {
        cb(QString::fromUtf8(p->readAllStandardOutput()),
           QString::fromUtf8(p->readAllStandardError()),
           st == QProcess::NormalExit ? code : -1);
        p->deleteLater();
    });
    connect(p, &QProcess::errorOccurred, this, [p, cb, prog](QProcess::ProcessError err) {
        if (err != QProcess::FailedToStart)
            return;
        cb(QString(), i18n("Não consegui executar %1.", prog), -1);
        p->deleteLater();
    });
    p->start(prog, args);
}

// ===================== Serviços =====================

ServicesPanel::ServicesPanel(QWidget *parent) : LivePanel(3000, parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);

    auto *bar = new QHBoxLayout;
    m_scope = new QComboBox(this);
    m_scope->addItem(i18n("Serviços do sistema"));
    m_scope->addItem(i18n("Serviços do usuário"));
    bar->addWidget(m_scope);
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(i18n("Filtrar serviços"));
    m_filter->setClearButtonEnabled(true);
    bar->addWidget(m_filter, 1);
    m_onlyActive = new QCheckBox(i18n("Só ativos"), this);
    bar->addWidget(m_onlyActive);
    m_count = new QLabel(this);
    bar->addWidget(m_count);
    v->addLayout(bar);

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

    connect(m_filter, &QLineEdit::textChanged, this, &ServicesPanel::applyFilter);
    connect(m_onlyActive, &QCheckBox::toggled, this, &ServicesPanel::applyFilter);
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this]() {
        m_tbl->setRowCount(0);
        refresh();
    });
    connect(m_tbl, &QAbstractItemView::doubleClicked, this, &ServicesPanel::showStatus);

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
        connect(menu.addAction(i18n("Copiar nome")), &QAction::triggered, this, [this]{
            QApplication::clipboard()->setText(currentUnit()); });
        menu.exec(m_tbl->viewport()->mapToGlobal(p));
    });
}

QStringList ServicesPanel::scopeArgs() const
{
    return m_scope->currentIndex() == 1 ? QStringList{QStringLiteral("--user")} : QStringList{};
}

QString ServicesPanel::currentUnit() const
{
    const int r = m_tbl->currentRow();
    return (r >= 0 && m_tbl->item(r, 0)) ? m_tbl->item(r, 0)->text() : QString();
}

void ServicesPanel::applyFilter()
{
    const int shown = filterRows(m_tbl, m_filter->text().trimmed(), [this](int r) {
        return !m_onlyActive->isChecked()
            || (m_tbl->item(r, 2) && m_tbl->item(r, 2)->text() == QLatin1String("active"));
    });
    m_count->setText(i18n("%1 de %2", shown, m_tbl->rowCount()));
}

void ServicesPanel::runVerb(const QString &verb)
{
    const QString unit = currentUnit();
    if (unit.isEmpty()) return;
    // systemctl pede a autorização ao polkit por conta própria (unidades de sistema).
    runAsync(QStringLiteral("systemctl"), scopeArgs() + QStringList{verb, unit},
             [this, verb, unit](const QString &, const QString &err, int code) {
        if (code != 0)
            KMessageBox::error(this, i18n("systemctl %1 %2 falhou:\n%3", verb, unit,
                                          err.trimmed().isEmpty() ? i18n("código %1", code) : err.trimmed()));
        refresh();
    });
}

void ServicesPanel::showStatus()
{
    const QString unit = currentUnit();
    if (unit.isEmpty()) return;
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Status — %1", unit));
    dlg->resize(760, 520);
    auto *v = new QVBoxLayout(dlg);
    auto *te = new QPlainTextEdit(dlg);
    te->setReadOnly(true);
    te->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    te->setPlainText(i18n("carregando..."));
    v->addWidget(te);
    dlg->show();
    QPointer<QPlainTextEdit> guard(te);
    runAsync(QStringLiteral("systemctl"),
             scopeArgs() + QStringList{QStringLiteral("status"), QStringLiteral("--no-pager"),
                                       QStringLiteral("--lines=50"), QStringLiteral("--"), unit},
             [guard](const QString &out, const QString &err, int) {
        if (!guard) return;
        const QString text = (out + err).trimmed();
        guard->setPlainText(text.isEmpty() ? i18n("(sem saída)") : text);
    });
}

void ServicesPanel::gotoOwner()
{
    const QString unit = currentUnit();
    if (unit.isEmpty()) return;
    runAsync(QStringLiteral("systemctl"),
             scopeArgs() + QStringList{QStringLiteral("show"), QStringLiteral("-p"),
                                       QStringLiteral("MainPID"), QStringLiteral("--value"), unit},
             [this, unit](const QString &out, const QString &, int) {
        const int pid = out.trimmed().toInt();
        if (pid > 0)
            Q_EMIT goToProcess(pid);
        else
            KMessageBox::information(this, i18n("O serviço %1 não tem processo principal em execução.", unit));
    });
}

void ServicesPanel::refresh()
{
    if (m_busy) return;          // a consulta anterior ainda não voltou
    m_busy = true;
    const QStringList scope = scopeArgs();
    // Estado de ativação (enabled/disabled/static/masked) por unidade, depois a lista.
    runAsync(QStringLiteral("systemctl"),
             scope + QStringList{QStringLiteral("list-unit-files"), QStringLiteral("--type=service"),
                                 QStringLiteral("--no-legend"), QStringLiteral("--plain"), QStringLiteral("--no-pager")},
             [this, scope](const QString &files, const QString &, int) {
        QHash<QString, QString> enableState;
        for (const QString &line : files.split(QLatin1Char('\n'))) {
            const QStringList f = line.simplified().split(QLatin1Char(' '));
            if (f.size() >= 2) enableState.insert(f.at(0), f.at(1));
        }
        runAsync(QStringLiteral("systemctl"),
                 scope + QStringList{QStringLiteral("list-units"), QStringLiteral("--type=service"),
                                     QStringLiteral("--all"), QStringLiteral("--no-legend"),
                                     QStringLiteral("--plain"), QStringLiteral("--no-pager")},
                 [this, enableState, scope](const QString &out, const QString &, int) {
            m_busy = false;
            if (scope != scopeArgs()) { refresh(); return; }   // trocou de escopo no meio
            if (out.isEmpty()) { applyFilter(); return; }
            {
                TableGuard g(m_tbl);
                int row = 0;
                static const QRegularExpression ws(QStringLiteral("\\s+"));
                for (const QString &line : out.split(QLatin1Char('\n'))) {
                    const QString l = line.trimmed();
                    if (l.isEmpty()) continue;
                    const QStringList f = l.split(ws);
                    if (f.size() < 4) continue;
                    const QString unit = f.at(0), load = f.at(1), active = f.at(2), sub = f.at(3);
                    m_tbl->insertRow(row);
                    m_tbl->setItem(row, 0, new QTableWidgetItem(unit));
                    m_tbl->setItem(row, 1, new QTableWidgetItem(load));
                    m_tbl->setItem(row, 2, new QTableWidgetItem(active));
                    m_tbl->setItem(row, 3, new QTableWidgetItem(sub));
                    m_tbl->setItem(row, 4, new QTableWidgetItem(enableState.value(unit, QStringLiteral("-"))));
                    m_tbl->setItem(row, 5, new QTableWidgetItem(f.mid(4).join(QLatin1Char(' '))));
                    if (active == QLatin1String("failed"))
                        for (int c = 0; c < 6; ++c) m_tbl->item(row, c)->setForeground(QColor(0xd0, 0x40, 0x40));
                    else if (active != QLatin1String("active"))
                        for (int c = 0; c < 6; ++c) m_tbl->item(row, c)->setForeground(QColor(0x9a, 0x9a, 0x9a));
                    ++row;
                }
            }
            applyFilter();
        });
    });
}

// ===================== Rede =====================

NetworkPanel::NetworkPanel(QWidget *parent) : LivePanel(3000, parent)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);

    // ---- processos com "Cortar rede" ativo (só aparece quando há algum) ----
    m_blockedBox = new QGroupBox(i18n("Processos com a rede cortada"), this);
    auto *bv = new QVBoxLayout(m_blockedBox);
    m_blockedTbl = new QTableWidget(0, 3, m_blockedBox);
    m_blockedTbl->setHorizontalHeaderLabels({i18n("Processo"), i18n("PID"), i18n("Caminho")});
    m_blockedTbl->verticalHeader()->setVisible(false);
    m_blockedTbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_blockedTbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_blockedTbl->setSelectionMode(QAbstractItemView::SingleSelection);
    m_blockedTbl->horizontalHeader()->setStretchLastSection(true);
    m_blockedTbl->setMaximumHeight(140);
    m_blockedTbl->setContextMenuPolicy(Qt::CustomContextMenu);
    bv->addWidget(m_blockedTbl);
    v->addWidget(m_blockedBox);
    m_blockedBox->setVisible(false); // só mostra quando há pelo menos um bloqueado

    connect(m_blockedTbl, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const int row = m_blockedTbl->currentRow();
        if (row < 0 || !m_blockedTbl->item(row, 1)) return;
        const int pid = m_blockedTbl->item(row, 1)->text().toInt();
        QMenu menu(this);
        connect(menu.addAction(i18n("Restabelecer rede")), &QAction::triggered, this, [this, pid]() {
            procfs::StatFields st;
            const quint64 starttime = procfs::readStat(pid, st) ? st.starttime : 0;
            helper::netBlockAsync(pid, starttime, false, this, [this](actions::Result r) {
                if (!r.ok) KMessageBox::error(this, r.error);
                else refreshBlocked();
            });
        });
        connect(menu.addAction(i18n("Ir para o processo")), &QAction::triggered, this, [this, pid]{ Q_EMIT goToProcess(pid); });
        connect(menu.addAction(i18n("Copiar PID")), &QAction::triggered, this, [pid]{ QApplication::clipboard()->setText(QString::number(pid)); });
        menu.exec(m_blockedTbl->viewport()->mapToGlobal(p));
    });

    auto *bar = new QHBoxLayout;
    m_filter = new QLineEdit(this);
    m_filter->setPlaceholderText(i18n("Filtrar conexões (endereço, porta, processo)"));
    m_filter->setClearButtonEnabled(true);
    bar->addWidget(m_filter, 1);
    m_onlyListen = new QCheckBox(i18n("Só portas abertas (LISTEN)"), this);
    bar->addWidget(m_onlyListen);
    m_count = new QLabel(this);
    bar->addWidget(m_count);
    v->addLayout(bar);
    connect(m_filter, &QLineEdit::textChanged, this, &NetworkPanel::applyFilter);
    connect(m_onlyListen, &QCheckBox::toggled, this, &NetworkPanel::applyFilter);

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
            procfs::StatFields st;
            const quint64 starttime = procfs::readStat(pid, st) ? st.starttime : 0;
            actions::Result r = actions::sendSignal(pid, starttime, sig);
            if (r.ok || !r.needsPrivilege()) {
                if (!r.ok) KMessageBox::error(this, r.error);
                return;
            }
            helper::sendSignalAsync(pid, starttime, sig, this, [this](actions::Result hr) {
                if (!hr.ok) KMessageBox::error(this, hr.error);
            });
        };
        QMenu menu(this);
        auto *aClose = menu.addAction(i18n("Fechar esta conexão (ss -K)"));
        connect(aClose, &QAction::triggered, this, [this, local, peer]() {
            const auto l = splitHostPort(local);
            const auto r = splitHostPort(peer);
            if (l.second.isEmpty() || r.second.isEmpty() || r.second == QLatin1String("*")) {
                KMessageBox::information(this, i18n("Não consegui montar o filtro da conexão."));
                return;
            }
            // ss -K precisa de root; pkexec dispara o polkit. Cada pedaço do
            // filtro vai como argumento próprio (sem shell).
            runAsync(QStringLiteral("pkexec"),
                     {QStringLiteral("ss"), QStringLiteral("-K"),
                      QStringLiteral("src"), ssHost(l.first), QStringLiteral("sport"), QStringLiteral("="), l.second,
                      QStringLiteral("dst"), ssHost(r.first), QStringLiteral("dport"), QStringLiteral("="), r.second},
                     [this](const QString &, const QString &err, int code) {
                if (code != 0 && code != 126 && !err.trimmed().isEmpty())   // 126 = cancelado no polkit
                    KMessageBox::error(this, err.trimmed());
                refresh();
            });
        });
        aClose->setEnabled((proto.startsWith(QLatin1String("tcp")) || proto.startsWith(QLatin1String("udp")))
                           && !peer.endsWith(QLatin1String(":*")));
        menu.addSeparator();
        connect(menu.addAction(i18n("Matar processo (SIGTERM)")), &QAction::triggered, this, [killOwner]{ killOwner(SIGTERM); });
        connect(menu.addAction(i18n("Matar processo (SIGKILL)")), &QAction::triggered, this, [killOwner]{ killOwner(SIGKILL); });
        if (pid > 0) {
            connect(menu.addAction(i18n("Ir para o processo")), &QAction::triggered, this, [this, pid]{ Q_EMIT goToProcess(pid); });
            connect(menu.addAction(i18n("Copiar PID")), &QAction::triggered, this, [pid]{ QApplication::clipboard()->setText(QString::number(pid)); });
        }
        connect(menu.addAction(i18n("Copiar endereço remoto")), &QAction::triggered, this, [peer]{ QApplication::clipboard()->setText(peer); });
        menu.exec(m_tbl->viewport()->mapToGlobal(p));
    });
}

void NetworkPanel::applyFilter()
{
    const int shown = filterRows(m_tbl, m_filter->text().trimmed(), [this](int r) {
        if (!m_onlyListen->isChecked()) return true;
        const QString st = m_tbl->item(r, 1) ? m_tbl->item(r, 1)->text() : QString();
        return st == QLatin1String("LISTEN") || st == QLatin1String("UNCONN");
    });
    m_count->setText(i18n("%1 de %2", shown, m_tbl->rowCount()));
}

void NetworkPanel::refreshBlocked()
{
    const QList<int> pids = blockedPids();
    m_blockedBox->setVisible(!pids.isEmpty());
    if (pids.isEmpty()) {
        m_blockedTbl->setRowCount(0);
        return;
    }

    TableGuard g(m_blockedTbl, 1); // chave de seleção pela coluna PID
    int row = 0;
    for (int pid : pids) {
        procfs::StatFields st;
        if (!procfs::readStat(pid, st))
            continue; // saiu/morreu; o marcador ainda não foi limpo
        ProcInfo pi;
        pi.pid = pid;
        pi.name = st.comm;
        pi.cmdline = procfs::cmdlineOf(pid);
        pi.kernel = pi.cmdline.isEmpty();

        m_blockedTbl->insertRow(row);
        auto *nameItem = new QTableWidgetItem(proc_icons::iconFor(pi), pi.name);
        m_blockedTbl->setItem(row, 0, nameItem);
        m_blockedTbl->setItem(row, 1, new QTableWidgetItem(QString::number(pid)));
        m_blockedTbl->setItem(row, 2, new QTableWidgetItem(pi.cmdline));
        ++row;
    }
}

void NetworkPanel::refresh()
{
    refreshBlocked();
    if (m_busy) return;
    m_busy = true;
    runAsync(QStringLiteral("ss"), {QStringLiteral("-tunapH")},
             [this](const QString &out, const QString &, int) {
        m_busy = false;
        static const QRegularExpression procRe(QStringLiteral("\"([^\"]+)\",pid=(\\d+)"));
        {
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
        applyFilter();
    });
}

// ===================== Disco =====================

DiskPanel::DiskPanel(QWidget *parent) : LivePanel(1500, parent)
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
        m_tbl->setItem(row, 1, new NumItem(rrate >= 1 ? fmt.formatByteSize(rrate, 1) + QStringLiteral("/s") : QString(), rrate));
        m_tbl->setItem(row, 2, new NumItem(wrate >= 1 ? fmt.formatByteSize(wrate, 1) + QStringLiteral("/s") : QString(), wrate));
        m_tbl->setItem(row, 3, new NumItem(fmt.formatByteSize(sr * 512.0, 1), double(sr)));
        m_tbl->setItem(row, 4, new NumItem(fmt.formatByteSize(sw * 512.0, 1), double(sw)));
        ++row;
    }
    m_prev = cur;
    m_prevMs = nowMs;
}
