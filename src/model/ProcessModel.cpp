#include "ProcessModel.h"
#include "core/ProcessIcons.h"

#include <KFormat>
#include <KLocalizedString>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QPalette>
#include <unistd.h>

namespace {
// Cores de fundo por categoria, no espírito do Process Hacker (claro/escuro).
QColor categoryColor(const ProcInfo &p, bool dark)
{
    static const quint32 myUid = static_cast<quint32>(::getuid());
    if (p.state == 'T' || p.state == 't')                 // suspenso
        return dark ? QColor(0x4a, 0x4a, 0x4a) : QColor(0xd9, 0xd9, 0xd9);
    if (p.kernel)                                         // kernel thread
        return dark ? QColor(0x3a, 0x3a, 0x2a) : QColor(0xf0, 0xf0, 0xd6);
    if (p.service)                                        // processo de serviço
        return dark ? QColor(0x22, 0x3a, 0x22) : QColor(0xd6, 0xf2, 0xd6);
    if (p.uid == 0)                                       // root
        return dark ? QColor(0x46, 0x2c, 0x2c) : QColor(0xfb, 0xdc, 0xdc);
    if (p.uid == myUid)                                   // seus processos
        return dark ? QColor(0x20, 0x33, 0x42) : QColor(0xd8, 0xec, 0xf8);
    return {};                                            // outros usuários: padrão
}
} // namespace

ProcessModel::ProcessModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_root = new ProcNode; // sentinela invisível
}

ProcessModel::~ProcessModel()
{
    // apaga a árvore inteira
    QList<ProcNode*> stack{m_root};
    while (!stack.isEmpty()) {
        ProcNode *n = stack.takeLast();
        stack.append(n->children);
        if (n != m_root) delete n;
    }
    delete m_root;
}

QModelIndex ProcessModel::indexForNode(ProcNode *n, int column) const
{
    if (!n || n == m_root)
        return {};
    return createIndex(n->rowInParent(), column, n);
}

QModelIndex ProcessModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};
    ProcNode *p = parent.isValid() ? static_cast<ProcNode*>(parent.internalPointer()) : m_root;
    if (row < 0 || row >= p->children.size())
        return {};
    return createIndex(row, column, p->children.at(row));
}

QModelIndex ProcessModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};
    ProcNode *n = static_cast<ProcNode*>(child.internalPointer());
    ProcNode *p = n ? n->parent : nullptr;
    if (!p || p == m_root)
        return {};
    return createIndex(p->rowInParent(), 0, p);
}

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    ProcNode *p = parent.isValid() ? static_cast<ProcNode*>(parent.internalPointer()) : m_root;
    return p->children.size();
}

int ProcessModel::columnCount(const QModelIndex &) const { return ColumnCount; }

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};
    const ProcNode *n = static_cast<ProcNode*>(index.internalPointer());
    const ProcInfo &p = n->info;

    switch (role) {
    case PidRole:        return p.pid;
    case PpidRole:       return p.ppid;
    case StateCharRole:  return QChar(p.state);
    case StarttimeRole:  return static_cast<qulonglong>(p.starttime);
    case CpuRole:        return p.cpuPercent;
    default: break;
    }

    if (role == Qt::DecorationRole && index.column() == ColName)
        return proc_icons::iconFor(p);

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case ColPid: case ColCpu: case ColIo: case ColMem:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    if (role == Qt::BackgroundRole) {
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        if (n->bornMs && QDateTime::currentMSecsSinceEpoch() - n->bornMs < 1200)
            return QBrush(dark ? QColor(0x2c, 0x58, 0x2c) : QColor(0xbe, 0xe8, 0xbe)); // flash "novo"
        const QColor c = categoryColor(p, dark);
        return c.isValid() ? QVariant(QBrush(c)) : QVariant();
    }

    if (role == SortRole) {
        switch (index.column()) {
        case ColName: return p.name.toLower();
        case ColPid:  return p.pid;
        case ColCpu:  return p.cpuPercent;
        case ColIo:   return p.ioRate;
        case ColMem:  return static_cast<qulonglong>(p.rssBytes);
        case ColUser: return p.user;
        case ColDesc: return p.cmdline;
        }
    }

    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        static const KFormat fmt;
        switch (index.column()) {
        case ColName: return p.name;
        case ColPid:  return p.pid;
        case ColCpu:  return p.cpuPercent >= 0.005 ? QString::number(p.cpuPercent, 'f', 2) : QString();
        case ColIo:   return p.ioRate >= 1.0
                                 ? fmt.formatByteSize(p.ioRate, 1) + QStringLiteral("/s") : QString();
        case ColMem:  return p.rssBytes ? fmt.formatByteSize(p.rssBytes, 1) : QString();
        case ColUser: return p.user;
        case ColDesc: return p.cmdline;
        }
    }
    return {};
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColName: return i18n("Nome");
    case ColPid:  return i18n("PID");
    case ColCpu:  return i18n("CPU");
    case ColIo:   return i18n("I/O total");
    case ColMem:  return i18n("Bytes privados");
    case ColUser: return i18n("Usuário");
    case ColDesc: return i18n("Descrição");
    }
    return {};
}

