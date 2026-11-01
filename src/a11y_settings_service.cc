#include "xserver/a11y_settings_service.h"
#include "xserver/config_service.h"
#include "xserver/speech.h"
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
    s.screen_reader_    = cfgU("a11y.screen_reader", 0) ? 1u : 0u;
    return s;
}
// The full accessibility panel's settings (com.freebsdx.A11yPanel), from the
// same config keys as the quick settings, so both panels and the screen reader
// agree.
constexpr const char* kPanelService = "com.freebsdx.A11yPanel";
shared::AccessibilitySettings buildFull() {
    shared::AccessibilitySettings f{};
    f.high_contrast_      = cfgU("a11y.high_contrast", 0) ? 1u : 0u;
    f.large_text_         = cfgU("a11y.font_preset", 1) >= 2 ? 1u : 0u;
    f.magnifier_          = cfgU("a11y.magnifier", 0) ? 1u : 0u;
    f.magnifier_scale_    = std::clamp(cfgU("a11y.magnifier_scale", 200), 100u, 500u);
    f.screen_reader_      = cfgU("a11y.screen_reader", 0) ? 1u : 0u;
    f.reduce_motion_      = cfgU("a11y.reduce_motion", 0) ? 1u : 0u;
    f.sticky_keys_        = cfgU("a11y.sticky_keys", 0) ? 1u : 0u;
    f.slow_keys_          = cfgU("a11y.slow_keys", 0) ? 1u : 0u;
    f.slow_keys_delay_ms_ = std::min(cfgU("a11y.slow_keys_delay_ms", 300), 2000u);
    f.bounce_keys_        = cfgU("a11y.bounce_keys", 0) ? 1u : 0u;
    f.cursor_size_        = std::clamp(cfgU("a11y.cursor_size", 1), 1u, 4u);
    return f;
}
// Tell both panels (quick settings and the full panel).
void bcast(const shared::AccessibilityQuickSettings& s) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::AccessibilitySettingsOpcode::SettingsChanged);
    std::memcpy(b.payload_, &s, sizeof(s)); BroadcastXBusMessage(b);
    shared::XBusMessage p{};
    std::strncpy(p.service_name_, kPanelService, sizeof(p.service_name_) - 1);
    p.opcode_ = static_cast<uint32_t>(shared::AccessibilityPanelOpcode::SettingsChanged);
    auto f = buildFull();
    std::memcpy(p.payload_, &f, sizeof(f)); BroadcastXBusMessage(p);
}
void setReader(bool on) {
    const bool was = cfgU("a11y.screen_reader", 0) != 0;
    ConfigService::set("a11y.screen_reader", on ? "1" : "0");
    if (on != was) speech::say(on ? "Screen reader on" : "Screen reader off", /*even_if_off=*/true);
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
    case O::SetScreenReader: {
        shared::AccessibilityToggleRequest r{}; std::memcpy(&r, msg.payload_, sizeof(r));
        setReader(r.enabled_ != 0);
        break;
    }
    default: return;
    }
    bcast(build());
}
void A11ySettingsService::handlePanelMessage(const shared::XBusMessage& msg,
                                             std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::AccessibilityPanelOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetSettings: {
        auto f = buildFull();
        if (client) {
            shared::XBusMessage m = msg;
            std::memcpy(m.payload_, &f, sizeof(f));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        }
        break;
    }
    case O::SetSettings: {
        shared::AccessibilitySettings f{};
        std::memcpy(&f, msg.payload_, sizeof(f));
        auto b = [](uint32_t v) { return std::string(v ? "1" : "0"); };
        ConfigService::set("a11y.high_contrast", b(f.high_contrast_));
        const uint32_t preset = cfgU("a11y.font_preset", 1);
        if (f.large_text_ && preset < 2) ConfigService::set("a11y.font_preset", "2");
        if (!f.large_text_ && preset >= 2) ConfigService::set("a11y.font_preset", "1");
        ConfigService::set("a11y.magnifier", b(f.magnifier_));
        ConfigService::set("a11y.magnifier_scale", std::to_string(std::clamp(f.magnifier_scale_, 100u, 500u)));
        ConfigService::set("a11y.reduce_motion", b(f.reduce_motion_));
        ConfigService::set("a11y.sticky_keys", b(f.sticky_keys_));
        ConfigService::set("a11y.slow_keys", b(f.slow_keys_));
        ConfigService::set("a11y.slow_keys_delay_ms", std::to_string(std::min(f.slow_keys_delay_ms_, 2000u)));
        ConfigService::set("a11y.bounce_keys", b(f.bounce_keys_));
        ConfigService::set("a11y.cursor_size", std::to_string(std::clamp(f.cursor_size_, 1u, 4u)));
        setReader(f.screen_reader_ != 0);
        bcast(build());
        break;
    }
    default: break;
    }
}

bool A11ySettingsService::screenReaderOn() noexcept {
    return cfgU("a11y.screen_reader", 0) != 0;
}
}  // namespace fbx::xserver
