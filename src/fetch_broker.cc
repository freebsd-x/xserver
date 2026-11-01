#include "xserver/fetch_broker.h"
#include "xserver/desktop_services.h"
#include "xserver/fetch_policy.h"
#include "xserver/ipc_socket.h"
#include "xserver/peer_identity.h"

#include <cstring>
#include <iostream>
#include <mutex>
#include <unordered_map>

namespace fbx::xserver {
namespace {

constexpr const char* kName = "com.freebsdx.Fetch";
constexpr size_t kMaxPending = 256;   // relayed requests awaiting xfetch

struct Pending {
    std::weak_ptr<IpcSocket> client;  // weak: the app may be gone (and its fd reused) by reply time
    uint32_t                 txn;     // the app's own transaction id
};

std::mutex                            g_mu;
std::weak_ptr<IpcSocket>              g_provider;
std::unordered_map<uint32_t, Pending> g_pending;   // keyed by the id we gave xfetch
uint32_t                              g_next_id = 1;

void replyTo(const std::shared_ptr<IpcSocket>& c, uint32_t txn, const shared::FetchReply& r, int fd = -1) {
    shared::XBusMessage m{};
    m.magic_ = shared::XBUS_MAGIC;
    m.transaction_id_ = txn;
    std::strncpy(m.service_name_, kName, sizeof(m.service_name_) - 1);
    m.opcode_ = static_cast<uint32_t>(shared::FetchOpcode::Get);
    std::memcpy(m.payload_, &r, sizeof(r));
    (void)c->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)}, fd);
}

void fail(const std::shared_ptr<IpcSocket>& c, uint32_t txn, shared::FetchStatus st) {
    shared::FetchReply r{};
    r.status_ = static_cast<int32_t>(st);
    replyTo(c, txn, r);
}

// Answer everything still waiting on a provider that's gone or was replaced.
void failAllPendingLocked() {
    for (auto& [id, p] : g_pending)
        if (auto c = p.client.lock()) fail(c, p.txn, shared::FetchStatus::Unavailable);
    g_pending.clear();
}

}  // namespace

void FetchBroker::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                 const shared::XBusMessage& msg, UniqueFd& fd) noexcept {
    using O = shared::FetchOpcode;
    std::lock_guard<std::mutex> lk(g_mu);
    switch (static_cast<O>(msg.opcode_)) {
    case O::ProviderRegister:
        // Only the real xfetch binary may serve fetches (by executable, like
        // the shell's trust check) — anything else could read every app's
        // requests and answer them with whatever it liked.
        if (!peer::isOneOf(client->fd(), {"xfetch"})) {
            std::cerr << "[Fetch] DENIED: provider registration from " << peer::executable(client->fd()) << "\n";
            return;
        }
        failAllPendingLocked();
        g_provider = client;
        std::cout << "[Fetch] xfetch registered\n";
        return;

    case O::ProviderReply: {
        if (client != g_provider.lock()) return;   // only the registered provider answers
        auto it = g_pending.find(msg.transaction_id_);
        if (it == g_pending.end()) return;
        Pending p = it->second;
        g_pending.erase(it);
        auto c = p.client.lock();
        if (!c) return;
        shared::FetchReply r{};
        std::memcpy(&r, msg.payload_, sizeof(r));
        replyTo(c, p.txn, r, fd ? fd.get() : -1);
        return;
    }

    case O::Get: {
        shared::FetchRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.url_[sizeof(req.url_) - 1] = '\0';
        const std::string app = verifiedAppId(client->fd());
        if (!fetch_policy::allowed(app, req.url_)) {
            std::cerr << "[Fetch] DENIED: " << (app.empty() ? "unknown app" : app) << " → " << req.url_ << "\n";
            fail(client, msg.transaction_id_, shared::FetchStatus::Denied);
            return;
        }
        auto provider = g_provider.lock();
        if (!provider) { fail(client, msg.transaction_id_, shared::FetchStatus::Unavailable); return; }
        if (g_pending.size() >= kMaxPending) { fail(client, msg.transaction_id_, shared::FetchStatus::Unavailable); return; }

        uint32_t id = g_next_id++;
        if (id == 0) id = g_next_id++;
        shared::XBusMessage fwd = msg;
        fwd.transaction_id_ = id;
        fwd.opcode_ = static_cast<uint32_t>(O::Get);
        std::memcpy(fwd.payload_, &req, sizeof(req));
        if (!provider->sendMessage({reinterpret_cast<const uint8_t*>(&fwd), sizeof(fwd)})) {
            fail(client, msg.transaction_id_, shared::FetchStatus::Unavailable);
            return;
        }
        g_pending[id] = {client, msg.transaction_id_};
        return;
    }
    }
    fail(client, msg.transaction_id_, shared::FetchStatus::BadRequest);
}

}  // namespace fbx::xserver
