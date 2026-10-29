#include "xserver/color_profile_service.h"
#include "xserver/color_profile.h"
#include "xserver/config_service.h"
#include "shared/logger.h"

#include <cstring>
#include <string>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kSvc = "com.freebsdx.ColorProfile";

std::function<void(const std::array<double, 9>&)> g_apply;

shared::ColorProfileState build() {
    shared::ColorProfileState st{};
    uint32_t p = 0;
    try { p = static_cast<uint32_t>(std::stoul(ConfigService::get("colorprofile.profile", "0"))); }
    catch (...) { p = 0; }
    st.profile_ = colorprofile::clampProfile(p);
    std::strncpy(st.profile_name_, colorprofile::profileName(st.profile_), sizeof(st.profile_name_) - 1);
    return st;
}
}  // namespace

void ColorProfileService::init(std::function<void(const std::array<double, 9>&)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(colorprofile::matrixFor(build().profile_));
}

void ColorProfileService::handleMessage(const shared::XBusMessage& msg,
                                        std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ColorProfileOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetProfile: {
        auto st = build();
        if (client) {
            shared::XBusMessage r{};
            r.transaction_id_ = msg.transaction_id_;
            std::strncpy(r.service_name_, kSvc, sizeof(r.service_name_) - 1);
            r.opcode_ = static_cast<uint32_t>(O::GetProfile);
            std::memcpy(r.payload_, &st, sizeof(st));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&r), sizeof(r)});
        }
        break;
    }
    case O::SetProfile: {
        shared::ColorProfileRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t p = colorprofile::clampProfile(req.profile_);
        ConfigService::set("colorprofile.profile", std::to_string(p));
        if (g_apply) g_apply(colorprofile::matrixFor(p));
        LOG_INFO("ColorProfileService: profile set to {} ({})", p, colorprofile::profileName(p));
        // No direct reply per contract — confirm via ProfileChanged broadcast.
        auto st = build();
        shared::XBusMessage b{};
        std::strncpy(b.service_name_, kSvc, sizeof(b.service_name_) - 1);
        b.opcode_ = static_cast<uint32_t>(O::ProfileChanged);
        std::memcpy(b.payload_, &st, sizeof(st));
        BroadcastXBusMessage(b);
        break;
    }
    default: break;
    }
}

}  // namespace fbx::xserver
