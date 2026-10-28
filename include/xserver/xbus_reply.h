#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

// Framing helpers shared by XBus service handlers: framed replies that echo
// transaction_id_, empty acks, broadcasts, and paged lists.
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

// Page size comes from the contract's entries_ array (3 for most, 2 for
// PrintJobPage).
template <typename Page, typename Item, typename Fill>
Page makePage(uint32_t page, const std::vector<Item>& items, Fill fill) {
    constexpr uint32_t kPer = std::extent_v<decltype(Page::entries_)>;
    Page pg{};
    pg.page_  = page;
    pg.total_ = static_cast<uint32_t>(items.size());
    uint64_t start = static_cast<uint64_t>(page) * kPer;
    uint32_t n = 0;
    for (; n < kPer && start + n < items.size(); ++n) fill(pg.entries_[n], items[start + n]);
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
