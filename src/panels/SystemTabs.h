#pragma once
#include <QWidget>
#include <QHash>
#include <QPair>

class QTableWidget;

// Aba Serviços: unidades do systemd, ordenável, refresh estável (não pula).
// Ações: iniciar/parar/reiniciar/recarregar, habilitar/desabilitar, mascarar/
// desmascarar, status, e ir para o processo (MainPID).
class ServicesPanel : public QWidget {
    Q_OBJECT
public:
    explicit ServicesPanel(QWidget *parent = nullptr);
Q_SIGNALS:
    void goToProcess(int pid);
private Q_SLOTS:
    void refresh();
private:
    void runVerb(const QString &verb);   // systemctl <verb> <unit> (polkit próprio)
    void showStatus();
    void gotoOwner();
    QString currentUnit() const;
    QTableWidget *m_tbl = nullptr;
};

// Aba Rede: conexões (ss), ordenável e estável. Ações: matar dono, fechar a
// conexão específica (ss -K via pkexec), copiar PID, ir para o processo.
// Também lista os processos com a rede cortada (Cortar rede.../menu de
// contexto da lista de processos), com ícone/nome/caminho e "Restabelecer".
class NetworkPanel : public QWidget {
    Q_OBJECT
public:
    explicit NetworkPanel(QWidget *parent = nullptr);
Q_SIGNALS:
    void goToProcess(int pid);
private Q_SLOTS:
    void refresh();
    void refreshBlocked();
private:
    QTableWidget *m_tbl = nullptr;
    class QGroupBox *m_blockedBox = nullptr;
    QTableWidget *m_blockedTbl = nullptr;
};

// Aba Disco: throughput por dispositivo (/proc/diskstats), ordenável e estável.
class DiskPanel : public QWidget {
    Q_OBJECT
public:
    explicit DiskPanel(QWidget *parent = nullptr);
private Q_SLOTS:
    void refresh();
private:
    QTableWidget *m_tbl = nullptr;
    QHash<QString, QPair<quint64, quint64>> m_prev;
    qint64 m_prevMs = 0;
};
