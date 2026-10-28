#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>

namespace fbx::xserver {

// FreeBSD system-admin services. Reads run base-system tools unprivileged
// (parsed by sysadmin_parse.h); mutations go through the xsessiond privilege
// broker (priv_ops.h verbs) and are logged as no-ops when it's unreachable.
// Every request gets a framed reply echoing transaction_id_ (mutations reply
// with an empty ack so the panel's refresh callback fires); mutations also
// broadcast the service's *Changed opcode.

// com.freebsdx.Services — enabled rc(8) daemons (scripts with a pidfile=),
// running state from the process table. Start/Stop → service.start/stop.
class RcServicesService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.Jails — jls(8). StopJail → jail.remove (jail -r <jid>).
class JailsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.ZfsPools — zpool list/status. StartScrub → zfs.scrub.
class ZfsPoolsService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.BootEnv — bectl list. ActivateBE/DeleteBE → be.activate/destroy
// (the booted BE is refused before reaching the broker).
class BootEnvService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.NTP — ntpd running state + ntpq -pn peer/sync/offset.
// SetEnabled → service.start/stop ntpd (runtime only; rc.conf untouched).
class NtpService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

// com.freebsdx.RemoteLogin — sshd running state, port/listen address from
// sshd_config, inbound session count from netstat. SetEnabled →
// service.start/stop sshd (runtime only; rc.conf untouched).
class RemoteLoginService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;
};

}  // namespace fbx::xserver
