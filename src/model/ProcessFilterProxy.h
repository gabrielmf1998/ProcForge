#pragma once
#include "ProcessModel.h"
#include <QSortFilterProxyModel>

// Ordena por valor cru (SortRole) e filtra por nome/PID/cmdline. Recursivo:
// um processo que casa mantém os pais visíveis, preservando a árvore.
class ProcessFilterProxy : public QSortFilterProxyModel {
    Q_OBJECT
public:
    explicit ProcessFilterProxy(QObject *parent = nullptr)
        : QSortFilterProxyModel(parent)
    {
        setSortRole(ProcessModel::SortRole);
        setRecursiveFilteringEnabled(true);
        setDynamicSortFilter(true);
        setSortCaseSensitivity(Qt::CaseInsensitive);
    }

    void setFilterText(const QString &t)
    {
        m_text = t.trimmed();
        beginFilterChange();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (m_text.isEmpty())
            return true;
        const QModelIndex idx = sourceModel()->index(row, 0, parent);
        if (!idx.isValid())
            return false;
        const auto *m = static_cast<const ProcessModel*>(sourceModel());
        const QString name = m->data(idx, Qt::DisplayRole).toString();
        const QString pid  = m->data(idx, ProcessModel::PidRole).toString();
        const QModelIndex cmdIdx =
            sourceModel()->index(row, ProcessModel::ColDesc, parent);
        const QString cmd = m->data(cmdIdx, Qt::DisplayRole).toString();
        return name.contains(m_text, Qt::CaseInsensitive)
            || cmd.contains(m_text, Qt::CaseInsensitive)
            || pid == m_text;
    }

private:
    QString m_text;
};
