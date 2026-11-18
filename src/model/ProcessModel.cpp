#include "ProcessModel.h"
#include "core/ProcessIcons.h"
#include "core/Procfs.h"

#include <KFormat>
#include <KLocalizedString>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QLocale>
#include <QPalette>
#include <QTimer>
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

// O que aparece na tela mudou? Evita repintar/reordenar centenas de linhas
// paradas a cada tick (a maior parte do custo ocioso da lista).
bool sameDisplay(const ProcInfo &a, const ProcInfo &b)
{
    return a.cpuPercent == b.cpuPercent && a.ioRate == b.ioRate
        && a.ioReadRate == b.ioReadRate && a.ioWriteRate == b.ioWriteRate
        && a.rssBytes == b.rssBytes && a.vsizeBytes == b.vsizeBytes
        && a.state == b.state && a.threads == b.threads && a.nice == b.nice
        && a.cpuTicks / 100 == b.cpuTicks / 100   // tempo de CPU exibido em s
        && a.ppid == b.ppid && a.name == b.name && a.user == b.user
        && a.cmdline == b.cmdline;
}

QString formatCpuTime(quint64 ticks)
{
    const double hz = double(procfs::clockTicksPerSec() > 0 ? procfs::clockTicksPerSec() : 100);
    const quint64 totalSec = quint64(double(ticks) / hz);
    const quint64 h = totalSec / 3600, m = (totalSec / 60) % 60, s = totalSec % 60;
    return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QDateTime startedAt(const ProcInfo &p)
{
    const qint64 bt = procfs::bootTimeSecs();
    const long hz = procfs::clockTicksPerSec() > 0 ? procfs::clockTicksPerSec() : 100;
    if (bt <= 0)
        return {};
    return QDateTime::fromMSecsSinceEpoch(bt * 1000 + qint64(p.starttime) * 1000 / hz);
}
} // namespace

