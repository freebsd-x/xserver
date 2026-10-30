#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>
#include <string>

namespace fbx::xserver {

// com.freebsdx.AdBlock — a DNS sinkhole in /etc/hosts (adblock.h). UpdateList
// downloads a hosts-format blocklist as the user and stores it normalized in
// ~/.cache/freebsd-x/adblock/domains; SetEnabled asks the root broker
// (adblock.apply) to write or remove AdBlock's block of /etc/hosts. GetStatus
// reports what /etc/hosts actually holds. SetEnabled/UpdateList have no direct
// reply: StatusChanged is broadcast when they finish (updating_ while a
// download runs). Shell-only; handled on a worker thread (downloads are slow).
class AdBlockService {
public:
    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // What the service touches outside itself; tests swap these out.
    struct Backend {
        std::function<bool(const std::string& url, const std::string& dest)> download;  // → file
        std::function<int(bool on)> apply;     // the broker's adblock.apply (0 = ok, -errno)
        std::string hosts_path = "/etc/hosts";
    };
    static void setBackend(Backend b) noexcept;   // empty members → the real ones
};

}  // namespace fbx::xserver
