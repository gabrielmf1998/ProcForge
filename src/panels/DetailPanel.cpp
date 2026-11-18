#include "DetailPanel.h"
#include "inject/FdInjector.h"

#include <KFormat>
#include <KLocalizedString>
#include <KMessageBox>

#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QPointer>
#include <QThread>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTableView>
#include <QToolBar>
#include <QVBoxLayout>
#include <QHBoxLayout>

// =================== FdTableModel ===================

QVariant FdTableModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid())
        return {};
    const procfs::FdEntry &e = m_rows.at(i.row());
    if (role == Qt::DisplayRole) {
        switch (i.column()) {
        case ColFd:     return e.fd;
        case ColType:   return e.type;
        case ColTarget: return e.target;
        case ColFlags:  return e.flags;
        }
    }
    if (role == Qt::TextAlignmentRole && i.column() == ColFd)
        return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

QVariant FdTableModel::headerData(int s, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (s) {
    case ColFd:     return i18n("FD");
    case ColType:   return i18n("Tipo");
    case ColTarget: return i18n("Alvo");
    case ColFlags:  return i18n("Flags");
    }
    return {};
}

void FdTableModel::setRows(const QList<procfs::FdEntry> &r)
{
    beginResetModel();
    m_rows = r;
    endResetModel();
}

// =================== MapsTableModel ===================

QVariant MapsTableModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid())
        return {};
    const procfs::MapEntry &m = m_rows.at(i.row());
    static const KFormat fmt;
    if (role == Qt::DisplayRole) {
        switch (i.column()) {
        case ColBase:   return QStringLiteral("0x%1").arg(m.start, 0, 16);
        case ColPerms:  return m.perms;
        case ColOffset: return QStringLiteral("0x%1").arg(m.offset, 0, 16);
        case ColSize:   return fmt.formatByteSize(m.end - m.start, 1);
        case ColPath:   return m.path.isEmpty() ? QStringLiteral("[anônimo]") : m.path;
        }
    }
    if (role == Qt::TextAlignmentRole && (i.column() == ColSize))
        return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
}

QVariant MapsTableModel::headerData(int s, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (s) {
    case ColBase:   return i18n("Base");
    case ColPerms:  return i18n("Perms");
    case ColOffset: return i18n("Offset");
    case ColSize:   return i18n("Tamanho");
    case ColPath:   return i18n("Arquivo mapeado");
    }
    return {};
}

void MapsTableModel::setRows(const QList<procfs::MapEntry> &r, bool onlyLibs)
{
    beginResetModel();
    m_rows.clear();
    for (const procfs::MapEntry &m : r) {
        if (onlyLibs) {
            const bool isLib = m.path.contains(QLatin1String(".so"))
                            && m.perms.contains(QLatin1Char('x'));
            if (!isLib)
                continue;
        }
        m_rows.append(m);
    }
    endResetModel();
}

// =================== DetailPanel ===================

DetailPanel::DetailPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    m_header = new QLabel(i18n("Nenhum processo selecionado"), this);
    m_header->setContentsMargins(6, 4, 6, 4);
    QFont hf = m_header->font();
    hf.setBold(true);
    m_header->setFont(hf);
    outer->addWidget(m_header);

    m_tabs = new QTabWidget(this);
    outer->addWidget(m_tabs, 1);

    // ---- aba Descritores ----
    {
        auto *page = new QWidget(m_tabs);
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);

        auto *bar = new QToolBar(page);
        auto *actRefresh = bar->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                          i18n("Atualizar"));
        connect(actRefresh, &QAction::triggered, this, &DetailPanel::refresh);
        bar->addSeparator();
        auto *actClose = bar->addAction(QIcon::fromTheme(QStringLiteral("dialog-close")),
                                        i18n("Fechar no dono"));
        actClose->setToolTip(i18n("Injeta close() no processo alvo (libera a trava)."));
        connect(actClose, &QAction::triggered, this, &DetailPanel::onCloseFd);
        auto *actDup = bar->addAction(QIcon::fromTheme(QStringLiteral("edit-copy")),
                                      i18n("Salvar cópia..."));
        actDup->setToolTip(i18n("Duplica o fd via pidfd_getfd e salva o conteúdo do arquivo (recupera até arquivo apagado que o processo ainda mantém aberto)."));
        connect(actDup, &QAction::triggered, this, &DetailPanel::onDupFd);
        v->addWidget(bar);

        m_fdView = new QTableView(page);
        m_fdModel = new FdTableModel(this);
        m_fdView->setModel(m_fdModel);
        m_fdView->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_fdView->setSelectionMode(QAbstractItemView::SingleSelection);
        m_fdView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_fdView->verticalHeader()->setVisible(false);
        m_fdView->horizontalHeader()->setSectionResizeMode(FdTableModel::ColTarget,
                                                           QHeaderView::Stretch);
        v->addWidget(m_fdView, 1);
        m_tabs->addTab(page, i18n("Descritores"));
    }

    // ---- aba Bibliotecas / Mapas ----
    {
        auto *page = new QWidget(m_tabs);
        auto *v = new QVBoxLayout(page);
        v->setContentsMargins(0, 0, 0, 0);

        auto *bar = new QToolBar(page);
        auto *actRefresh = bar->addAction(QIcon::fromTheme(QStringLiteral("view-refresh")),
                                          i18n("Atualizar"));
        connect(actRefresh, &QAction::triggered, this, &DetailPanel::refresh);
        bar->addSeparator();
        m_onlyLibs = new QCheckBox(i18n("Só bibliotecas (.so)"), page);
        m_onlyLibs->setChecked(true);
        connect(m_onlyLibs, &QCheckBox::toggled, this, [this]() {
            m_mapsModel->setRows(m_lastMaps, m_onlyLibs->isChecked());
        });
        bar->addWidget(m_onlyLibs);
        v->addWidget(bar);

        m_mapsView = new QTableView(page);
        m_mapsModel = new MapsTableModel(this);
        m_mapsView->setModel(m_mapsModel);
        m_mapsView->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_mapsView->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_mapsView->verticalHeader()->setVisible(false);
        m_mapsView->horizontalHeader()->setSectionResizeMode(MapsTableModel::ColPath,
                                                            QHeaderView::Stretch);
        v->addWidget(m_mapsView, 1);
        m_tabs->addTab(page, i18n("Bibliotecas / Mapas"));
    }
}

