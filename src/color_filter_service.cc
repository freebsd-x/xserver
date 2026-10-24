#include "xserver/color_filter_service.h"
#include "xserver/config_service.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <array>
#include <functional>

namespace fbx::xserver {

void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {
constexpr const char* kService = "com.freebsdx.ColorFilter";
std::function<void(const std::array<double, 9>&)> g_apply;   // set via init()

using Mat = std::array<double, 9>;
constexpr Mat kIdentity = {1,0,0, 0,1,0, 0,0,1};
// Grayscale (Rec.601 luma) — every output channel = luminance.
constexpr Mat kMono = {0.299,0.587,0.114, 0.299,0.587,0.114, 0.299,0.587,0.114};
// Standard colour-blindness simulation matrices (sRGB, row-major).
constexpr Mat kProtanopia   = {0.567,0.433,0.000, 0.558,0.442,0.000, 0.000,0.242,0.758};
constexpr Mat kDeuteranopia = {0.625,0.375,0.000, 0.700,0.300,0.000, 0.000,0.300,0.700};
constexpr Mat kTritanopia   = {0.950,0.050,0.000, 0.000,0.433,0.567, 0.000,0.475,0.525};

uint32_t cfgU32(const char* key, uint32_t def) {
    std::string v = ConfigService::get(key, std::to_string(def));
    try { return static_cast<uint32_t>(std::stoul(v)); } catch (...) { return def; }
}

shared::ColorFilterState currentState() {
    shared::ColorFilterState st{};
    st.enabled_    = cfgU32("colorfilter.enabled", 0) ? 1u : 0u;
    st.type_       = std::min(cfgU32("colorfilter.type", 0), 3u);   // 0..3
    st.intensity_  = std::min(cfgU32("colorfilter.intensity", 100), 100u);
    st.monochrome_ = cfgU32("colorfilter.monochrome", 0) ? 1u : 0u;
    return st;
}

void broadcast(const shared::ColorFilterState& st) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, kService, sizeof(b.service_name_) - 1);
    b.opcode_ = static_cast<uint32_t>(shared::ColorFilterOpcode::StateChanged);
    std::memcpy(b.payload_, &st, sizeof(st));
    BroadcastXBusMessage(b);
}

void reply(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t opcode,
           const shared::ColorFilterState& st) {
    if (!client) return;
    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, kService, sizeof(m.service_name_) - 1);
    m.opcode_ = opcode;
    std::memcpy(m.payload_, &st, sizeof(st));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

uint32_t firstU32(const shared::XBusMessage& msg) {
    uint32_t v = 0; std::memcpy(&v, msg.payload_, sizeof(v)); return v;
}
}  // namespace

void ColorFilterService::handleMessage(const shared::XBusMessage& msg,
                                       std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ColorFilterOpcode;
    O op = static_cast<O>(msg.opcode_);
    switch (op) {
    case O::GetState:
        reply(client, msg.transaction_id_, static_cast<uint32_t>(O::GetState), currentState());
        return;
    case O::SetEnabled:
        ConfigService::set("colorfilter.enabled", std::to_string(firstU32(msg) ? 1u : 0u));
        break;
    case O::SetType:
        ConfigService::set("colorfilter.type", std::to_string(std::min(firstU32(msg), 3u)));
        break;
    case O::SetIntensity:
        ConfigService::set("colorfilter.intensity", std::to_string(std::min(firstU32(msg), 100u)));
        break;
    case O::SetMonochrome:
        ConfigService::set("colorfilter.monochrome", std::to_string(firstU32(msg) ? 1u : 0u));
        break;
    default:
        return;
    }
    // A setter changed state: apply the colour matrix, broadcast, and ack.
    auto st = currentState();
    if (g_apply) g_apply(computeMatrix(st));
    broadcast(st);
    reply(client, msg.transaction_id_, msg.opcode_, st);
}

std::array<double, 9> ColorFilterService::computeMatrix(const shared::ColorFilterState& st) noexcept {
    if (!st.enabled_) return kIdentity;
    const Mat& base = st.monochrome_ ? kMono
                    : st.type_ == 1 ? kDeuteranopia
                    : st.type_ == 2 ? kProtanopia
                    : st.type_ == 3 ? kTritanopia
                    : kIdentity;                       // type 0 (Normal) → identity
    double t = std::clamp(static_cast<double>(st.intensity_), 0.0, 100.0) / 100.0;
    Mat out{};
    for (int i = 0; i < 9; ++i) out[i] = kIdentity[i] * (1.0 - t) + base[i] * t;  // lerp
    return out;
}

void ColorFilterService::init(std::function<void(const std::array<double, 9>&)> apply) noexcept {
    g_apply = std::move(apply);
    if (g_apply) g_apply(computeMatrix(currentState()));   // push current at startup
}

}  // namespace fbx::xserver
