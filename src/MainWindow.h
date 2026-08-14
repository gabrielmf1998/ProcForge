#pragma once
#include "core/ProcInfo.h"
#include <QMainWindow>
#include <QMap>

class QTreeView;
class QLineEdit;
class QLabel;
class QThread;
class ProcessModel;
class ProcessFilterProxy;
class ProcessCollector;
class DetailPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private Q_SLOTS:
    void onSnapshot(const QList<ProcInfo> &procs);
    void onCurrentChanged(const QModelIndex &current, const QModelIndex &previous);
    void showContextMenu(const QPoint &pos);
    void setInterval(int ms);
    void onProcEvent(uint what, uint pid, uint ppid);   // cn_proc em push
    void goToProcess(int pid);                          // seleciona o pid na aba Processos

private:
    bool     currentInfo(ProcInfo &out) const;   // processo selecionado (fonte)
    void     signalCurrent(int sig);
    void     killTreeCurrent();
    void     reniceCurrent();
    void     affinityCurrent();
    void     injectCurrent();
    void     cgroupLimitCurrent();
    void     cgroupReleaseCurrent();
    void     schedCurrent();
    void     prlimitCurrent();
    void     threadsCurrent();
    void     namespacesCurrent();
    void     ebpfCurrent();
    void     propertiesCurrent();
    void     copyPidCurrent();
    // barra de ferramentas / menus estilo Process Hacker
    void     runCommand();
    void     runAsDialog();   // "Executar como…" (novo processo via helper)
    void     findHandlesOrDlls();
    void     systemInformation();
    void     aboutDialog();
    void     populateUsersMenu();
    void     openOptions();

private:
    void     buildMenusAndToolbar();

    ProcessModel        *m_model = nullptr;
    ProcessFilterProxy  *m_proxy = nullptr;
    QTreeView           *m_tree = nullptr;
    class QTabWidget    *m_tabs = nullptr;
    DetailPanel         *m_detail = nullptr;
    QLineEdit           *m_search = nullptr;
    QLabel              *m_countLabel = nullptr;
    QLabel              *m_cpuLabel = nullptr;
    QLabel              *m_memLabel = nullptr;
    class QMenu         *m_usersMenu = nullptr;
    QMap<QString, int>   m_userCounts;
    bool                 m_didInitialExpand = false;

    ProcessCollector    *m_collector = nullptr;
    QThread             *m_collectorThread = nullptr;
    class QTimer        *m_eventDebounce = nullptr;
    bool                 m_subscribed = false;
};
