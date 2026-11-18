#pragma once
#include <QDialog>
#include <QList>
#include <QString>
#include <atomic>
#include <memory>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// "Localizar handles ou DLLs" (Process Hacker): procura um texto nos fds
// abertos e nas bibliotecas/arquivos mapeados de TODOS os processos. A
// varredura roda numa thread de fundo; a janela não trava.
class FindHandlesDialog : public QDialog {
    Q_OBJECT
public:
    explicit FindHandlesDialog(QWidget *parent = nullptr);
    ~FindHandlesDialog() override;
    void setTerm(const QString &t);

    struct Hit { int pid; quint64 starttime; QString comm; QString kind; int fd; QString target; };

Q_SIGNALS:
    void goToProcess(int pid);

private:
    void startSearch();
    void onResults(const QList<Hit> &hits, int scanned, bool truncated);

    QLineEdit    *m_term = nullptr;
    QComboBox    *m_kind = nullptr;
    QPushButton  *m_go = nullptr;
    QLabel       *m_status = nullptr;
    QTableWidget *m_tbl = nullptr;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    int           m_generation = 0;
};
