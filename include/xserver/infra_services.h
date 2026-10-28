#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// Network shares, bhyve VMs and podman containers (parsed by infra_parse.h).
// Commands that can block on the network (mount/umount/podman start|stop) run
// on a detached worker under timeout(1), never on the compositor thread; the
// request is acked at once and the *Changed broadcast follows when the worker
// finishes. Shell-only in the dispatcher.

// com.freebsdx.NetworkShares — read-only list: mounted nfs/smbfs + the
// /etc/fstab entries that aren't mounted, 2 per page.
class NetworkSharesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.NetworkShare — same list, one per page, plus Mount/Unmount as
// the session user. That needs vfs.usermount=1 and a mount point the user
// owns; SMB passwords aren't passed on the command line (mount_smbfs -N reads
// ~/.nsmbrc). Mount/Unmount acks carry a uint32 status: 0 = started,
// otherwise an errno (EPERM: usermount off, ENOTDIR: bad mount point,
// EINVAL: bad host/share/user, ENOTSUP: password given, ENOENT: no such share).
class NetworkShareService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.VirtualMachines — running bhyve VMs (/dev/vmm/<name>). vCPU/RAM
// aren't readable unprivileged (reported 0); Start/Stop aren't supported yet.
class VirtualMachinesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.Containers — `podman ps -a` (empty when podman isn't
// installed); Start/Stop → podman start/stop <id>.
class ContainersService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
