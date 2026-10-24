#include "xserver/display_arrange_service.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <cstring>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kSvc = "com.freebsdx.DisplayArrange";
std::function<std::vector<disparrange::ArrangeOutput>()> g_provider;

std::vector<disparrange::ArrangeOutput> outputs() {
    return g_provider ? g_provider() : std::vector<disparrange::ArrangeOutput>{};
}

void ack(std::shared_ptr<IpcSocket>& c, const shared::XBusMessage& req, uint32_t op) {
    if (!c) return;
    shared::XBusMessage m{};
    m.transaction_id_ = req.transaction_id_;
    std::strncpy(m.service_name_, kSvc, sizeof(m.service_name_) - 1);
    m.opcode_ = op;
    c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcastLayoutChanged() {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kSvc, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::DisplayArrangeOpcode::LayoutChanged);
    BroadcastXBusMessage(b);
}
}  // namespace

void DisplayArrangeService::init(
    std::function<std::vector<disparrange::ArrangeOutput>()> provider) noexcept {
    g_provider = std::move(provider);
}

void DisplayArrangeService::handleMessage(const shared::XBusMessage& msg,
                                          std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::DisplayArrangeOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetLayout: {
        uint32_t page = 0; std::memcpy(&page, msg.payload_, sizeof(page));
        auto pg = disparrange::buildPage(outputs(), page);
        shared::XBusMessage reply{};
        reply.transaction_id_ = msg.transaction_id_;
        std::strncpy(reply.service_name_, kSvc, sizeof(reply.service_name_) - 1);
        reply.opcode_ = static_cast<uint32_t>(O::GetLayout);
        std::memcpy(reply.payload_, &pg, sizeof(pg));
        if (client) client->sendMessage({reinterpret_cast<const uint8_t*>(&reply), sizeof(reply)});
        break;
    }
    case O::SetPrimary: {
        shared::DisplayOutputRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("displayarrange.primary", std::to_string(req.output_id_));
        LOG_INFO("DisplayArrangeService: primary→output {} (persisted; CRTC reconfig is a follow-up)",
                 req.output_id_);
        ack(client, msg, static_cast<uint32_t>(O::SetPrimary));
        broadcastLayoutChanged();
        break;
    }
    case O::SetMirror: {
        shared::DisplayMirrorRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        ConfigService::set("displayarrange.mirror", req.enable_ ? "1" : "0");
        LOG_INFO("DisplayArrangeService: mirror {}→{} {} (persisted; CRTC reconfig is a follow-up)",
                 req.source_id_, req.target_id_, req.enable_ ? "on" : "off");
        ack(client, msg, static_cast<uint32_t>(O::SetMirror));
        broadcastLayoutChanged();
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
