#include "FindHandlesDialog.h"
#include "core/Procfs.h"
#include "inject/FdInjector.h"

#include <KLocalizedString>
#include <KMessageBox>

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTableWidget>
#include <QThread>
#include <QVBoxLayout>

#include <unistd.h>

namespace {
constexpr int kMaxHits = 5000;
}

FindHandlesDialog::FindHandlesDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(i18n("Localizar handles ou DLLs"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(860, 560);

    auto *v = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    m_term = new QLineEdit(this);
    m_term->setPlaceholderText(i18n("Nome de arquivo, socket:[inode], .so, caminho..."));
    m_term->setClearButtonEnabled(true);
    bar->addWidget(m_term, 1);
    m_kind = new QComboBox(this);
    m_kind->addItem(i18n("Tudo"));
    m_kind->addItem(i18n("Só descritores (fds)"));
    m_kind->addItem(i18n("Só bibliotecas e mapas"));
    bar->addWidget(m_kind);
    m_go = new QPushButton(QIcon::fromTheme(QStringLiteral("edit-find")), i18n("Buscar"), this);
    m_go->setDefault(true);
    bar->addWidget(m_go);
    v->addLayout(bar);

    m_tbl = new QTableWidget(0, 4, this);
    m_tbl->setObjectName(QStringLiteral("handlesTable"));
    m_tbl->setHorizontalHeaderLabels({i18n("PID"), i18n("Processo"), i18n("Tipo"), i18n("Alvo")});
    m_tbl->verticalHeader()->setVisible(false);
    m_tbl->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tbl->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tbl->horizontalHeader()->setStretchLastSection(true);
    m_tbl->setContextMenuPolicy(Qt::CustomContextMenu);
    v->addWidget(m_tbl, 1);

    m_status = new QLabel(i18n("Digite o que procurar e pressione Enter."), this);
    m_status->setObjectName(QStringLiteral("handlesStatus"));
    v->addWidget(m_status);

    connect(m_go, &QPushButton::clicked, this, &FindHandlesDialog::startSearch);
    connect(m_term, &QLineEdit::returnPressed, this, &FindHandlesDialog::startSearch);
    connect(m_tbl, &QAbstractItemView::doubleClicked, this, [this](const QModelIndex &i) {
        if (auto *it = m_tbl->item(i.row(), 0)) Q_EMIT goToProcess(it->data(Qt::UserRole).toInt());
    });
    connect(m_tbl, &QWidget::customContextMenuRequested, this, [this](const QPoint &p) {
        const int row = m_tbl->currentRow();
        if (row < 0 || !m_tbl->item(row, 0)) return;
        QTableWidgetItem *pidItem = m_tbl->item(row, 0);
        const int pid = pidItem->data(Qt::UserRole).toInt();
        const quint64 start = pidItem->data(Qt::UserRole + 1).toULongLong();
        const int fd = m_tbl->item(row, 2)->data(Qt::UserRole).toInt();
        const QString target = m_tbl->item(row, 3)->text();
        QMenu menu(this);
        connect(menu.addAction(i18n("Ir para o processo")), &QAction::triggered, this, [this, pid]{ Q_EMIT goToProcess(pid); });
        connect(menu.addAction(i18n("Copiar alvo")), &QAction::triggered, this, [target]{ QApplication::clipboard()->setText(target); });
        connect(menu.addAction(i18n("Copiar PID")), &QAction::triggered, this, [pid]{ QApplication::clipboard()->setText(QString::number(pid)); });
        if (fd >= 0) {
            menu.addSeparator();
            connect(menu.addAction(i18n("Fechar este descritor no dono...")), &QAction::triggered, this, [this, pid, start, fd, target, pidItem]() {
                if (KMessageBox::warningContinueCancel(this,
                        i18n("Injetar close(%1) no processo %2?\n\nAlvo: %3", fd, pid, target),
                        i18n("Fechar descritor alheio")) != KMessageBox::Continue)
                    return;
                // O resultado pode ser antigo: confere que o PID ainda é o MESMO processo.
                procfs::StatFields now;
                if (!procfs::readStat(pid, now) || now.starttime != start) {
                    KMessageBox::error(this, i18n("O processo %1 já encerrou; refaça a busca.", pid));
                    return;
                }
                const inject::Result r = inject::closeForeignFd(pid, fd);
                if (!r.ok) { KMessageBox::error(this, r.error); return; }
                m_tbl->removeRow(pidItem->row());
            });
        }
        menu.exec(m_tbl->viewport()->mapToGlobal(p));
    });
}

FindHandlesDialog::~FindHandlesDialog()
{
    if (m_cancel) *m_cancel = true;   // a thread de fundo para no próximo processo
}

void FindHandlesDialog::setTerm(const QString &t)
{
    m_term->setText(t);
    if (!t.isEmpty()) startSearch();
}

void FindHandlesDialog::startSearch()
{
    const QString term = m_term->text().trimmed();
    if (term.isEmpty()) return;
    if (m_cancel) *m_cancel = true;               // cancela uma busca anterior
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const int gen = ++m_generation;
    const int kind = m_kind->currentIndex();
    m_tbl->setRowCount(0);
    m_status->setText(i18n("Procurando \"%1\"...", term));
    setWindowTitle(i18n("Localizar handles ou DLLs"));

    auto cancel = m_cancel;
    QPointer<FindHandlesDialog> self(this);
    QThread *t = QThread::create([term, kind, cancel, self, gen]() {
        QList<Hit> hits;
        int scanned = 0;
        bool truncated = false;
        for (int pid : procfs::listPids()) {
            if (*cancel) return;
            if (hits.size() >= kMaxHits) { truncated = true; break; }
            ++scanned;
            procfs::StatFields st;
            if (!procfs::readStat(pid, st)) continue;
            const QString comm = st.comm;
            const quint64 start = st.starttime;
            if (kind != 2)
                for (const procfs::FdEntry &e : procfs::listFds(pid))
                    if (e.target.contains(term, Qt::CaseInsensitive))
                        hits.append({pid, start, comm, e.type, e.fd, e.target});
            if (kind != 1) {
                QSet<QString> seen;   // cada .so aparece em vários segmentos do maps
                for (const procfs::MapEntry &m : procfs::listMaps(pid))
                    if (!m.path.isEmpty() && m.path.contains(term, Qt::CaseInsensitive) && !seen.contains(m.path)) {
                        seen.insert(m.path);
                        hits.append({pid, start, comm, QStringLiteral("map"), -1, m.path});
                    }
            }
        }
        if (*cancel) return;
        QMetaObject::invokeMethod(qApp, [self, gen, hits, scanned, truncated]() {
            if (self && self->m_generation == gen)
                self->onResults(hits, scanned, truncated);
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

void FindHandlesDialog::onResults(const QList<Hit> &hits, int scanned, bool truncated)
{
    m_tbl->setSortingEnabled(false);
    m_tbl->setRowCount(hits.size());
    for (int r = 0; r < hits.size(); ++r) {
        const Hit &h = hits.at(r);
        auto *pid = new QTableWidgetItem;
        pid->setData(Qt::DisplayRole, h.pid);
        pid->setData(Qt::UserRole, h.pid);
        pid->setData(Qt::UserRole + 1, qulonglong(h.starttime));
        m_tbl->setItem(r, 0, pid);
        m_tbl->setItem(r, 1, new QTableWidgetItem(h.comm));
        auto *kind = new QTableWidgetItem(h.fd >= 0 ? i18n("fd %1 (%2)", h.fd, h.kind) : i18n("mapa"));
        kind->setData(Qt::UserRole, h.fd);
        m_tbl->setItem(r, 2, kind);
        m_tbl->setItem(r, 3, new QTableWidgetItem(h.target));
    }
    m_tbl->setSortingEnabled(true);
    m_tbl->resizeColumnToContents(0);
    m_tbl->resizeColumnToContents(1);
    m_tbl->resizeColumnToContents(2);
    QString msg = i18n("%1 resultado(s) em %2 processo(s).", hits.size(), scanned);
    if (truncated) msg += QLatin1Char(' ') + i18n("Parei em %1; refine a busca.", kMaxHits);
    if (::getuid() != 0) msg += QLatin1Char(' ') + i18n("Processos de outros usuários sem permissão de leitura ficam de fora.");
    m_status->setText(msg);
    setWindowTitle(i18n("%1 resultado(s) para \"%2\"", hits.size(), m_term->text().trimmed()));
}
