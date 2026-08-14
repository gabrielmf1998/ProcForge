#pragma once
#include <QDialog>
#include <QHash>

class QTableWidget;
class QLabel;

// Janela de propriedades do processo (como o duplo-clique do Process Hacker):
// abas Geral, Estatísticas, Threads, Ambiente, Handles (fds), Memória e Token.
class ProcessProperties : public QDialog {
    Q_OBJECT
public:
    ProcessProperties(int pid, quint64 starttime, const QString &name, QWidget *parent = nullptr);

private Q_SLOTS:
    void refreshLive();          // estatísticas + threads ao vivo

private:
    QWidget *buildGeneral();
    QWidget *buildStatistics();
    QWidget *buildThreads();
    QWidget *buildEnvironment();
    QWidget *buildHandles();
    QWidget *buildMemory();
    QWidget *buildToken();
    void     reloadHandles();
    void     reloadMemory();

    int      m_pid;
    quint64  m_starttime;
    QString  m_name;

    QTableWidget *m_stats = nullptr;
    QTableWidget *m_threads = nullptr;
    QTableWidget *m_handles = nullptr;
    QTableWidget *m_maps = nullptr;
    QLabel       *m_header = nullptr;
};