void DetailPanel::setProcess(int pid, quint64 starttime, const QString &name)
{
    m_pid = pid;
    m_starttime = starttime;
    m_name = name;
    refresh();
}

void DetailPanel::refresh()
{
    if (m_pid <= 0) {
        m_header->setText(i18n("Nenhum processo selecionado"));
        m_fdModel->setRows({});
        m_lastMaps.clear();
        m_mapsModel->setRows({}, false);
        return;
    }
    m_header->setText(i18n("%1 (PID %2)", m_name, m_pid));

    const QList<procfs::FdEntry> fds = procfs::listFds(m_pid);
    m_fdModel->setRows(fds);
    m_fdView->resizeColumnToContents(FdTableModel::ColFd);
    m_fdView->resizeColumnToContents(FdTableModel::ColType);

    m_lastMaps = procfs::listMaps(m_pid);
    m_mapsModel->setRows(m_lastMaps, m_onlyLibs->isChecked());
    m_mapsView->resizeColumnToContents(MapsTableModel::ColBase);
    m_mapsView->resizeColumnToContents(MapsTableModel::ColPerms);
}

void DetailPanel::onCloseFd()
{
    if (m_pid <= 0)
        return;
    const QModelIndex cur = m_fdView->currentIndex();
    if (!cur.isValid()) {
        KMessageBox::information(this, i18n("Selecione um descritor primeiro."));
        return;
    }
    const procfs::FdEntry e = m_fdModel->at(cur.row());

    const auto btn = KMessageBox::warningContinueCancel(
        this,
        i18n("Injetar close(%1) no processo %2 (%3)?\n\n"
             "Alvo: %4\n\n"
             "Isto libera a trava mantida por este fd (lockfile, socket do "
             "singleton, eventfd) sem matar o processo. A operação usa ptrace.",
             e.fd, m_pid, m_name, e.target),
        i18n("Fechar descritor alheio"),
        KStandardGuiItem::cont(), KStandardGuiItem::cancel(),
        QStringLiteral("procforge-close-fd"));
    if (btn != KMessageBox::Continue)
        return;

    const inject::Result r = inject::closeForeignFd(m_pid, e.fd);
    if (r.ok)
        KMessageBox::information(this, i18n("close(%1) executado no alvo (retorno %2).",
                                            e.fd, r.retval));
    else
        KMessageBox::error(this, r.error);
    refresh();
}

void DetailPanel::onDupFd()
{
    if (m_pid <= 0)
        return;
    const QModelIndex cur = m_fdView->currentIndex();
    if (!cur.isValid()) {
        KMessageBox::information(this, i18n("Selecione um descritor primeiro."));
        return;
    }
    const procfs::FdEntry e = m_fdModel->at(cur.row());
    const inject::Result r = inject::dupForeignFd(m_pid, e.fd);
    if (!r.ok) {
        KMessageBox::error(this, r.error);
        return;
    }
    const int local = int(r.retval);
    struct stat sb{};
    if (::fstat(local, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        ::close(local);
        KMessageBox::information(this,
            i18n("O fd %1 foi duplicado via pidfd_getfd, mas não é um arquivo comum (%2); "
                 "só arquivos podem ser salvos. O fd do dono continua aberto.", e.fd, e.type));
        return;
    }
    // Arquivo comum (inclusive apagado ou memfd): salva uma cópia do conteúdo.
    QString base = QFileInfo(e.target.section(QLatin1String(" (deleted)"), 0, 0)).fileName();
    if (base.isEmpty() || base.contains(QLatin1Char(':'))) base = QStringLiteral("fd-%1").arg(e.fd);
    const QString path = QFileDialog::getSaveFileName(
        this, i18n("Salvar cópia do arquivo aberto (%1)", KFormat().formatByteSize(double(sb.st_size))),
        QDir::homePath() + QLatin1Char('/') + base);
    if (path.isEmpty()) { ::close(local); return; }
    // Cópia numa thread: arquivo grande (log apagado de GBs, memfd) não trava a janela.
    QPointer<DetailPanel> self(this);
    QThread *t = QThread::create([local, path, self]() {
        QFile out(path);
        bool ok = out.open(QIODevice::WriteOnly | QIODevice::Truncate);
        QByteArray buf(1 << 20, Qt::Uninitialized);
        off_t off = 0;
        while (ok) {
            const ssize_t n = ::pread(local, buf.data(), buf.size(), off);
            if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
            if (n == 0) break;
            ok = out.write(buf.constData(), n) == n;
            off += n;
        }
        ::close(local);
        out.close();
        if (!ok) out.remove();
        QMetaObject::invokeMethod(qApp, [self, ok, path, off]() {
            if (!self) return;
            if (ok)
                KMessageBox::information(self, i18n("Cópia salva em %1 (%2).", path, KFormat().formatByteSize(double(off))));
            else
                KMessageBox::error(self, i18n("Falha ao salvar a cópia em %1.", path));
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}
