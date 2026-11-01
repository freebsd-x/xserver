#include "xserver/consent_broker.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <cstring>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace fbx::xserver::consent {

namespace {

struct Held {
    std::weak_ptr<IpcSocket> client;
    shared::XBusMessage      msg;
};

std::mutex                                                g_mu;
Replay                                                    g_replay;
std::map<std::pair<std::string, uint32_t>, std::vector<Held>> g_held;

constexpr size_t kMaxHeld = 16;

}  // namespace

void setReplay(Replay replay) noexcept {
    std::lock_guard lk(g_mu);
    g_replay = std::move(replay);
}

void hold(const std::string& app, const std::string& display_name, uint32_t cap,
          std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept {
    bool ask = false;
    {
        std::lock_guard lk(g_mu);
        auto& q = g_held[{app, cap}];
        ask = q.empty();                    // one question per (app, cap) at a time
        if (q.size() < kMaxHeld) q.push_back({client, msg});
    }
    if (!ask) return;
    LOG_INFO("Consent: asking whether {} may use capability {}", app, cap);
    shared::AppSandboxConsentRequest req{};
    std::strncpy(req.identifier_, app.c_str(), sizeof(req.identifier_) - 1);
    std::strncpy(req.display_name_, display_name.c_str(), sizeof(req.display_name_) - 1);
    req.permission_ = cap;
    svc::broadcast("com.freebsdx.AppSandbox",
                   static_cast<uint32_t>(shared::AppSandboxOpcode::ConsentRequested), &req);
}

void resolve(const std::string& app, uint32_t cap) noexcept {
    std::vector<Held> held;
    Replay replay;
    {
        std::lock_guard lk(g_mu);
        auto it = g_held.find({app, cap});
        if (it == g_held.end()) return;
        held = std::move(it->second);
        g_held.erase(it);
        replay = g_replay;
    }
    if (!replay) return;
    LOG_INFO("Consent: {} decided for capability {} — replaying {} held request(s)", app, cap, held.size());
    for (auto& h : held)
        if (auto c = h.client.lock()) replay(c, h.msg);   // a closed app's requests just drop
}

size_t pending(const std::string& app, uint32_t cap) noexcept {
    std::lock_guard lk(g_mu);
    auto it = g_held.find({app, cap});
    return it == g_held.end() ? 0 : it->second.size();
}

}  // namespace fbx::xserver::consent
