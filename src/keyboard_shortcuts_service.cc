#include "xserver/keyboard_shortcuts_service.h"
#include "xserver/config_service.h"
#include "xserver/ipc_socket.h"
#include "xserver/shortcuts.h"
#include "xserver/xbus_reply.h"
#include "shared/logger.h"

#include <cstdlib>
#include <cstring>
#include <string>

namespace fbx::xserver {

namespace {
using namespace svc;
constexpr const char* kDisabledKey = "shortcuts.disabled";
constexpr uint32_t    kValidBits   = (1u << shortcuts::kCount) - 1;
}  // namespace

void KeyboardShortcutsService::init() noexcept {
    uint32_t mask = static_cast<uint32_t>(std::strtoul(ConfigService::get(kDisabledKey, "0").c_str(), nullptr, 10));
    shortcuts::g_disabled = mask & kValidBits;
}

bool KeyboardShortcutsService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                              const shared::XBusMessage& msg) noexcept {
    if (std::strcmp(msg.service_name_, "com.freebsdx.KeyboardShortcuts") != 0) return false;
    using Op = shared::KeyboardShortcutOpcode;
    constexpr const char* kName = "com.freebsdx.KeyboardShortcuts";
    switch (static_cast<Op>(msg.opcode_)) {
    case Op::ListShortcuts: {
        uint32_t page = readPage(msg);
        shared::KeyShortcutPage pg{};
        pg.page_  = page;
        pg.total_ = shortcuts::kCount;
        if (page < shortcuts::kCount) {
            const auto& d = shortcuts::kAll[page];
            pg.valid_    = 1;
            pg.has_more_ = page + 1 < shortcuts::kCount ? 1u : 0u;
            std::strncpy(pg.action_, d.action, sizeof(pg.action_) - 1);
            std::strncpy(pg.binding_, d.binding, sizeof(pg.binding_) - 1);
            pg.enabled_ = shortcuts::enabled(d.id) ? 1u : 0u;
        }
        reply(msg, client, kName, msg.opcode_, &pg);
        break;
    }
    case Op::SetShortcut: {
        shared::KeyShortcutRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.action_[sizeof(req.action_) - 1] = '\0';
        shortcuts::Id id = shortcuts::byAction(req.action_);
        bool ok = id != shortcuts::kCount;
        if (ok) {
            shortcuts::setEnabled(id, req.enabled_ != 0);
            ConfigService::set(kDisabledKey, std::to_string(shortcuts::g_disabled.load()));
            LOG_INFO("KeyboardShortcutsService: '{}' {}", req.action_, req.enabled_ ? "on" : "off");
        } else {
            LOG_WARN("KeyboardShortcutsService: no shortcut '{}'", req.action_);
        }
        ack(msg, client, kName);
        if (ok) broadcast<char>(kName, static_cast<uint32_t>(Op::ShortcutsChanged), nullptr);
        break;
    }
    default: break;
    }
    return true;
}

} // namespace fbx::xserver
