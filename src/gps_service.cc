#include "xserver/gps_service.h"
#include "xserver/config_service.h"
#include "xserver/nmea_parse.h"

#include <cstring>
#include <cstdlib>
#include <string>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace fbx::xserver {

// Delivered by the compositor (xbus_dispatcher.cc); a no-op in standalone daemons
// (broadcast_stub.cc).
void BroadcastXBusMessage(const shared::XBusMessage&);

namespace {

std::mutex          g_mutex;
GpsService::Fix     g_nmea_fix;   // latest hardware fix (guarded by g_mutex)

// Parse a config value as a double. Returns false if empty/unparseable.
bool parseD(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

// The manual location configured via ConfigService, if any.
GpsService::Fix manualFix() {
    GpsService::Fix f;
    double lat, lon;
    if (parseD(ConfigService::get("gps.lat", ""), lat) &&
        parseD(ConfigService::get("gps.lon", ""), lon)) {
        // Reject out-of-range coordinates rather than reporting a bogus fix.
        if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0) {
            f.valid = true; f.lat = lat; f.lon = lon;
            f.accuracy_m = 0.0f; f.source = GpsService::Manual;
        }
    }
    return f;
}

// Best fix: a live hardware fix wins; otherwise the manual config location.
GpsService::Fix bestFix() {
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_nmea_fix.valid) return g_nmea_fix;
    }
    return manualFix();
}

void broadcastLocation(double lat, double lon) {
    shared::XBusMessage b{};
    std::strncpy(b.service_name_, "com.freebsdx.GPS", sizeof(b.service_name_) - 1);
    b.opcode_ = GpsService::LocationUpdate;   // 0x01
    std::memcpy(b.payload_,     &lat, sizeof(lat));
    std::memcpy(b.payload_ + 8, &lon, sizeof(lon));
    BroadcastXBusMessage(b);
}

void replyFix(std::shared_ptr<IpcSocket>& client, uint32_t tid, uint32_t opcode,
              const GpsService::Fix& f) {
    if (!client) return;
    GpsFixReply r{};
    r.has_fix_    = f.valid ? 1 : 0;
    r.source_     = f.source;
    r.lat_        = f.lat;
    r.lon_        = f.lon;
    r.accuracy_m_ = f.accuracy_m;

    shared::XBusMessage m{};
    m.transaction_id_ = tid;
    std::strncpy(m.service_name_, "com.freebsdx.GPS", sizeof(m.service_name_) - 1);
    m.opcode_ = opcode;   // echo the request's opcode
    std::memcpy(m.payload_, &r, sizeof(r));
    client->sendMessage({reinterpret_cast<const uint8_t*>(&m), sizeof(m)});
}

// The receiver device, or "" if none is present.
std::string devicePath() {
    if (const char* e = std::getenv("FREEBSDX_GPS_DEVICE"); e && e[0]) {
        return (access(e, R_OK) == 0) ? std::string(e) : std::string();
    }
    for (const char* p : {"/dev/gps0", "/dev/cuau0", "/dev/ttyU0"})
        if (access(p, R_OK) == 0) return p;
    return {};
}

// Read NMEA sentences from a serial device forever, publishing each new fix.
// The port is read as-is (no baud change) so a correctly-configured tty is not
// disturbed; garbage from a mismatched baud simply fails to parse.
void readerLoop(std::string dev) {
    int fd = ::open(dev.c_str(), O_RDONLY | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return;
    std::string acc;
    char buf[512];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            acc.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                nmea::Fix pf = nmea::parseSentence(std::string_view(acc).substr(0, nl));
                acc.erase(0, nl + 1);
                if (pf.valid) {
                    {
                        std::lock_guard<std::mutex> lk(g_mutex);
                        g_nmea_fix = {true, pf.lat, pf.lon, pf.accuracy_m, GpsService::Nmea};
                    }
                    broadcastLocation(pf.lat, pf.lon);
                }
            }
            if (acc.size() > 8192) acc.clear();   // bound the buffer on a garbage feed
        } else if (n == 0) {
            usleep(500000);                       // EOF on a tty — wait and retry
        } else {
            usleep(200000);                       // EWOULDBLOCK — poll again shortly
        }
    }
    ::close(fd);   // unreachable in normal operation
}

}  // namespace

void GpsService::init() noexcept {
    std::string dev = devicePath();
    if (!dev.empty()) {
        std::thread(readerLoop, dev).detach();
    }
    // No device → the service still answers from the manual config location.
}

GpsService::Fix GpsService::currentFix() noexcept { return bestFix(); }

void GpsService::handleMessage(const shared::XBusMessage& msg,
                               std::shared_ptr<IpcSocket> client) noexcept {
    switch (msg.opcode_) {
    case RequestLocation: {
        Fix f = bestFix();
        if (f.valid) broadcastLocation(f.lat, f.lon);   // clients update their pin
        replyFix(client, msg.transaction_id_, RequestLocation, f);
        break;
    }
    case GetLocation: {
        replyFix(client, msg.transaction_id_, GetLocation, bestFix());   // no broadcast
        break;
    }
    default:
        // Unknown opcode — reply with the current (possibly empty) fix so callers
        // never hang waiting for a response.
        replyFix(client, msg.transaction_id_, msg.opcode_, bestFix());
        break;
    }
}

}  // namespace fbx::xserver
