#include "MemoryIO.h"
#include <sys/uio.h>

namespace mem {

ssize_t readv(int pid, uint64_t addr, void *buf, size_t len)
{
    struct iovec local  { buf, len };
    struct iovec remote { reinterpret_cast<void *>(addr), len };
    return ::process_vm_readv(pid, &local, 1, &remote, 1, 0);
}

ssize_t writev(int pid, uint64_t addr, const void *buf, size_t len)
{
    struct iovec local  { const_cast<void *>(buf), len };
    struct iovec remote { reinterpret_cast<void *>(addr), len };
    return ::process_vm_writev(pid, &local, 1, &remote, 1, 0);
}

} // namespace mem
