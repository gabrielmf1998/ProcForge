#pragma once
#include <QWidget>
#include <QHash>
#include <QPair>
#include <QStringList>
#include <functional>

class QTableWidget;
class QTimer;
class QLineEdit;
class QCheckBox;
class QComboBox;
class QLabel;

// Base das abas de sistema: o timer só roda com a aba VISÍVEL (uma aba
// escondida não gasta nada) e os comandos externos rodam assíncronos, sem
// travar a janela esperando systemctl/ss.
class LivePanel : public QWidget {
    Q_OBJECT
public:
    explicit LivePanel(int intervalMs, QWidget *parent = nullptr);
    ~LivePanel() override;
protected:
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    virtual void refresh() = 0;
    // Roda prog/args sem bloquear; cb recebe stdout, stderr e o código de saída.
    void runAsync(const QString &prog, const QStringList &args,
                  std::function<void(const QString &, const QString &, int)> cb);
    QTimer *m_timer = nullptr;
};

// Aba Serviços: unidades do systemd (sistema ou do usuário), ordenável,
// refresh estável (não pula). Ações: iniciar/parar/reiniciar/recarregar,
// habilitar/desabilitar, mascarar/desmascarar, status, e ir para o processo.
class ServicesPanel : public LivePanel {
    Q_OBJECT
public:
    explicit ServicesPanel(QWidget *parent = nullptr);
Q_SIGNALS:
    void goToProcess(int pid);
protected:
    void refresh() override;
private:
    void runVerb(const QString &verb);
    void showStatus();
    void gotoOwner();
    void applyFilter();
    QString currentUnit() const;
    QStringList scopeArgs() const;   // {} ou {"--user"}
    QTableWidget *m_tbl = nullptr;
    QLineEdit    *m_filter = nullptr;
    QCheckBox    *m_onlyActive = nullptr;
    QComboBox    *m_scope = nullptr;
    QLabel       *m_count = nullptr;
    bool          m_busy = false;
};

// Aba Rede: conexões (ss), ordenável e estável. Ações: matar dono, fechar a
// conexão específica (ss -K via pkexec), copiar PID, ir para o processo.
// Também lista os processos com a rede cortada, com "Restabelecer".
class NetworkPanel : public LivePanel {
    Q_OBJECT
public:
    explicit NetworkPanel(QWidget *parent = nullptr);
Q_SIGNALS:
    void goToProcess(int pid);
protected:
    void refresh() override;
private:
    void refreshBlocked();
    void applyFilter();
    QTableWidget *m_tbl = nullptr;
    QLineEdit    *m_filter = nullptr;
    QCheckBox    *m_onlyListen = nullptr;
    QLabel       *m_count = nullptr;
    class QGroupBox *m_blockedBox = nullptr;
    QTableWidget *m_blockedTbl = nullptr;
    bool          m_busy = false;
};

// Aba Disco: throughput por dispositivo (/proc/diskstats), ordenável e estável.
class DiskPanel : public LivePanel {
    Q_OBJECT
public:
    explicit DiskPanel(QWidget *parent = nullptr);
protected:
    void refresh() override;
private:
    QTableWidget *m_tbl = nullptr;
    QHash<QString, QPair<quint64, quint64>> m_prev;
    qint64 m_prevMs = 0;
};
