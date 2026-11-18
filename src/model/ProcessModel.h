#pragma once
#include "core/ProcInfo.h"
#include <QAbstractItemModel>
#include <QHash>

class QTimer;

// Nó da árvore de processos. Estável entre ticks: preserva índices persistentes,
// seleção e expansão da view. Reconciliação por diff (add/remove/reparent).
struct ProcNode {
    ProcInfo         info;
    ProcNode        *parent = nullptr;
    QList<ProcNode*> children;
    qint64           bornMs = 0;   // quando surgiu (flash verde estilo Process Hacker)
    qint64           diedMs = 0;   // quando saiu (flash vermelho antes de sumir)
    int rowInParent() const {
        return parent ? parent->children.indexOf(const_cast<ProcNode*>(this)) : 0;
    }
};

class ProcessModel : public QAbstractItemModel {
    Q_OBJECT
public:
    // Colunas na ordem do Process Hacker. As 7 primeiras aparecem por padrão;
    // as demais são opcionais (botão direito no cabeçalho para escolher).
    enum Column {
        ColName = 0, ColPid, ColCpu, ColIo, ColMem, ColUser, ColDesc,
        ColPpid, ColThreads, ColState, ColNice, ColVirt, ColCpuTime, ColStarted,
        ColIoRead, ColIoWrite,
        ColumnCount
    };
    static constexpr int kDefaultColumns = ColDesc + 1;
    enum Roles {
        SortRole = Qt::UserRole + 1,
        PidRole, PpidRole, StateCharRole, StarttimeRole, CpuRole, UserRole, DyingRole
    };
    // Duração dos destaques de "novo" e "encerrado".
    static constexpr int kFlashMs = 1500;

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
    bool        isDying(const QModelIndex &index) const;
    int         liveCount() const { return m_byPid.size() - m_dying; }

    // Texto de uma célula como aparece na tela (para copiar/exportar).
    QString     cellText(const ProcInfo &p, int column) const;

public Q_SLOTS:
    void applySnapshot(const QList<ProcInfo> &procs);

private:
    QModelIndex indexForNode(ProcNode *n, int column = 0) const;
    void detach(ProcNode *n);
    void attach(ProcNode *n, ProcNode *parent);
    void reparent(ProcNode *n, ProcNode *newParent);
    bool isAncestorOrSelf(ProcNode *maybeAncestor, ProcNode *node) const;
    void removeNow(ProcNode *n);        // tira da árvore e apaga (filhos vão p/ raiz)
    void purgeDying();                  // remove os encerrados cujo flash acabou
    void emitRowChanged(ProcNode *n);

    ProcNode                 *m_root = nullptr;
    QHash<int, ProcNode*>      m_byPid;
    int                        m_dying = 0;
    QTimer                    *m_purgeTimer = nullptr;
    bool                       m_firstSnapshotDone = false; // não "flasha" a carga inicial
};
