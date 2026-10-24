#include "xserver/dock_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.Dock";
uint32_t cfg(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
shared::DockSettings build() {
    shared::DockSettings s{};
    uint32_t pos = cfg("dock.position", 0);  s.position_ = pos > 2 ? 0u : pos;
    s.autohide_  = cfg("dock.autohide", 0) ? 1u : 0u;
    uint32_t sz  = cfg("dock.icon_size", 48);
    s.icon_size_ = (sz == 32 || sz == 48 || sz == 64) ? sz : 48u;
    return s;
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::DockSettings& s) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &s, sizeof(s));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::DockSettings& s) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::DockOpcode::SettingsChanged);
    std::memcpy(b.payload_, &s, sizeof(s)); BroadcastXBusMessage(b);
}
}  // namespace
void DockService::handleMessage(const shared::XBusMessage& msg,
                                std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DockOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetSettings:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetSettings), build());
        break;
    case O::SetSettings: {
        shared::DockSettings s{}; std::memcpy(&s, msg.payload_, sizeof(s));
        uint32_t pos = s.position_ > 2 ? 0u : s.position_;
        uint32_t sz  = (s.icon_size_ == 32 || s.icon_size_ == 48 || s.icon_size_ == 64) ? s.icon_size_ : 48u;
        ConfigService::set("dock.position", std::to_string(pos));
        ConfigService::set("dock.autohide", std::to_string(s.autohide_ ? 1u : 0u));
        ConfigService::set("dock.icon_size", std::to_string(sz));
        bcast(build());
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
