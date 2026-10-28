#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <memory>
#include <vector>

// Framing helpers shared by XBus service handlers: framed replies that echo
// transaction_id_, empty acks, broadcasts, and 3-per-page list pages.
namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace svc {

template <typename T>
void reply(const shared::XBusMessage& msg, const std::shared_ptr<IpcSocket>& client,
           const char* service, uint32_t opcode, const T* payload) {
    shared::XBusMessage r{};
    r.transaction_id_ = msg.transaction_id_;
    std::strncpy(r.service_name_, service, sizeof(r.service_name_) - 1);
    r.opcode_ = opcode;
    if (payload) {
        static_assert(sizeof(T) <= sizeof(r.payload_));
        std::memcpy(r.payload_, payload, sizeof(T));
    }
    client->sendMessage({reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
}

inline void ack(const shared::XBusMessage& msg, const std::shared_ptr<IpcSocket>& client,
                const char* service) {
    reply<char>(msg, client, service, msg.opcode_, nullptr);
}

template <typename T>
void broadcast(const char* service, uint32_t opcode, const T* payload) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, service, sizeof(b.service_name_) - 1);
    b.opcode_ = opcode;
    if (payload) {
        static_assert(sizeof(T) <= sizeof(b.payload_));
        std::memcpy(b.payload_, payload, sizeof(T));
    }
    BroadcastXBusMessage(b);
}

template <typename Page, typename Item, typename Fill>
Page makePage(uint32_t page, const std::vector<Item>& items, Fill fill) {
    Page pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(items.size());
    uint64_t start = static_cast<uint64_t>(page) * 3;
    uint32_t n = 0;
    for (; n < 3 && start + n < items.size(); ++n) fill(pg.entries_[n], items[start + n]);
    pg.count_    = n;
    pg.has_more_ = (start + n < items.size()) ? 1u : 0u;
    return pg;
}

inline uint32_t readPage(const shared::XBusMessage& msg) {
    uint32_t page = 0;
    std::memcpy(&page, msg.payload_, sizeof(page));
    return page;
}

}  // namespace svc
}  // namespace fbx::xserver
