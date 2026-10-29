#include "xserver/update_channel_service.h"
#include "xserver/config_service.h"
#include "xserver/async_shell.h"
#include "xserver/sysadmin_parse.h"
#include <cstring>
#include <cstdlib>
#include <string>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.UpdateChannel";
shared::UpdateChannelState build() {
    shared::UpdateChannelState st{};
    std::string v = ConfigService::get("update.channel", "0");
    uint32_t ch = 0;
    try { ch = static_cast<uint32_t>(std::stoul(v)); } catch (...) { ch = 0; }
    st.channel_       = ch > 1 ? 0u : ch;
    // Outdated packages vs. the cached repo catalogue (-U: no network fetch).
    // Off the event loop: the last result now, a refresh in the background.
    auto pending = sysadmin::parsePkgOutdated(g_asyncShell.get("pkg version -vRl '<' -U 2>/dev/null", 20));
    st.pending_count_ = pending.count;
    std::strncpy(st.pending_ver_, pending.first.c_str(), sizeof(st.pending_ver_) - 1);
    std::strncpy(st.channel_name_, st.channel_ ? "Beta" : "Stable", sizeof(st.channel_name_) - 1);
    return st;
}
void send(std::shared_ptr<IpcSocket>& c, uint32_t tid, uint32_t op, const shared::UpdateChannelState& st) {
    if (!c) return;
    shared::XBusMessage m{}; m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = op; std::memcpy(m.payload_, &st, sizeof(st));
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}
void bcast(const shared::UpdateChannelState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::UpdateChannelOpcode::ChannelChanged);
    std::memcpy(b.payload_, &st, sizeof(st)); BroadcastXBusMessage(b);
}
}  // namespace
void UpdateChannelService::handleMessage(const shared::XBusMessage& msg,
                                         std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::UpdateChannelOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetChannel:
        send(client, msg.transaction_id_, static_cast<uint32_t>(O::GetChannel), build());
        break;
    case O::SetChannel: {
        shared::UpdateChannelRequest req{}; std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("update.channel", std::to_string(req.channel_ > 1 ? 0u : req.channel_));
        bcast(build());
        break;
    }
    default: break;
    }
}
}  // namespace fbx::xserver
