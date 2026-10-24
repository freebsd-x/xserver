#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"
#include <memory>
#include <cstdint>

namespace fbx::xserver {

// com.freebsdx.GPS — location provider.
//
// Sources, in priority order:
//   1. NMEA serial device — a real GPS dongle. Path from $FREEBSDX_GPS_DEVICE,
//      else the first readable of /dev/gps0, /dev/cuau0, /dev/ttyU0. A background
//      thread reads sentences and updates the current fix.
//   2. Manual location — gps.lat / gps.lon set via ConfigService (a deterministic
//      offline fallback for machines without a receiver).
//
// A LocationUpdate (opcode 0x01) is broadcast whenever a fix is obtained. Its
// payload is two little-endian doubles: latitude then longitude — the exact
// layout Maps.xapp already listens for.
class GpsService {
public:
    enum Opcode : uint32_t {
        LocationUpdate  = 0x01,  // service → clients (broadcast): double lat, double lon
        RequestLocation = 0x02,  // client → service: refresh, reply GpsFixReply + broadcast
        GetLocation     = 0x03,  // client → service: reply last-known GpsFixReply (no refresh)
    };

    enum Source : uint8_t { None = 0, Nmea = 1, Manual = 2 };

    // Load manual config and, if a receiver device is present, start the reader
    // thread. Safe to call once at startup.
    static void init() noexcept;

    static void handleMessage(const shared::XBusMessage& msg,
                              std::shared_ptr<IpcSocket> client) noexcept;

    // Best current fix (test/inspection seam). valid=false until one is obtained.
    struct Fix {
        bool     valid = false;
        double   lat = 0.0;
        double   lon = 0.0;
        float    accuracy_m = 0.0f;
        uint8_t  source = None;
    };
    [[nodiscard]] static Fix currentFix() noexcept;
};

// Wire payload for RequestLocation / GetLocation replies.
struct GpsFixReply {
    uint8_t  has_fix_;        // 1 = valid fix present
    uint8_t  source_;         // GpsService::Source
    double   lat_;
    double   lon_;
    float    accuracy_m_;     // 0 = unknown
};

}  // namespace fbx::xserver