ProcInfo ProcessModel::infoForIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};
    return static_cast<ProcNode*>(index.internalPointer())->info;
}

int ProcessModel::pidForIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return 0;
    return static_cast<ProcNode*>(index.internalPointer())->info.pid;
}

QModelIndex ProcessModel::indexForPid(int pid) const
{
    ProcNode *n = m_byPid.value(pid, nullptr);
    return n ? indexForNode(n, 0) : QModelIndex();
}

// ---- mutações de árvore (com begin/end corretos) ----

void ProcessModel::detach(ProcNode *n)
{
    ProcNode *p = n->parent;
    if (!p)
        return;
    const int r = p->children.indexOf(n);
    beginRemoveRows(indexForNode(p), r, r);
    p->children.removeAt(r);
    n->parent = nullptr;
    endRemoveRows();
}

void ProcessModel::attach(ProcNode *n, ProcNode *parent)
{
    const int r = parent->children.size();
    beginInsertRows(indexForNode(parent), r, r);
    parent->children.append(n);
    n->parent = parent;
    endInsertRows();
}

void ProcessModel::reparent(ProcNode *n, ProcNode *newParent)
{
    if (n->parent == newParent)
        return;
    detach(n);
    attach(n, newParent);
}

bool ProcessModel::isAncestorOrSelf(ProcNode *maybeAncestor, ProcNode *node) const
{
    for (ProcNode *w = node; w; w = w->parent)
        if (w == maybeAncestor)
            return true;
    return false;
}

// ---- reconciliação por diff ----

void ProcessModel::applySnapshot(const QList<ProcInfo> &procs)
{
    QHash<int, const ProcInfo*> incoming;
    incoming.reserve(procs.size());
    for (const ProcInfo &pi : procs)
        incoming.insert(pi.pid, &pi);

    // Pai desejado de um pid: seu ppid se existir no snapshot; senão a raiz.
    auto desiredParent = [&](const ProcInfo &pi) -> ProcNode* {
        if (pi.ppid != 0 && pi.ppid != pi.pid && incoming.contains(pi.ppid))
            return m_byPid.value(pi.ppid, m_root);
        return m_root;
    };

    // 1) Coleta os que sumiram.
    QList<ProcNode*> gone;
    for (auto it = m_byPid.constBegin(); it != m_byPid.constEnd(); ++it)
        if (!incoming.contains(it.key()))
            gone.append(it.value());

    // 1a) Move filhos dos sumidos para a raiz (assim todo sumido vira folha).
    for (ProcNode *g : gone) {
        const QList<ProcNode*> kids = g->children; // cópia: vamos mutar
        for (ProcNode *k : kids)
            reparent(k, m_root);
    }
    // 1b) Remove os sumidos (agora folhas sob a raiz ou sob sobreviventes).
    for (ProcNode *g : gone) {
        detach(g);
        m_byPid.remove(g->info.pid);
        delete g;
    }

    // 2) Cria os novos, inicialmente pendurados na raiz.
    for (const ProcInfo &pi : procs) {
        if (!m_byPid.contains(pi.pid)) {
            ProcNode *n = new ProcNode;
            n->info = pi;
            n->bornMs = m_firstSnapshotDone ? QDateTime::currentMSecsSinceEpoch() : 0;
            m_byPid.insert(pi.pid, n);
            attach(n, m_root);
        }
    }

    // 3) Atualiza dados de todos e corrige o pai.
    for (const ProcInfo &pi : procs) {
        ProcNode *n = m_byPid.value(pi.pid);
        if (!n)
            continue;
        n->info = pi;
        ProcNode *dp = desiredParent(pi);
        if (n->parent != dp && !isAncestorOrSelf(n, dp)) // guarda anti-ciclo
            reparent(n, dp);
    }

    // 4) Sinaliza mudança de dados nas linhas visíveis.
    for (const ProcInfo &pi : procs) {
        ProcNode *n = m_byPid.value(pi.pid);
        if (!n)
            continue;
        const QModelIndex a = indexForNode(n, 0);
        const QModelIndex b = indexForNode(n, ColumnCount - 1);
        if (a.isValid() && b.isValid())
            Q_EMIT dataChanged(a, b);
    }

    m_firstSnapshotDone = true;
}