ProcessModel::ProcessModel(QObject *parent)
    : QAbstractItemModel(parent)
{
    m_root = new ProcNode; // sentinela invisível
    m_purgeTimer = new QTimer(this);
    m_purgeTimer->setSingleShot(true);
    connect(m_purgeTimer, &QTimer::timeout, this, &ProcessModel::purgeDying);
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

QString ProcessModel::cellText(const ProcInfo &p, int column) const
{
    static const KFormat fmt;
    switch (column) {
    case ColName:    return p.name;
    case ColPid:     return QString::number(p.pid);
    case ColCpu:     return p.cpuPercent >= 0.005 ? QString::number(p.cpuPercent, 'f', 2) : QString();
    case ColIo:      return p.ioRate >= 1.0 ? fmt.formatByteSize(p.ioRate, 1) + QStringLiteral("/s") : QString();
    case ColMem:     return p.rssBytes ? fmt.formatByteSize(p.rssBytes, 1) : QString();
    case ColUser:    return p.user;
    case ColDesc:    return p.cmdline;
    case ColPpid:    return p.ppid ? QString::number(p.ppid) : QString();
    case ColThreads: return QString::number(p.threads);
    case ColState:   return ProcInfo::stateText(p.state);
    case ColNice:    return QString::number(p.nice);
    case ColVirt:    return p.vsizeBytes ? fmt.formatByteSize(p.vsizeBytes, 1) : QString();
    case ColCpuTime: return formatCpuTime(p.cpuTicks);
    case ColStarted: {
        const QDateTime t = startedAt(p);
        return t.isValid() ? QLocale().toString(t, QLocale::ShortFormat) : QString();
    }
    case ColIoRead:  return p.ioReadRate >= 1.0 ? fmt.formatByteSize(p.ioReadRate, 1) + QStringLiteral("/s") : QString();
    case ColIoWrite: return p.ioWriteRate >= 1.0 ? fmt.formatByteSize(p.ioWriteRate, 1) + QStringLiteral("/s") : QString();
    }
    return {};
}

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
    case UserRole:       return p.user;
    case DyingRole:      return n->diedMs != 0;
    default: break;
    }

    if (role == Qt::DecorationRole && index.column() == ColName)
        return proc_icons::iconFor(p);

    if (role == Qt::TextAlignmentRole) {
        switch (index.column()) {
        case ColPid: case ColCpu: case ColIo: case ColMem: case ColPpid: case ColThreads:
        case ColNice: case ColVirt: case ColCpuTime: case ColIoRead: case ColIoWrite:
            return int(Qt::AlignRight | Qt::AlignVCenter);
        default:
            return int(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }

    if (role == Qt::BackgroundRole) {
        const bool dark = qApp->palette().color(QPalette::Base).lightness() < 128;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (n->diedMs)
            return QBrush(dark ? QColor(0x6a, 0x22, 0x22) : QColor(0xf2, 0xa0, 0xa0)); // "encerrado"
        if (n->bornMs && now - n->bornMs < kFlashMs)
            return QBrush(dark ? QColor(0x2c, 0x58, 0x2c) : QColor(0xbe, 0xe8, 0xbe)); // "novo"
        const QColor c = categoryColor(p, dark);
        return c.isValid() ? QVariant(QBrush(c)) : QVariant();
    }

    if (role == SortRole) {
        switch (index.column()) {
        case ColName:    return p.name.toLower();
        case ColPid:     return p.pid;
        case ColCpu:     return p.cpuPercent;
        case ColIo:      return p.ioRate;
        case ColMem:     return static_cast<qulonglong>(p.rssBytes);
        case ColUser:    return p.user;
        case ColDesc:    return p.cmdline;
        case ColPpid:    return p.ppid;
        case ColThreads: return p.threads;
        case ColState:   return QString(QChar(p.state));
        case ColNice:    return p.nice;
        case ColVirt:    return static_cast<qulonglong>(p.vsizeBytes);
        case ColCpuTime: return static_cast<qulonglong>(p.cpuTicks);
        case ColStarted: return static_cast<qulonglong>(p.starttime);
        case ColIoRead:  return p.ioReadRate;
        case ColIoWrite: return p.ioWriteRate;
        }
    }

    if (role == Qt::DisplayRole || role == Qt::ToolTipRole) {
        if (index.column() == ColPid && role == Qt::DisplayRole)
            return p.pid;
        return cellText(p, index.column());
    }
    return {};
}

QVariant ProcessModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColName:    return i18n("Nome");
    case ColPid:     return i18n("PID");
    case ColCpu:     return i18n("CPU");
    case ColIo:      return i18n("I/O total");
    case ColMem:     return i18n("Memória (RSS)");
    case ColUser:    return i18n("Usuário");
    case ColDesc:    return i18n("Descrição");
    case ColPpid:    return i18n("PID pai");
    case ColThreads: return i18n("Threads");
    case ColState:   return i18n("Estado");
    case ColNice:    return i18n("Nice");
    case ColVirt:    return i18n("Memória virtual");
    case ColCpuTime: return i18n("Tempo de CPU");
    case ColStarted: return i18n("Iniciado em");
    case ColIoRead:  return i18n("Leitura de disco");
    case ColIoWrite: return i18n("Escrita em disco");
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

bool ProcessModel::isDying(const QModelIndex &index) const
{
    return index.isValid() && static_cast<ProcNode*>(index.internalPointer())->diedMs != 0;
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

void ProcessModel::removeNow(ProcNode *n)
{
    const QList<ProcNode*> kids = n->children; // cópia: vamos mutar
    for (ProcNode *k : kids)
        reparent(k, m_root);
    detach(n);
    if (m_byPid.value(n->info.pid) == n)
        m_byPid.remove(n->info.pid);
    if (n->diedMs)
        --m_dying;
    delete n;
}

void ProcessModel::emitRowChanged(ProcNode *n)
{
    const QModelIndex a = indexForNode(n, 0);
    const QModelIndex b = indexForNode(n, ColumnCount - 1);
    if (a.isValid() && b.isValid())
        Q_EMIT dataChanged(a, b);
}

void ProcessModel::purgeDying()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 nextDue = 0;
    QList<ProcNode*> expired;
    for (ProcNode *n : std::as_const(m_byPid)) {
        if (!n->diedMs)
            continue;
        if (now - n->diedMs >= kFlashMs)
            expired.append(n);
        else if (!nextDue || n->diedMs + kFlashMs < nextDue)
            nextDue = n->diedMs + kFlashMs;
    }
    for (ProcNode *n : expired)
        removeNow(n);
    if (nextDue)
        m_purgeTimer->start(int(qMax<qint64>(50, nextDue - now)));
}

// ---- reconciliação por diff ----

void ProcessModel::applySnapshot(const QList<ProcInfo> &procs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<int, const ProcInfo*> incoming;
    incoming.reserve(procs.size());
    for (const ProcInfo &pi : procs)
        incoming.insert(pi.pid, &pi);

    // 0) PID reutilizado (starttime diferente): é OUTRO processo, remove o
    //    antigo já. Mesmo starttime de volta enquanto piscava como encerrado
    //    (leitura do /proc falhou por um tick): é o MESMO, só "ressuscita".
    for (const ProcInfo &pi : procs) {
        ProcNode *n = m_byPid.value(pi.pid, nullptr);
        if (!n)
            continue;
        if (n->info.starttime != pi.starttime) {
            removeNow(n);
        } else if (n->diedMs) {
            n->diedMs = 0;
            --m_dying;
            emitRowChanged(n);
        }
    }

    // 1) Os que sumiram ficam vermelhos por kFlashMs e depois saem (como no
    //    Process Hacker). Na carga inicial não há o que destacar.
    bool newlyDead = false;
    for (auto it = m_byPid.constBegin(); it != m_byPid.constEnd(); ++it) {
        ProcNode *n = it.value();
        if (!n->diedMs && !incoming.contains(it.key())) {
            n->diedMs = now;
            n->info.cpuPercent = 0.0;
            n->info.ioRate = n->info.ioReadRate = n->info.ioWriteRate = 0.0;
            ++m_dying;
            newlyDead = true;
            emitRowChanged(n);
        }
    }
    if (newlyDead && !m_purgeTimer->isActive())
        m_purgeTimer->start(kFlashMs);

    // Pai desejado de um pid: seu ppid se existir vivo no snapshot; senão a raiz.
    auto desiredParent = [&](const ProcInfo &pi) -> ProcNode* {
        if (pi.ppid != 0 && pi.ppid != pi.pid && incoming.contains(pi.ppid))
            return m_byPid.value(pi.ppid, m_root);
        return m_root;
    };

    // 2) Cria os novos, inicialmente pendurados na raiz.
    for (const ProcInfo &pi : procs) {
        if (!m_byPid.contains(pi.pid)) {
            ProcNode *n = new ProcNode;
            n->info = pi;
            n->bornMs = m_firstSnapshotDone ? now : 0;
            m_byPid.insert(pi.pid, n);
            attach(n, m_root);
        }
    }

    // 3) Atualiza dados, corrige o pai e avisa a view só do que mudou.
    for (const ProcInfo &pi : procs) {
        ProcNode *n = m_byPid.value(pi.pid);
        if (!n)
            continue;
        bool changed = !sameDisplay(n->info, pi);
        // O destaque de "novo" precisa de um repaint quando acaba.
        if (n->bornMs && now - n->bornMs >= kFlashMs) {
            n->bornMs = 0;
            changed = true;
        } else if (n->bornMs) {
            changed = true;
        }
        n->info = pi;
        ProcNode *dp = desiredParent(pi);
        if (n->parent != dp && !isAncestorOrSelf(n, dp)) // guarda anti-ciclo
            reparent(n, dp);
        if (changed)
            emitRowChanged(n);
    }

    m_firstSnapshotDone = true;
}
