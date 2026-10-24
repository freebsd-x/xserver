#include "xserver/text_size_service.h"
#include "xserver/config_service.h"

#include <cstring>
#include <cstdlib>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kService = "com.freebsdx.TextSize";
constexpr const char* kKey     = "text.size";

uint32_t currentSize() {
    std::string v = ConfigService::get(kKey, "1");   // default Normal
    uint32_t n = 1;
    try { n = static_cast<uint32_t>(std::stoul(v)); } catch (...) { n = 1; }
    return n > 2 ? 1u : n;
}

void sendState(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t opcode, uint32_t size) {
    if (!client) return;
    shared::TextSizeState st{}; st.size_ = size;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = opcode;
    std::memcpy(m.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

void broadcastState(uint32_t size) {
    shared::TextSizeState st{}; st.size_ = size;
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::TextSizeOpcode::SizeChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}
}  // namespace

void TextSizeService::handleMessage(const shared::XBusMessage& msg,
                                    std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::TextSizeOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetSize:
        sendState(client, msg.transaction_id_, static_cast<uint32_t>(O::GetSize), currentSize());
        break;
    case O::SetSize: {
        shared::TextSizeRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t size = req.size_ > 2 ? 1u : req.size_;
        ConfigService::set(kKey, std::to_string(size));
        broadcastState(size);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
