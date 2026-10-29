#include "xserver/keymap_service.h"
#include "xserver/keymap.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <cstring>
#include <functional>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
std::function<bool(const std::string&, const std::string&)> g_apply;
}  // namespace

void KeymapService::init(std::function<bool(const std::string&, const std::string&)> apply) noexcept {
    g_apply = std::move(apply);
    std::string layout  = ConfigService::get("keymap.layout", "us");
    std::string variant = ConfigService::get("keymap.variant", "");
    if (!keymap::isValidLayout(layout) || !keymap::isValidVariant(variant)) { layout = "us"; variant.clear(); }
    if (g_apply && !g_apply(layout, variant) && layout != "us") g_apply("us", "");
}

void KeymapService::handleMessage(const shared::XBusMessage& msg,
                                  std::shared_ptr<IpcSocket> client) noexcept {
    (void)client;   // SetLayout has no direct reply — confirmation is the broadcast.
    if (static_cast<shared::KeymapOpcode>(msg.opcode_) != shared::KeymapOpcode::SetLayout)
        return;

    shared::KeymapRequest req{};
    std::memcpy(&req, msg.payload_, sizeof(req));
    req.layout_[sizeof(req.layout_) - 1]   = '\0';
    req.variant_[sizeof(req.variant_) - 1] = '\0';

    if (!keymap::isValidLayout(req.layout_) || !keymap::isValidVariant(req.variant_)) {
        LOG_WARN("KeymapService: rejected invalid layout '{}' variant '{}'",
                 req.layout_, req.variant_);
        return;   // don't persist or confirm a bogus layout
    }

    if (g_apply && !g_apply(req.layout_, req.variant_)) {
        LOG_WARN("KeymapService: XKB has no layout '{}' variant '{}'", req.layout_, req.variant_);
        return;   // nothing changed; don't persist or confirm it
    }
    ConfigService::set("keymap.layout",  req.layout_);
    ConfigService::set("keymap.variant", req.variant_);
    LOG_INFO("KeymapService: layout set to '{}' variant '{}'", req.layout_, req.variant_);

    // Confirm via LayoutChanged broadcast (echo the request).
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.Keymap", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::KeymapOpcode::LayoutChanged);
    std::memcpy(b.payload_, &req, sizeof(req));
    BroadcastXBusMessage(b);
}

}  // namespace fbx::xserver
