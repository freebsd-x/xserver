#include "xserver/xwayland_service.h"
#include <cstring>
#include <iostream>

// Forward-declared in main.cc — broadcasts a message to every connected client.
namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

std::atomic<bool> XWaylandService::running_{false};
std::atomic<int>  XWaylandService::pid_{-1};
std::string       XWaylandService::display_;

bool XWaylandService::isRunning() noexcept { return running_.load(); }
const std::string& XWaylandService::display() noexcept { return display_; }
int XWaylandService::pid() noexcept { return pid_.load(); }

void XWaylandService::broadcastStatus() noexcept {
    using namespace shared;
    XBusMessage msg{};
    msg.magic_ = XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.XWayland",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(XWaylandOpcode::StatusChanged);
    XWaylandStatus st{};
    st.running_ = running_.load() ? 1u : 0u;
    st.pid_     = pid_.load();
    std::strncpy(st.display_, display_.c_str(), sizeof(st.display_) - 1);
    std::memcpy(msg.payload_, &st, sizeof(st));
    BroadcastXBusMessage(msg);
}

bool XWaylandService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                      const shared::XBusMessage& msg) noexcept {
    using namespace shared;
    if (std::strncmp(msg.service_name_, "com.freebsdx.XWayland",
                     sizeof(msg.service_name_)) != 0)
        return false;

    XBusMessage resp = msg;

    switch (static_cast<XWaylandOpcode>(msg.opcode_)) {

    case XWaylandOpcode::GetStatus: {
        XWaylandStatus st{};
        st.running_ = running_.load() ? 1u : 0u;
        st.pid_     = pid_.load();
        std::strncpy(st.display_, display_.c_str(), sizeof(st.display_) - 1);
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;
    }

    case XWaylandOpcode::StatusChanged: {
        // Received from the bridge after Xwayland spawns
        XWaylandStatus st{};
        std::memcpy(&st, msg.payload_, sizeof(st));
        running_ = (st.running_ != 0);
        pid_     = st.pid_;
        display_ = st.display_;
        std::cout << "[XWaylandService] Xwayland running on " << display_
                  << " (pid=" << pid_.load() << ")\n";
        // Propagate DISPLAY env for subprocesses spawned by this compositor
        if (running_ && !display_.empty())
            setenv("DISPLAY", display_.c_str(), 1);
        broadcastStatus(); // notify all xkit apps
        break;
    }

    case XWaylandOpcode::Enable:
        // Re-broadcast current state (bridge manages actual spawn)
        broadcastStatus();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;

    case XWaylandOpcode::Disable:
        running_ = false;
        pid_     = -1;
        display_.clear();
        unsetenv("DISPLAY");
        broadcastStatus();
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        break;

    default:
        break;
    }
    return true;
}

} // namespace fbx::xserver
