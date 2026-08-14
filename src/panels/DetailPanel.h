#pragma once
#include "core/Procfs.h"
#include <QAbstractTableModel>
#include <QWidget>

class QTableView;
class QTabWidget;
class QLabel;
class QCheckBox;

// ---- Modelo da aba de descritores de arquivo ----
class FdTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum { ColFd = 0, ColType, ColTarget, ColFlags, ColumnCount };
    explicit FdTableModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex & = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return ColumnCount; }
    QVariant data(const QModelIndex &i, int role) const override;
    QVariant headerData(int s, Qt::Orientation o, int role) const override;
    void setRows(const QList<procfs::FdEntry> &r);
    procfs::FdEntry at(int row) const {
        return (row >= 0 && row < m_rows.size()) ? m_rows.at(row) : procfs::FdEntry{};
    }
private:
    QList<procfs::FdEntry> m_rows;
};

// ---- Modelo da aba de mapas/bibliotecas ----
class MapsTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum { ColBase = 0, ColPerms, ColOffset, ColSize, ColPath, ColumnCount };
    explicit MapsTableModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex & = {}) const override { return m_rows.size(); }
    int columnCount(const QModelIndex & = {}) const override { return ColumnCount; }
    QVariant data(const QModelIndex &i, int role) const override;
    QVariant headerData(int s, Qt::Orientation o, int role) const override;
    void setRows(const QList<procfs::MapEntry> &r, bool onlyLibs);
private:
    QList<procfs::MapEntry> m_rows;
};

// ---- Painel com as abas ----
class DetailPanel : public QWidget {
    Q_OBJECT
public:
    explicit DetailPanel(QWidget *parent = nullptr);

public Q_SLOTS:
    void setProcess(int pid, quint64 starttime, const QString &name);
    void refresh();

private Q_SLOTS:
    void onCloseFd();
    void onDupFd();

private:
    int          m_pid = 0;
    quint64      m_starttime = 0;
    QString      m_name;

    QTabWidget  *m_tabs = nullptr;
    QLabel      *m_header = nullptr;
    QTableView  *m_fdView = nullptr;
    QTableView  *m_mapsView = nullptr;
    FdTableModel  *m_fdModel = nullptr;
    MapsTableModel*m_mapsModel = nullptr;
    QCheckBox   *m_onlyLibs = nullptr;
    QList<procfs::MapEntry> m_lastMaps;
};
