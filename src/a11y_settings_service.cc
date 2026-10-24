#include "xserver/a11y_settings_service.h"
#include "xserver/config_service.h"
#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
namespace fbx::xserver {
void BroadcastXBusMessage(const shared::XBusMessage&);
namespace {
constexpr const char* kService = "com.freebsdx.A11ySettings";
uint32_t cfgU(const char* k, uint32_t d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return d; }
}
float cfgF(const char* k, float d) {
    std::string v = ConfigService::get(k, std::to_string(d));
    try { return std::stof(v); } catch (...) { return d; }
}
shared::AccessibilityQuickSettings build() {
    shared::AccessibilityQuickSettings s{};
    s.font_size_preset_ = std::min(cfgU("a11y.font_preset", 1), 3u);
    s.cursor_scale_     = std::clamp(cfgF("a11y.cursor_scale", 1.0f), 1.0f, 3.0f);
    s.high_contrast_    = cfgU("a11y.high_contrast", 0) ? 1u : 0u;
    s.magnifier_        = cfgU("a11y.magnifier", 0) ? 1u : 0u;
    return s;
}
void bcast(const shared::AccessibilityQuickSettings& s) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::AccessibilitySettingsOpcode::SettingsChanged);
    std::memcpy(b.payload_, &s, sizeof(s)); BroadcastXBusMessage(b);
}
}  // namespace
void A11ySettingsService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AccessibilitySettingsOpcode;
    O op = static_cast<O>(msg.opcode_);
    if (op == O::GetSettings) {
        auto s = build();
        if (client) {
            shared::XBusMessage m{}; m.transaction_id_ = msg.transaction_id_;
            std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
            m.opcode_ = static_cast<uint32_t>(O::GetSettings);
            std::memcpy(m.payload_, &s, sizeof(s));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        }
        return;
    }
    switch (op) {   // per-field setters (no response)
    case O::SetFontSize: {
        shared::AccessibilityFontRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        ConfigService::set("a11y.font_preset", std::to_string(std::min(r.preset_, 3u)));
        break;
    }
    case O::SetCursorScale: {
        shared::AccessibilityCursorRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        ConfigService::set("a11y.cursor_scale", std::to_string(std::clamp(r.scale_, 1.0f, 3.0f)));
        break;
    }
    case O::SetHighContrast: {
        shared::AccessibilityToggleRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        ConfigService::set("a11y.high_contrast", std::to_string(r.enabled_ ? 1u : 0u));
        break;
    }
    case O::SetMagnifier: {
        shared::AccessibilityToggleRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        ConfigService::set("a11y.magnifier", std::to_string(r.enabled_ ? 1u : 0u));
        break;
    }
    default: return;
    }
    bcast(build());
}
}  // namespace fbx::xserver
