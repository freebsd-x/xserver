#include "xserver/keymap_service.h"
#include "xserver/keymap.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <cstring>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

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

    ConfigService::set("keymap.layout",  req.layout_);
    ConfigService::set("keymap.variant", req.variant_);
    LOG_INFO("KeymapService: layout set to '{}' variant '{}' (persisted; xkb remap is a follow-up)",
             req.layout_, req.variant_);

    // Confirm via LayoutChanged broadcast (echo the request).
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.Keymap", sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::KeymapOpcode::LayoutChanged);
    std::memcpy(b.payload_, &req, sizeof(req));
    BroadcastXBusMessage(b);
}

}  // namespace fbx::xserver
