#pragma once
#include "ProcessModel.h"
#include <QSortFilterProxyModel>

// Ordena por valor cru (SortRole) e filtra por nome/PID/cmdline/usuário.
// Recursivo: um processo que casa mantém os pais visíveis, preservando a árvore.
//
// Sintaxe da busca:
//   texto      nome, linha de comando ou usuário contém o texto; ou PID exato
//   user:nome  só processos daquele usuário (usado pelo menu Usuários)
//   pid:N      PID exato (e a descendência fica visível pelos pais)
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        beginFilterChange();
#endif
        m_text = t.trimmed();
        m_user.clear();
        m_pid = 0;
        if (m_text.startsWith(QLatin1String("user:"), Qt::CaseInsensitive)) {
            m_user = m_text.mid(5).trimmed();
            m_text.clear();
        } else if (m_text.startsWith(QLatin1String("pid:"), Qt::CaseInsensitive)) {
            m_pid = m_text.mid(4).trimmed().toInt();
            m_text.clear();
        }
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        invalidateRowsFilter();
#endif
    }

    bool isFiltering() const { return !m_text.isEmpty() || !m_user.isEmpty() || m_pid > 0; }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (!isFiltering())
            return true;
        const QModelIndex idx = sourceModel()->index(row, 0, parent);
        if (!idx.isValid())
            return false;
        const auto *m = static_cast<const ProcessModel*>(sourceModel());
        if (!m_user.isEmpty())
            return m->data(idx, ProcessModel::UserRole).toString() == m_user;
        const int pid = m->data(idx, ProcessModel::PidRole).toInt();
        if (m_pid > 0)
            return pid == m_pid;
        const QString name = m->data(idx, Qt::DisplayRole).toString();
        const QModelIndex cmdIdx =
            sourceModel()->index(row, ProcessModel::ColDesc, parent);
        const QString cmd = m->data(cmdIdx, Qt::DisplayRole).toString();
        const QString user = m->data(idx, ProcessModel::UserRole).toString();
        return name.contains(m_text, Qt::CaseInsensitive)
            || cmd.contains(m_text, Qt::CaseInsensitive)
            || user.compare(m_text, Qt::CaseInsensitive) == 0
            || QString::number(pid) == m_text;
    }

private:
    QString m_text;
    QString m_user;
    int     m_pid = 0;
};
