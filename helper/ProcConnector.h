#pragma once
#include <QObject>

class QSocketNotifier;

// Conector de processos do kernel (netlink NETLINK_CONNECTOR / CN_IDX_PROC).
// Empurra eventos fork/exec/exit em tempo real (precisa CAP_NET_ADMIN). Só nível
// de processo (tgid); eventos de thread são filtrados.
class ProcConnector : public QObject {
    Q_OBJECT
public:
    enum What { Fork = 1, Exec = 2, Exit = 3 };

    explicit ProcConnector(QObject *parent = nullptr);
    ~ProcConnector() override;

    bool start();          // abre o socket e assina a multicast
    void stop();
    bool active() const { return m_fd >= 0; }

Q_SIGNALS:
    void procEvent(uint what, uint pid, uint ppid);

private Q_SLOTS:
    void onReadable();

private:
    int             m_fd = -1;
    QSocketNotifier *m_notifier = nullptr;
};
