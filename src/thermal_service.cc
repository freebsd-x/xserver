#include "xserver/thermal_service.h"

#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#if __has_include(<sys/sysctl.h>)
#  include <sys/sysctl.h>
#  define HAVE_SYSCTL 1
#endif

namespace fbx::xserver {

namespace {

// Read an integer sysctl in deci-Kelvin (FreeBSD temperature sensors: tenths of a
// Kelvin). Returns Celsius in `out`; false if the sensor is absent.
bool readTempC(const char* name, uint32_t& out) {
#ifdef HAVE_SYSCTL
    int dk = 0; std::size_t sz = sizeof(dk);
    if (sysctlbyname(name, &dk, &sz, nullptr, 0) != 0) return false;
    if (dk <= 0) return false;
    int c = (dk - 2732) / 10;                 // 2732 dK ≈ 0.05 °C
    out = c < 0 ? 0u : static_cast<uint32_t>(c);
    return true;
#else
    (void)name; (void)out; return false;
#endif
}

}  // namespace

std::vector<ThermalService::Zone> ThermalService::readZones() noexcept {
    std::vector<Zone> zones;
#ifdef HAVE_SYSCTL
    int ncpu = 1; std::size_t l = sizeof(ncpu);
    sysctlbyname("hw.ncpu", &ncpu, &l, nullptr, 0);
    if (ncpu < 1 || ncpu > 1024) ncpu = 1;
    for (int i = 0; i < ncpu; ++i) {
        char name[64]; std::snprintf(name, sizeof(name), "dev.cpu.%d.temperature", i);
        uint32_t c;
        if (readTempC(name, c)) {
            Zone z{}; z.temp_c = c;
            std::snprintf(z.label, sizeof(z.label), "CPU %d", i);
            zones.push_back(z);
        }
    }
    for (int i = 0; i < 16; ++i) {
        char name[64]; std::snprintf(name, sizeof(name), "hw.acpi.thermal.tz%d.temperature", i);
        uint32_t c;
        if (readTempC(name, c)) {
            Zone z{}; z.temp_c = c;
            std::snprintf(z.label, sizeof(z.label), "ACPI Thermal %d", i);
            zones.push_back(z);
        }
    }
#endif
    return zones;
}

void ThermalService::handleMessage(const shared::XBusMessage& msg,
                                   std::shared_ptr<IpcSocket> client) noexcept {
    using O = shared::ThermalOpcode;
    switch (static_cast<O>(msg.opcode_)) {
    case O::GetZones: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        auto zones = readZones();

        shared::ThermalZonePage pg{};
        pg.page_     = page;
        pg.total_    = static_cast<uint32_t>(zones.size());
        pg.has_more_ = (page + 1 < zones.size()) ? 1u : 0u;
        if (page < zones.size()) {
            pg.valid_    = 1;
            pg.zone_id_  = page;
            pg.temp_c_   = zones[page].temp_c;
            pg.fan_rpm_  = 0;
            pg.fan_mode_ = 0;
            pg.fan_pct_  = 0;
            std::strncpy(pg.label_, zones[page].label, sizeof(pg.label_) - 1);
        }
        if (client) {
            shared::XBusMessage m{};
            m.transaction_id_ = msg.transaction_id_;
            std::strncpy(m.service_name_, "com.freebsdx.Thermal", sizeof(m.service_name_) - 1);
            m.opcode_ = static_cast<uint32_t>(O::GetZones);
            std::memcpy(m.payload_, &pg, sizeof(pg));
            client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        }
        break;
    }
    case O::SetFanMode:
        // Fan control needs hardware/root the compositor lacks — accept as a no-op.
        if (client) {
            shared::XBusMessage m{};
            m.transaction_id_ = msg.transaction_id_;
            std::strncpy(m.service_name_, "com.freebsdx.Thermal", sizeof(m.service_name_) - 1);
            m.opcode_ = static_cast<uint32_t>(O::SetFanMode);
            client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
        }
        break;
    default:
        break;
    }
}

}  // namespace fbx::xserver
