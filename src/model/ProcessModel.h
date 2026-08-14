#pragma once
#include "core/ProcInfo.h"
#include <QAbstractItemModel>
#include <QHash>

// Nó da árvore de processos. Estável entre ticks: preserva índices persistentes,
// seleção e expansão da view. Reconciliação por diff (add/remove/reparent).
struct ProcNode {
    ProcInfo         info;
    ProcNode        *parent = nullptr;
    QList<ProcNode*> children;
    qint64           bornMs = 0;   // quando surgiu (flash verde estilo Process Hacker)
    int rowInParent() const {
        return parent ? parent->children.indexOf(const_cast<ProcNode*>(this)) : 0;
    }
};

class ProcessModel : public QAbstractItemModel {
    Q_OBJECT
public:
    // Colunas na ordem do Process Hacker.
    enum Column {
        ColName = 0, ColPid, ColCpu, ColIo, ColMem, ColUser, ColDesc,
        ColumnCount
    };
    enum Roles {
        SortRole = Qt::UserRole + 1,
        PidRole, PpidRole, StateCharRole, StarttimeRole, CpuRole
    };

    explicit ProcessModel(QObject *parent = nullptr);
    ~ProcessModel() override;

    // QAbstractItemModel
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    // Helpers de acesso
    ProcInfo    infoForIndex(const QModelIndex &index) const;
    int         pidForIndex(const QModelIndex &index) const;
    QModelIndex indexForPid(int pid) const;

public Q_SLOTS:
    void applySnapshot(const QList<ProcInfo> &procs);

private:
    QModelIndex indexForNode(ProcNode *n, int column = 0) const;
    void detach(ProcNode *n);
    void attach(ProcNode *n, ProcNode *parent);
    void reparent(ProcNode *n, ProcNode *newParent);
    bool isAncestorOrSelf(ProcNode *maybeAncestor, ProcNode *node) const;

    ProcNode                 *m_root = nullptr;
    QHash<int, ProcNode*>      m_byPid;
    bool                       m_firstSnapshotDone = false; // não "flasha" a carga inicial
};
