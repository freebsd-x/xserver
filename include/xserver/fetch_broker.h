#pragma once
#include "shared/protocol.h"
#include "xserver/unique_fd.h"
#include <memory>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.Fetch: relays an app's allowed fetch to the xfetch daemon and
// its reply (with the body fd) back to the app. Policy: fetch_policy.h.
class FetchBroker {
public:
    // Every com.freebsdx.Fetch message, from apps and from xfetch itself.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg, UniqueFd& fd) noexcept;
};

}  // namespace fbx::xserver
