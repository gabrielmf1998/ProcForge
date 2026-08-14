#include "ProcConnector.h"

#include <QSocketNotifier>

#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/connector.h>
#include <linux/cn_proc.h>

ProcConnector::ProcConnector(QObject *parent) : QObject(parent) {}

ProcConnector::~ProcConnector() { stop(); }

bool ProcConnector::start()
{
    if (m_fd >= 0)
        return true;

    m_fd = ::socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_CONNECTOR);
    if (m_fd < 0)
        return false;

    struct sockaddr_nl addr;
    std::memset(&addr, 0, sizeof addr);
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    addr.nl_pid    = static_cast<__u32>(::getpid());
    if (::bind(m_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof addr) < 0) {
        ::close(m_fd); m_fd = -1;
        return false;
    }

    // Mensagem PROC_CN_MCAST_LISTEN para começar a receber eventos.
    // (cn_msg tem membro flexível; montamos num buffer em vez de struct aninhada.)
    char nlbuf[NLMSG_SPACE(sizeof(struct cn_msg) + sizeof(enum proc_cn_mcast_op))];
    std::memset(nlbuf, 0, sizeof nlbuf);
    struct nlmsghdr *nlh = reinterpret_cast<struct nlmsghdr *>(nlbuf);
    nlh->nlmsg_len  = sizeof nlbuf;
    nlh->nlmsg_type = NLMSG_DONE;
    nlh->nlmsg_pid  = static_cast<__u32>(::getpid());
    struct cn_msg *cn = static_cast<struct cn_msg *>(NLMSG_DATA(nlh));
    cn->id.idx = CN_IDX_PROC;
    cn->id.val = CN_VAL_PROC;
    cn->len    = sizeof(enum proc_cn_mcast_op);
    *reinterpret_cast<enum proc_cn_mcast_op *>(cn->data) = PROC_CN_MCAST_LISTEN;
    if (::send(m_fd, nlh, nlh->nlmsg_len, 0) != static_cast<ssize_t>(nlh->nlmsg_len)) {
        ::close(m_fd); m_fd = -1;
        return false;
    }

    m_notifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &ProcConnector::onReadable);
    return true;
}

void ProcConnector::stop()
{
    if (m_notifier) { m_notifier->setEnabled(false); m_notifier->deleteLater(); m_notifier = nullptr; }
    if (m_fd >= 0)  { ::close(m_fd); m_fd = -1; }
}

void ProcConnector::onReadable()
{
    char buf[8192];
    const ssize_t got = ::recv(m_fd, buf, sizeof buf, 0);
    if (got <= 0)
        return;
    int len = static_cast<int>(got);

    for (struct nlmsghdr *nlh = reinterpret_cast<struct nlmsghdr *>(buf);
         NLMSG_OK(nlh, len);
         nlh = NLMSG_NEXT(nlh, len)) {
        if (nlh->nlmsg_type == NLMSG_NOOP)
            continue;
        if (nlh->nlmsg_type == NLMSG_ERROR || nlh->nlmsg_type == NLMSG_OVERRUN)
            break;

        const struct cn_msg *cn = static_cast<const struct cn_msg *>(NLMSG_DATA(nlh));
        if (cn->id.idx != CN_IDX_PROC || cn->id.val != CN_VAL_PROC)
            continue;
        const struct proc_event *ev = reinterpret_cast<const struct proc_event *>(cn->data);

        switch (ev->what) {
        case PROC_EVENT_FORK:
            if (ev->event_data.fork.child_pid == ev->event_data.fork.child_tgid)
                Q_EMIT procEvent(Fork, static_cast<uint>(ev->event_data.fork.child_tgid),
                                 static_cast<uint>(ev->event_data.fork.parent_tgid));
            break;
        case PROC_EVENT_EXEC:
            Q_EMIT procEvent(Exec, static_cast<uint>(ev->event_data.exec.process_tgid), 0);
            break;
        case PROC_EVENT_EXIT:
            if (ev->event_data.exit.process_pid == ev->event_data.exit.process_tgid)
                Q_EMIT procEvent(Exit, static_cast<uint>(ev->event_data.exit.process_tgid), 0);
            break;
        default:
            break;
        }
    }
}
