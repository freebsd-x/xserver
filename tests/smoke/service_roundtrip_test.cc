// Integration smoke test for XBus service handlers.
//
// Unit tests cover each service's pure logic (parsers/formatters); this test
// covers the seam they can't: a real request sent over a socket, dispatched to a
// service's handleMessage(), producing a correctly *framed* reply — full 300-byte
// XBusMessage, echoed transaction_id_, right service_name_/opcode_, decodable
// payload. That framing seam is exactly where the historical bugs lived (payload-
// only replies, missing transaction_id echo). No compositor, Vulkan, or DRM —
// just a socketpair(), like ipc_pipeline_test.
//
// Services are driven directly (not through the dispatcher, which pulls in the
// whole compositor); the dispatcher's strcmp routing is validated by its compile.

#include "xserver/ipc_socket.h"
#include "xserver/netconn_service.h"
#include "xserver/proxy_service.h"
#include "xserver/wol_service.h"
#include "xserver/sysinfo_service.h"
#include "xserver/mounted_vol_service.h"
#include "xserver/process_service.h"
#include "xserver/syslog_service.h"
#include "xserver/usb_service.h"
#include "xserver/smart_service.h"
#include "xserver/kmod_service.h"
#include "xserver/sysadmin_services.h"
#include "xserver/user_services.h"
#include "xserver/device_services.h"
#include "xserver/infra_services.h"
#include "xserver/personal_services.h"
#include "xserver/lock_service.h"
#include "xserver/battery_health_service.h"
#include "xserver/audio_output_service.h"
#include "xserver/gpu_monitor_service.h"
#include "xserver/frame_perf_service.h"
#include "xserver/battery_drain_service.h"
#include "xserver/keymap_service.h"
#include "xserver/adblock_service.h"
#include "xserver/location_service.h"
#include "xserver/kbd_backlight_service.h"
#include "xserver/energy_service.h"
#include "xserver/color_profile_service.h"
#include "xserver/dns_config_service.h"
#include "xserver/firewall_service.h"
#include "xserver/backlight_service.h"
#include "xserver/display_arrange_service.h"
#include "xserver/datasaver_service.h"
#include "xserver/dnd_service.h"
#include "xserver/micmute_service.h"
#include "xserver/rotation_lock_service.h"
#include "xserver/wakelock_service.h"
#include "xserver/accessibility_service.h"
#include "xserver/scrolldir_service.h"
#include "xserver/micmute_service.h"
#include "xserver/gps_service.h"
#include "xserver/timezone_service.h"
#include "xserver/disk_usage_service.h"
#include "xserver/thermal_service.h"
#include "xserver/mem_pressure_service.h"
#include "xserver/cursor_size_service.h"
#include "xserver/text_size_service.h"
#include "xserver/screen_timeout_service.h"
#include "xserver/pointer_service.h"
#include "xserver/color_filter_service.h"
#include "xserver/nightlight_service.h"
#include "xserver/touchpad_service.h"
#include "xserver/display_scale_service.h"
#include "xserver/system_sounds_service.h"
#include "xserver/privacy_service.h"
#include "xserver/camera_service.h"
#include "xserver/focus_mode_service.h"
#include "xserver/auto_brightness_service.h"
#include "xserver/refresh_rate_service.h"
#include "xserver/dock_service.h"
#include "xserver/gesture_service.h"
#include "xserver/a11y_settings_service.h"
#include "xserver/update_channel_service.h"
#include "xserver/spatial_audio_service.h"
#include "xserver/stylus_service.h"
#include "xserver/config_service.h"
#include <fstream>
#include <filesystem>
#include "xserver/unique_fd.h"
#include "shared/protocol.h"
#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sstream>
#include <climits>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <array>

// Counts LockService::lock() calls (lock_service_stub.cc).
namespace fbx::xserver { extern std::atomic<int> g_stub_lock_calls; }

using namespace fbx::xserver;
using namespace fbx::shared;

namespace {

// peer = the end the test reads the reply from; svc = the end the service writes
// to (handed to handleMessage as the "client").
struct Pipe {
    IpcSocket peer;
    std::shared_ptr<IpcSocket> svc;
};
Pipe makePipe() {
    int sv[2];
    EXPECT_EQ(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv), 0)
        << strerror(errno);
    return Pipe{IpcSocket{UniqueFd{sv[0]}},
                std::make_shared<IpcSocket>(IpcSocket{UniqueFd{sv[1]}})};
}

XBusMessage makeReq(const char* service, uint32_t opcode, uint32_t tid,
                    const void* payload = nullptr, size_t len = 0) {
    XBusMessage m{};
    m.magic_          = XBUS_MAGIC;
    m.transaction_id_ = tid;
    m.opcode_         = opcode;
    std::strncpy(m.service_name_, service, sizeof(m.service_name_) - 1);
    if (payload && len) std::memcpy(m.payload_, payload, std::min(len, sizeof(m.payload_)));
    return m;
}

// Read one reply datagram and validate the common framing invariants.
XBusMessage recvReply(IpcSocket& s, const char* service, uint32_t opcode, uint32_t tid) {
    auto r = s.receiveMessage();
    EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message());
    XBusMessage m{};
    if (r.has_value()) {
        EXPECT_EQ(r->first.size(), sizeof(XBusMessage))     // full frame, not payload-only
            << "reply must be a full 300-byte XBusMessage";
        std::memcpy(&m, r->first.data(), std::min(sizeof(m), r->first.size()));
        EXPECT_EQ(m.transaction_id_, tid) << "transaction_id_ must be echoed";
        EXPECT_STREQ(m.service_name_, service);
        EXPECT_EQ(m.opcode_, opcode);
    }
    return m;
}

// Isolate persisted config (~/.freebsdx-*) into a throwaway HOME.
class ServiceRoundtrip : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/fbxsvc.XXXXXX";
        char* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr) << strerror(errno);
        home_ = dir;
        setenv("HOME", home_.c_str(), 1);
    }
    void TearDown() override {
        // best-effort cleanup of everything the services wrote under HOME
        std::error_code ec;
        std::filesystem::remove_all(home_, ec);
    }
    std::string home_;
};

// ── NetConns: read-list framing ───────────────────────────────────────────────
// Content depends on the host's sockets (may be empty if sockstat is absent) —
// what we lock down is that the reply is a well-framed ConnPage for the request.

TEST_F(ServiceRoundtrip, NetConnsListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.NetConns",
                              static_cast<uint32_t>(NetConnOpcode::ListConns),
                              0x11110001, &page, sizeof(page));
    NetConnService::handleMessage(req, p.svc);

    XBusMessage rep = recvReply(p.peer, "com.freebsdx.NetConns",
                                static_cast<uint32_t>(NetConnOpcode::ListConns), 0x11110001);
    ConnPage cp{};
    std::memcpy(&cp, rep.payload_, sizeof(cp));
    EXPECT_EQ(cp.page_, 0u);
    EXPECT_LE(cp.count_, 3u);
    if (cp.total_ <= 3) EXPECT_EQ(cp.has_more_, 0u);  // single page ⇒ no more
}

// ── Proxy: SetConfig → GetConfig round-trip (handler + framing + persistence) ──

TEST_F(ServiceRoundtrip, ProxySetThenGet) {
    ProxyConfig set{};
    set.mode_       = 2;  // Manual
    std::strcpy(set.http_host_, "proxy.test");
    set.http_port_  = 8080;
    set.https_port_ = 8443;
    std::strcpy(set.no_proxy_, "localhost,10.0.0.0/8");

    {
        auto p = makePipe();
        XBusMessage req = makeReq("com.freebsdx.Proxy",
                                  static_cast<uint32_t>(ProxyOpcode::SetConfig),
                                  0x22220001, &set, sizeof(set));
        ProxyService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.Proxy",
                  static_cast<uint32_t>(ProxyOpcode::SetConfig), 0x22220001);
    }
    {
        auto p = makePipe();
        XBusMessage req = makeReq("com.freebsdx.Proxy",
                                  static_cast<uint32_t>(ProxyOpcode::GetConfig),
                                  0x22220002);
        ProxyService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.Proxy",
                                    static_cast<uint32_t>(ProxyOpcode::GetConfig), 0x22220002);
        ProxyConfig got{};
        std::memcpy(&got, rep.payload_, sizeof(got));
        EXPECT_EQ(got.mode_, 2u);
        EXPECT_STREQ(got.http_host_, "proxy.test");   // survived persist round-trip
        EXPECT_EQ(got.http_port_, 8080);
        EXPECT_EQ(got.https_port_, 8443);
        EXPECT_STREQ(got.no_proxy_, "localhost,10.0.0.0/8");
    }
}

// ── WakeOnLan: AddTarget → ListTargets round-trip ─────────────────────────────

TEST_F(ServiceRoundtrip, WolAddThenList) {
    WakeOnLanTarget tgt{};
    std::strcpy(tgt.name_, "nas");
    std::strcpy(tgt.mac_, "AA:BB:CC:DD:EE:FF");
    {
        auto p = makePipe();
        XBusMessage req = makeReq("com.freebsdx.WakeOnLan",
                                  static_cast<uint32_t>(WakeOnLanOpcode::AddTarget),
                                  0x33330001, &tgt, sizeof(tgt));
        WakeOnLanService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.WakeOnLan",
                  static_cast<uint32_t>(WakeOnLanOpcode::AddTarget), 0x33330001);
    }
    {
        auto p = makePipe();
        uint32_t page = 0;
        XBusMessage req = makeReq("com.freebsdx.WakeOnLan",
                                  static_cast<uint32_t>(WakeOnLanOpcode::ListTargets),
                                  0x33330002, &page, sizeof(page));
        WakeOnLanService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.WakeOnLan",
                                    static_cast<uint32_t>(WakeOnLanOpcode::ListTargets), 0x33330002);
        WakeOnLanPage wp{};
        std::memcpy(&wp, rep.payload_, sizeof(wp));
        EXPECT_GE(wp.total_, 1u);
        EXPECT_EQ(wp.valid_, 1u);
        EXPECT_STREQ(wp.name_, "nas");
        EXPECT_STREQ(wp.mac_, "AA:BB:CC:DD:EE:FF");
    }
}

// ── SysInfo: GetInfo returns a well-framed SysInfoState ───────────────────────

TEST_F(ServiceRoundtrip, SysInfoGetInfo) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.SysInfo",
                              static_cast<uint32_t>(SysInfoOpcode::GetInfo), 0x55550001);
    SysInfoService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.SysInfo",
                                static_cast<uint32_t>(SysInfoOpcode::GetInfo), 0x55550001);
    SysInfoState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    // On the build host these are populated; assert the frame is sane + read.
    EXPECT_GT(st.cpu_cores_, 0u);
    EXPECT_NE(st.os_version_[0], '\0');   // e.g. "FreeBSD 15.0-…"
}

// ── MountedVolumes: ListVolumes returns a well-framed VolumePage ──────────────

TEST_F(ServiceRoundtrip, MountedVolumesList) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.MountedVolumes",
                              static_cast<uint32_t>(MountedVolOpcode::ListVolumes),
                              0x66660001, &page, sizeof(page));
    MountedVolService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.MountedVolumes",
                                static_cast<uint32_t>(MountedVolOpcode::ListVolumes), 0x66660001);
    VolumePage vp{};
    std::memcpy(&vp, rep.payload_, sizeof(vp));
    EXPECT_EQ(vp.page_, 0u);
    EXPECT_LE(vp.count_, 3u);
    EXPECT_GT(vp.total_, 0u);             // at least the root filesystem is mounted
}

// ── Process: ListTop returns a well-framed ProcessPage ────────────────────────

TEST_F(ServiceRoundtrip, ProcessListTop) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Process",
                              static_cast<uint32_t>(ProcessOpcode::ListTop),
                              0x77770001, &page, sizeof(page));
    ProcessService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Process",
                                static_cast<uint32_t>(ProcessOpcode::ListTop), 0x77770001);
    ProcessPage pp{};
    std::memcpy(&pp, rep.payload_, sizeof(pp));
    EXPECT_GT(pp.total_, 0u);              // this test process at least
    EXPECT_LE(pp.count_, 3u);
    if (pp.count_ >= 2)                    // sorted by CPU descending
        EXPECT_GE(pp.entries_[0].cpu_pct10_, pp.entries_[1].cpu_pct10_);
}

// ── USB: all three enumeration services return well-framed pages ──────────────
// Drives real usbconfig(8) on the build host; asserts framing + page invariants
// rather than a device count (CI/containers may have none).

TEST_F(ServiceRoundtrip, UsbListDevicesFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Usb",
                              static_cast<uint32_t>(UsbOpcode::ListDevices),
                              0x5b000001, &page, sizeof(page));
    UsbService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Usb",
                                static_cast<uint32_t>(UsbOpcode::ListDevices), 0x5b000001);
    UsbDevicePage dp{};
    std::memcpy(&dp, rep.payload_, sizeof(dp));
    EXPECT_EQ(dp.page_, 0u);
    EXPECT_LE(dp.count_, 1u);                             // one device per page
    EXPECT_EQ(dp.count_, dp.total_ > 0 ? 1u : 0u);
    if (dp.total_ > 1) EXPECT_EQ(dp.has_more_, 1u);
}

TEST_F(ServiceRoundtrip, UsbDevicesPageOfThree) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.UsbDevices",
                              static_cast<uint32_t>(UsbDevOpcode::ListDevices),
                              0x5b000002, &page, sizeof(page));
    UsbDevicesService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.UsbDevices",
                                static_cast<uint32_t>(UsbDevOpcode::ListDevices), 0x5b000002);
    UsbDevPage dp{};
    std::memcpy(&dp, rep.payload_, sizeof(dp));
    EXPECT_EQ(dp.page_, 0u);
    EXPECT_LE(dp.count_, 3u);                             // up to three entries per page
    EXPECT_EQ(dp.count_, std::min<uint32_t>(dp.total_, 3u));
}

TEST_F(ServiceRoundtrip, UsbDeviceDetailFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.UsbDevice",
                              static_cast<uint32_t>(UsbDeviceOpcode::ListDevices),
                              0x5b000003, &page, sizeof(page));
    UsbDeviceService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.UsbDevice",
                                static_cast<uint32_t>(UsbDeviceOpcode::ListDevices), 0x5b000003);
    UsbDeviceDetailPage dp{};
    std::memcpy(&dp, rep.payload_, sizeof(dp));
    EXPECT_EQ(dp.page_, 0u);
    EXPECT_EQ(dp.valid_, dp.total_ > 0 ? 1u : 0u);        // valid iff a device exists at page 0
}

// ── DisplayArrange: GetLayout framing + a provider feeds real outputs ─────────

TEST_F(ServiceRoundtrip, DisplayArrangeGetLayout) {
    // No provider wired in the test harness → empty layout (valid_=0).
    DisplayArrangeService::init(nullptr);
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.DisplayArrange",
                              static_cast<uint32_t>(DisplayArrangeOpcode::GetLayout),
                              0x77000001, &page, sizeof(page));
    DisplayArrangeService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.DisplayArrange",
                                static_cast<uint32_t>(DisplayArrangeOpcode::GetLayout), 0x77000001);
    DisplayOutputPage dp{};
    std::memcpy(&dp, rep.payload_, sizeof(dp));
    EXPECT_EQ(dp.total_, 0u);
    EXPECT_EQ(dp.valid_, 0u);

    // With a provider, GetLayout reports its outputs.
    DisplayArrangeService::init([]() {
        disparrange::ArrangeOutput o; o.width = 1920; o.height = 1080;
        o.primary = true; o.name = "output-0";
        return std::vector<disparrange::ArrangeOutput>{o};
    });
    DisplayArrangeService::handleMessage(req, p.svc);
    XBusMessage rep2 = recvReply(p.peer, "com.freebsdx.DisplayArrange",
                                 static_cast<uint32_t>(DisplayArrangeOpcode::GetLayout), 0x77000001);
    std::memcpy(&dp, rep2.payload_, sizeof(dp));
    EXPECT_EQ(dp.total_, 1u);
    EXPECT_EQ(dp.valid_, 1u);
    EXPECT_EQ(dp.is_primary_, 1u);
    EXPECT_STREQ(dp.name_, "output-0");
    DisplayArrangeService::init(nullptr);   // reset the static for other tests
}

TEST_F(ServiceRoundtrip, DisplayArrangeSetPrimaryAcks) {
    auto p = makePipe();
    DisplayOutputRequest req{}; req.output_id_ = 0;
    XBusMessage set = makeReq("com.freebsdx.DisplayArrange",
                              static_cast<uint32_t>(DisplayArrangeOpcode::SetPrimary),
                              0x77000002, &req, sizeof(req));
    DisplayArrangeService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.DisplayArrange",
                                static_cast<uint32_t>(DisplayArrangeOpcode::SetPrimary), 0x77000002);
    EXPECT_EQ(rep.transaction_id_, 0x77000002u);
}

// ── Backlight: GetBrightness returns a real 0–100 level ───────────────────────

TEST_F(ServiceRoundtrip, BacklightGetBrightness) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.Backlight",
                              static_cast<uint32_t>(BacklightOpcode::GetBrightness), 0x76000001);
    BacklightService::ProcessMessage(p.svc, req);   // older-style (client, msg) signature
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Backlight",
                                static_cast<uint32_t>(BacklightOpcode::GetBrightness), 0x76000001);
    BacklightState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_LE(st.level_, 100u);     // real backlight(8) level, or the 80% fallback
}

// ── Firewall: GetStatus framing + SetEnabled acks via broker-wired path ───────

TEST_F(ServiceRoundtrip, FirewallGetStatusFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.Firewall",
                              static_cast<uint32_t>(FirewallOpcode::GetStatus), 0x75000001);
    FirewallService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Firewall",
                                static_cast<uint32_t>(FirewallOpcode::GetStatus), 0x75000001);
    FirewallStatus st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_LE(st.enabled_, 1u);          // 0 unprivileged, 1 if pf readable+enabled
    EXPECT_LE(st.block_all_, 1u);
}

TEST_F(ServiceRoundtrip, FirewallSetEnabledAcks) {
    // Broker stub is unreachable → handler falls back (pfctl needs root → no-op)
    // and still acks with a FirewallStatus. Exercises the broker-wired path.
    auto p = makePipe();
    FirewallEnableRequest on{}; on.enabled_ = 1;
    XBusMessage set = makeReq("com.freebsdx.Firewall",
                              static_cast<uint32_t>(FirewallOpcode::SetEnabled),
                              0x75000002, &on, sizeof(on));
    FirewallService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Firewall",
                                static_cast<uint32_t>(FirewallOpcode::SetEnabled), 0x75000002);
    EXPECT_EQ(rep.transaction_id_, 0x75000002u);
}

// ── DnsConfig: SetConfig persists resolver; rejects an injection attempt ──────

TEST_F(ServiceRoundtrip, DnsConfigSetThenGet) {
    auto p = makePipe();
    DnsResolverConfig cfg{};
    std::strncpy(cfg.primary_,   "1.1.1.1", sizeof(cfg.primary_) - 1);
    std::strncpy(cfg.secondary_, "8.8.8.8", sizeof(cfg.secondary_) - 1);
    cfg.doh_enabled_ = 1;
    std::strncpy(cfg.doh_url_, "https://1.1.1.1/dns-query", sizeof(cfg.doh_url_) - 1);
    XBusMessage set = makeReq("com.freebsdx.DnsConfig",
                              static_cast<uint32_t>(DnsConfigOpcode::SetConfig),
                              0x74000001, &cfg, sizeof(cfg));
    DnsConfigService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.DnsConfig",
                    static_cast<uint32_t>(DnsConfigOpcode::SetConfig), 0x74000001);  // ack

    XBusMessage get = makeReq("com.freebsdx.DnsConfig",
                              static_cast<uint32_t>(DnsConfigOpcode::GetConfig), 0x74000002);
    DnsConfigService::handleMessage(get, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.DnsConfig",
                                static_cast<uint32_t>(DnsConfigOpcode::GetConfig), 0x74000002);
    DnsResolverConfig got{};
    std::memcpy(&got, rep.payload_, sizeof(got));
    EXPECT_STREQ(got.primary_, "1.1.1.1");
    EXPECT_STREQ(got.secondary_, "8.8.8.8");
    EXPECT_EQ(got.doh_enabled_, 1u);
}

TEST_F(ServiceRoundtrip, DnsConfigRejectsInjection) {
    auto p = makePipe();
    // Establish a known-good primary first (ConfigService is process-global).
    DnsResolverConfig good{}; std::strncpy(good.primary_, "9.9.9.9", sizeof(good.primary_) - 1);
    XBusMessage s1 = makeReq("com.freebsdx.DnsConfig",
                             static_cast<uint32_t>(DnsConfigOpcode::SetConfig),
                             0x74000003, &good, sizeof(good));
    DnsConfigService::handleMessage(s1, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.DnsConfig",
                    static_cast<uint32_t>(DnsConfigOpcode::SetConfig), 0x74000003);

    // A newline-injecting primary is acked but NOT persisted.
    DnsResolverConfig bad{};
    std::strncpy(bad.primary_, "1.1.1.1\nnameserver 6.6.6.6", sizeof(bad.primary_) - 1);
    XBusMessage s2 = makeReq("com.freebsdx.DnsConfig",
                             static_cast<uint32_t>(DnsConfigOpcode::SetConfig),
                             0x74000004, &bad, sizeof(bad));
    DnsConfigService::handleMessage(s2, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.DnsConfig",
                    static_cast<uint32_t>(DnsConfigOpcode::SetConfig), 0x74000004);
    EXPECT_EQ(ConfigService::get("dnsconfig.primary", ""), "9.9.9.9");   // unchanged
}

// ── ColorProfile: SetProfile persists + GetProfile reflects name; clamps ──────

TEST_F(ServiceRoundtrip, ColorProfileSetThenGet) {
    auto p = makePipe();
    ColorProfileRequest req{}; req.profile_ = 1;   // Display P3
    XBusMessage set = makeReq("com.freebsdx.ColorProfile",
                              static_cast<uint32_t>(ColorProfileOpcode::SetProfile),
                              0x73000001, &req, sizeof(req));
    ColorProfileService::handleMessage(set, p.svc);   // no reply (broadcast only)

    XBusMessage get = makeReq("com.freebsdx.ColorProfile",
                              static_cast<uint32_t>(ColorProfileOpcode::GetProfile), 0x73000002);
    ColorProfileService::handleMessage(get, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.ColorProfile",
                                static_cast<uint32_t>(ColorProfileOpcode::GetProfile), 0x73000002);
    ColorProfileState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.profile_, 1u);
    EXPECT_STREQ(st.profile_name_, "Display P3");
}

TEST_F(ServiceRoundtrip, ColorProfileClampsOutOfRange) {
    auto p = makePipe();
    ColorProfileRequest req{}; req.profile_ = 99;   // invalid → sRGB
    XBusMessage set = makeReq("com.freebsdx.ColorProfile",
                              static_cast<uint32_t>(ColorProfileOpcode::SetProfile),
                              0x73000003, &req, sizeof(req));
    ColorProfileService::handleMessage(set, p.svc);

    XBusMessage get = makeReq("com.freebsdx.ColorProfile",
                              static_cast<uint32_t>(ColorProfileOpcode::GetProfile), 0x73000004);
    ColorProfileService::handleMessage(get, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.ColorProfile",
                                static_cast<uint32_t>(ColorProfileOpcode::GetProfile), 0x73000004);
    ColorProfileState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.profile_, 0u);
    EXPECT_STREQ(st.profile_name_, "sRGB");
}

// ── Energy: ListUsage ranks real processes by CPU% ────────────────────────────

TEST_F(ServiceRoundtrip, EnergyListUsageFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Energy",
                              static_cast<uint32_t>(EnergyOpcode::ListUsage),
                              0x72000001, &page, sizeof(page));
    EnergyService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Energy",
                                static_cast<uint32_t>(EnergyOpcode::ListUsage), 0x72000001);
    EnergyPage ep{};
    std::memcpy(&ep, rep.payload_, sizeof(ep));
    EXPECT_EQ(ep.page_, 0u);
    EXPECT_LE(ep.count_, 3u);
    for (uint32_t i = 0; i < ep.count_; ++i) {
        EXPECT_NE(ep.entries_[i].app_name_[0], '\0');
        EXPECT_LE(ep.entries_[i].cpu_avg_pct_, 100u);
        EXPECT_LE(ep.entries_[i].impact_, 3u);
    }
    if (ep.count_ >= 2)                                   // ranked by CPU desc
        EXPECT_GE(ep.entries_[0].cpu_avg_pct_, ep.entries_[1].cpu_avg_pct_);
}

TEST_F(ServiceRoundtrip, AudioOutputSetActiveSinkAcks) {
    // Broker is the unreachable stub here → the handler falls back and still
    // acks. Verifies the SetActiveSink reply framing on the broker-wired path.
    auto p = makePipe();
    AudioSinkSelectRequest req{};
    std::strncpy(req.id_, "pcm1", sizeof(req.id_) - 1);
    XBusMessage set = makeReq("com.freebsdx.AudioOutput",
                              static_cast<uint32_t>(AudioOutputOpcode::SetActiveSink),
                              0x6a000002, &req, sizeof(req));
    AudioOutputService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.AudioOutput",
                                static_cast<uint32_t>(AudioOutputOpcode::SetActiveSink), 0x6a000002);
    EXPECT_EQ(rep.transaction_id_, 0x6a000002u);          // acked
}

// ── KeyboardBacklight: enabled + brightness toggle, clamps 0–100 ──────────────

TEST_F(ServiceRoundtrip, KbdBacklightEnableAndBrightnessClamp) {
    auto p = makePipe();
    KeyboardBacklightEnableRequest on{}; on.enabled_ = 1;
    XBusMessage se = makeReq("com.freebsdx.KeyboardBacklight",
                             static_cast<uint32_t>(KeyboardBacklightOpcode::SetEnabled),
                             0x71000001, &on, sizeof(on));
    KbdBacklightService::handleMessage(se, p.svc);
    XBusMessage er = recvReply(p.peer, "com.freebsdx.KeyboardBacklight",
                               static_cast<uint32_t>(KeyboardBacklightOpcode::SetEnabled), 0x71000001);
    KeyboardBacklightState st{};
    std::memcpy(&st, er.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);

    // Over-range brightness clamps to 100.
    KeyboardBacklightBrightnessRequest br{}; br.brightness_ = 150;
    XBusMessage sb = makeReq("com.freebsdx.KeyboardBacklight",
                             static_cast<uint32_t>(KeyboardBacklightOpcode::SetBrightness),
                             0x71000002, &br, sizeof(br));
    KbdBacklightService::handleMessage(sb, p.svc);
    XBusMessage brp = recvReply(p.peer, "com.freebsdx.KeyboardBacklight",
                                static_cast<uint32_t>(KeyboardBacklightOpcode::SetBrightness), 0x71000002);
    std::memcpy(&st, brp.payload_, sizeof(st));
    EXPECT_EQ(st.brightness_, 100u);

    // GetState reflects both persisted values.
    XBusMessage g = makeReq("com.freebsdx.KeyboardBacklight",
                            static_cast<uint32_t>(KeyboardBacklightOpcode::GetState), 0x71000003);
    KbdBacklightService::handleMessage(g, p.svc);
    XBusMessage gr = recvReply(p.peer, "com.freebsdx.KeyboardBacklight",
                               static_cast<uint32_t>(KeyboardBacklightOpcode::GetState), 0x71000003);
    std::memcpy(&st, gr.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);
    EXPECT_EQ(st.brightness_, 100u);
}

// ── Location: master toggle (LocationServices) + per-app list (Location) ──────

TEST_F(ServiceRoundtrip, LocationToggleAndAppList) {
    auto p = makePipe();
    // Master toggle on via LocationServices → ack reflects + persists.
    LocationEnableRequest on{}; on.enabled_ = 1;
    XBusMessage set = makeReq("com.freebsdx.LocationServices",
                              static_cast<uint32_t>(LocationOpcode::SetEnabled),
                              0x70000001, &on, sizeof(on));
    LocationService::handleMessage(set, p.svc);
    XBusMessage sr = recvReply(p.peer, "com.freebsdx.LocationServices",
                               static_cast<uint32_t>(LocationOpcode::SetEnabled), 0x70000001);
    LocationState st{};
    std::memcpy(&st, sr.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);
    EXPECT_EQ(st.requesting_count_, 0u);

    // GetState on LocationServices reflects the persisted toggle.
    XBusMessage get = makeReq("com.freebsdx.LocationServices",
                              static_cast<uint32_t>(LocationOpcode::GetState), 0x70000002);
    LocationService::handleMessage(get, p.svc);
    XBusMessage gr = recvReply(p.peer, "com.freebsdx.LocationServices",
                               static_cast<uint32_t>(LocationOpcode::GetState), 0x70000002);
    std::memcpy(&st, gr.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);

    // ListApps on Location → empty page (no app registry), reply echoes "Location".
    uint32_t page = 0;
    XBusMessage la = makeReq("com.freebsdx.Location",
                             static_cast<uint32_t>(LocationOpcode::ListApps),
                             0x70000003, &page, sizeof(page));
    LocationService::handleMessage(la, p.svc);
    XBusMessage lr = recvReply(p.peer, "com.freebsdx.Location",
                               static_cast<uint32_t>(LocationOpcode::ListApps), 0x70000003);
    LocationAppPage ap{};
    std::memcpy(&ap, lr.payload_, sizeof(ap));
    EXPECT_EQ(ap.total_, 0u);
    EXPECT_EQ(ap.valid_, 0u);
}

TEST_F(ServiceRoundtrip, LocationSetAppPolicyPersistsClamped) {
    auto p = makePipe();
    LocationAppPolicyRequest req{};
    std::strncpy(req.identifier_, "com.example.maps", sizeof(req.identifier_) - 1);
    req.precision_ = 99;   // out of range → clamps to 0 (off)
    XBusMessage sp = makeReq("com.freebsdx.Location",
                             static_cast<uint32_t>(LocationOpcode::SetAppPolicy),
                             0x70000004, &req, sizeof(req));
    LocationService::handleMessage(sp, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.Location",
                    static_cast<uint32_t>(LocationOpcode::SetAppPolicy), 0x70000004);  // ack
    EXPECT_EQ(ConfigService::get("location.app.com.example.maps", ""), "0");

    req.precision_ = 2;    // precise
    XBusMessage sp2 = makeReq("com.freebsdx.Location",
                              static_cast<uint32_t>(LocationOpcode::SetAppPolicy),
                              0x70000005, &req, sizeof(req));
    LocationService::handleMessage(sp2, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.Location",
                    static_cast<uint32_t>(LocationOpcode::SetAppPolicy), 0x70000005);
    EXPECT_EQ(ConfigService::get("location.app.com.example.maps", ""), "2");
}

// ── AdBlock: SetEnabled persists + GetStatus reflects; UpdateList stamps date ─
// SetEnabled/UpdateList have no direct reply (StatusChanged broadcast, swallowed
// by the stub) — read back via GetStatus / ConfigService.

TEST_F(ServiceRoundtrip, AdBlockToggleAndUpdate) {
    auto p = makePipe();
    AdBlockEnableRequest on{}; on.enabled_ = 1;
    XBusMessage set = makeReq("com.freebsdx.AdBlock",
                              static_cast<uint32_t>(AdBlockOpcode::SetEnabled),
                              0x6f000001, &on, sizeof(on));
    AdBlockService::handleMessage(set, p.svc);   // no reply

    XBusMessage get = makeReq("com.freebsdx.AdBlock",
                              static_cast<uint32_t>(AdBlockOpcode::GetStatus), 0x6f000002);
    AdBlockService::handleMessage(get, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.AdBlock",
                                static_cast<uint32_t>(AdBlockOpcode::GetStatus), 0x6f000002);
    AdBlockStatus st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);
    EXPECT_EQ(st.updating_, 0u);

    // UpdateList stamps a real date into list_updated_ (was "never").
    XBusMessage upd = makeReq("com.freebsdx.AdBlock",
                              static_cast<uint32_t>(AdBlockOpcode::UpdateList), 0x6f000003);
    AdBlockService::handleMessage(upd, p.svc);   // no reply
    EXPECT_NE(ConfigService::get("adblock.list_date", "never"), "never");
}

// ── Keymap: SetLayout persists a valid layout, rejects a bogus one ────────────
// SetLayout has no direct reply (confirmation is the LayoutChanged broadcast,
// swallowed by broadcast_stub here) — so assert the persisted ConfigService value.

TEST_F(ServiceRoundtrip, KeymapSetLayoutPersists) {
    auto p = makePipe();
    KeymapRequest kr{};
    std::strncpy(kr.layout_, "fr", sizeof(kr.layout_) - 1);
    std::strncpy(kr.variant_, "dvorak", sizeof(kr.variant_) - 1);
    XBusMessage req = makeReq("com.freebsdx.Keymap",
                              static_cast<uint32_t>(KeymapOpcode::SetLayout),
                              0x6b100001, &kr, sizeof(kr));
    KeymapService::handleMessage(req, p.svc);
    EXPECT_EQ(ConfigService::get("keymap.layout", "us"), "fr");
    EXPECT_EQ(ConfigService::get("keymap.variant", "x"), "dvorak");
}

TEST_F(ServiceRoundtrip, KeymapRejectsBogusLayout) {
    auto p = makePipe();
    // Establish a known-good layout first (ConfigService state is process-global).
    KeymapRequest good{}; std::strncpy(good.layout_, "de", sizeof(good.layout_) - 1);
    XBusMessage r1 = makeReq("com.freebsdx.Keymap",
                             static_cast<uint32_t>(KeymapOpcode::SetLayout),
                             0x6b100002, &good, sizeof(good));
    KeymapService::handleMessage(r1, p.svc);
    ASSERT_EQ(ConfigService::get("keymap.layout", ""), "de");

    // A bogus layout must be rejected and must NOT overwrite the good one.
    KeymapRequest bad{}; std::strncpy(bad.layout_, "us;rm", sizeof(bad.layout_) - 1);
    XBusMessage r2 = makeReq("com.freebsdx.Keymap",
                             static_cast<uint32_t>(KeymapOpcode::SetLayout),
                             0x6b100003, &bad, sizeof(bad));
    KeymapService::handleMessage(r2, p.svc);
    EXPECT_EQ(ConfigService::get("keymap.layout", ""), "de");   // unchanged
}

// ── BatteryDrain: ListDrain returns a CPU-share-ranked page from real procs ───

TEST_F(ServiceRoundtrip, BatteryDrainListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.BatteryDrain",
                              static_cast<uint32_t>(BatteryDrainOpcode::ListDrain),
                              0x6e000001, &page, sizeof(page));
    BatteryDrainService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.BatteryDrain",
                                static_cast<uint32_t>(BatteryDrainOpcode::ListDrain), 0x6e000001);
    BatteryDrainPage dp{};
    std::memcpy(&dp, rep.payload_, sizeof(dp));
    EXPECT_EQ(dp.page_, 0u);
    EXPECT_LE(dp.count_, 3u);
    for (uint32_t i = 0; i < dp.count_; ++i) {
        EXPECT_NE(dp.entries_[i].app_name_[0], '\0');   // every drainer is named
        EXPECT_LE(dp.entries_[i].energy_pct_, 100u);
        EXPECT_LE(dp.entries_[i].impact_, 2u);          // Low/Medium/High
    }
    if (dp.count_ >= 2)                                  // ranked by energy desc
        EXPECT_GE(dp.entries_[0].energy_pct_, dp.entries_[1].energy_pct_);
}

// ── FramePerf: publish()ed frames surface through GetStats ────────────────────

TEST_F(ServiceRoundtrip, FramePerfReflectsPublishedFrames) {
    FramePerfService::reset();
    // Simulate the compositor feeding frames: mostly 10 ms, two janky.
    for (int i = 0; i < 10; ++i) FramePerfService::publish(10.0f, static_cast<uint64_t>(i + 1));
    FramePerfService::publish(20.0f, 11);   // jank
    FramePerfService::publish(33.0f, 12);   // jank

    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.FramePerf",
                              static_cast<uint32_t>(FramePerfOpcode::GetStats), 0x66000001);
    FramePerfService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.FramePerf",
                                static_cast<uint32_t>(FramePerfOpcode::GetStats), 0x66000001);
    FramePerfState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.total_frames_, 12u);
    EXPECT_EQ(st.jank_count_, 2u);          // the 20 ms + 33 ms frames
    EXPECT_EQ(st.gpu_submit_us_, 33000u);   // last published frame
    EXPECT_EQ(st.recent_[15], 33);          // newest in the sparkline
    EXPECT_GT(st.avg_frame_us_, 0u);
}

TEST_F(ServiceRoundtrip, FramePerfZeroWhenNoFrames) {
    FramePerfService::reset();
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.FramePerf",
                              static_cast<uint32_t>(FramePerfOpcode::GetStats), 0x66000002);
    FramePerfService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.FramePerf",
                                static_cast<uint32_t>(FramePerfOpcode::GetStats), 0x66000002);
    FramePerfState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.total_frames_, 0u);        // headless / no compositor
    EXPECT_EQ(st.avg_frame_us_, 0u);
}

// ── GpuMonitor: GetStats returns a well-framed GpuStatsDetail with a GPU name ─

TEST_F(ServiceRoundtrip, GpuMonitorGetStatsFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.GpuMonitor",
                              static_cast<uint32_t>(GpuMonitorOpcode::GetStats), 0x67000001);
    GpuMonitorService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.GpuMonitor",
                                static_cast<uint32_t>(GpuMonitorOpcode::GetStats), 0x67000001);
    GpuStatsDetail st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_NE(st.name_[0], '\0');       // always a name (real via pciconf, or "Unknown GPU")
    EXPECT_LE(st.util_pct_, 100u);
}

// ── AudioOutput: ListSinks returns a well-framed AudioSinkPage ────────────────
// Reads real /dev/sndstat; at most one sink is active (the default).

TEST_F(ServiceRoundtrip, AudioOutputListSinksFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.AudioOutput",
                              static_cast<uint32_t>(AudioOutputOpcode::ListSinks),
                              0x6a000001, &page, sizeof(page));
    AudioOutputService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.AudioOutput",
                                static_cast<uint32_t>(AudioOutputOpcode::ListSinks), 0x6a000001);
    AudioSinkPage sp{};
    std::memcpy(&sp, rep.payload_, sizeof(sp));
    EXPECT_EQ(sp.page_, 0u);
    EXPECT_EQ(sp.count_, std::min<uint32_t>(sp.total_, 3u));
    uint32_t active = 0;
    for (uint32_t i = 0; i < sp.count_; ++i) {
        EXPECT_NE(sp.entries_[i].id_[0], '\0');     // every sink carries an id
        active += sp.entries_[i].active_ ? 1u : 0u;
    }
    EXPECT_LE(active, 1u);                           // at most one default on page 0
}

// ── BatteryHealth: real wear data + persisted optimized-charging toggle ───────

TEST_F(ServiceRoundtrip, BatteryHealthGetFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.BatteryHealth",
                              static_cast<uint32_t>(BatteryHealthOpcode::GetState), 0x6d000001);
    BatteryHealthService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.BatteryHealth",
                                static_cast<uint32_t>(BatteryHealthOpcode::GetState), 0x6d000001);
    BatteryHealthState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_LE(st.health_pct_, 100u);       // 0 if no battery; real % on a laptop
}

TEST_F(ServiceRoundtrip, BatteryHealthOptimizedChargingPersists) {
    auto p = makePipe();
    BatteryHealthRequest on{}; on.optimized_charging_ = 1;
    XBusMessage set = makeReq("com.freebsdx.BatteryHealth",
                              static_cast<uint32_t>(BatteryHealthOpcode::SetState),
                              0x6d000002, &on, sizeof(on));
    BatteryHealthService::handleMessage(set, p.svc);
    XBusMessage sr = recvReply(p.peer, "com.freebsdx.BatteryHealth",
                               static_cast<uint32_t>(BatteryHealthOpcode::SetState), 0x6d000002);
    BatteryHealthState st{};
    std::memcpy(&st, sr.payload_, sizeof(st));
    EXPECT_EQ(st.optimized_charging_, 1u);

    // Fresh GetState reflects the persisted toggle.
    XBusMessage get = makeReq("com.freebsdx.BatteryHealth",
                              static_cast<uint32_t>(BatteryHealthOpcode::GetState), 0x6d000003);
    BatteryHealthService::handleMessage(get, p.svc);
    XBusMessage gr = recvReply(p.peer, "com.freebsdx.BatteryHealth",
                               static_cast<uint32_t>(BatteryHealthOpcode::GetState), 0x6d000003);
    std::memcpy(&st, gr.payload_, sizeof(st));
    EXPECT_EQ(st.optimized_charging_, 1u);
}

// ── KernelModules: ListModules returns a well-framed KmodPage ─────────────────
// kldstat is unprivileged, so this box always has loaded modules (the kernel at
// minimum) — assert framing + that the first page carries real entries.

TEST_F(ServiceRoundtrip, KernelModulesListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.KernelModules",
                              static_cast<uint32_t>(KmodOpcode::ListModules),
                              0x6b000001, &page, sizeof(page));
    KmodService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.KernelModules",
                                static_cast<uint32_t>(KmodOpcode::ListModules), 0x6b000001);
    KmodPage kp{};
    std::memcpy(&kp, rep.payload_, sizeof(kp));
    EXPECT_EQ(kp.page_, 0u);
    EXPECT_GT(kp.total_, 0u);                              // kldstat always lists ≥1 module
    EXPECT_EQ(kp.count_, std::min<uint32_t>(kp.total_, 3u));
    EXPECT_NE(kp.entries_[0].name_[0], '\0');              // first module is named
    EXPECT_GT(kp.entries_[0].size_kb_, 0u);                // and has a size
}

// ── System-admin services (Services/Jails/ZfsPools/BootEnv/NTP) ──────────────
// Lists come from real base-system tools, so contents depend on the box; assert
// framing + per-page invariants. Mutations use the stub broker (always
// unreachable), so they must still ack on the same transaction and never act.

template <typename Page>
void expectPageInvariants(const Page& pg, uint32_t page) {
    EXPECT_EQ(pg.page_, page);
    uint32_t start = page * 3;
    uint32_t want = pg.total_ > start ? std::min<uint32_t>(pg.total_ - start, 3u) : 0u;
    EXPECT_EQ(pg.count_, want);
    EXPECT_EQ(pg.has_more_, (start + pg.count_ < pg.total_) ? 1u : 0u);
}

TEST_F(ServiceRoundtrip, ServicesListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Services",
                              static_cast<uint32_t>(ServiceOpcode::ListServices),
                              0x73000001, &page, sizeof(page));
    RcServicesService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Services",
                                static_cast<uint32_t>(ServiceOpcode::ListServices), 0x73000001);
    ServicePage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 0);
    for (uint32_t i = 0; i < pg.count_; ++i) {
        std::string state(pg.entries_[i].state_);
        EXPECT_NE(pg.entries_[i].name_[0], '\0');
        EXPECT_TRUE(state == "running" || state == "stopped") << state;
        EXPECT_EQ(state == "running", pg.entries_[i].pid_ != 0);
    }
}

TEST_F(ServiceRoundtrip, ServicesStopSessionCriticalIsAckedNotActed) {
    auto p = makePipe();
    ServiceRequest r{};
    std::strncpy(r.name_, "seatd", sizeof(r.name_) - 1);
    XBusMessage req = makeReq("com.freebsdx.Services",
                              static_cast<uint32_t>(ServiceOpcode::StopService),
                              0x73000002, &r, sizeof(r));
    RcServicesService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.Services",
              static_cast<uint32_t>(ServiceOpcode::StopService), 0x73000002);
}

TEST_F(ServiceRoundtrip, JailsListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Jails",
                              static_cast<uint32_t>(JailOpcode::ListJails),
                              0x6a000001, &page, sizeof(page));
    JailsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Jails",
                                static_cast<uint32_t>(JailOpcode::ListJails), 0x6a000001);
    JailPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 0);
}

TEST_F(ServiceRoundtrip, ZfsPoolsListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.ZfsPools",
                              static_cast<uint32_t>(ZfsOpcode::ListPools),
                              0x7a000001, &page, sizeof(page));
    ZfsPoolsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.ZfsPools",
                                static_cast<uint32_t>(ZfsOpcode::ListPools), 0x7a000001);
    ZfsPoolPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 0);
    for (uint32_t i = 0; i < pg.count_; ++i) {
        EXPECT_NE(pg.entries_[i].name_[0], '\0');
        EXPECT_NE(pg.entries_[i].health_[0], '\0');
        EXPECT_LE(pg.entries_[i].used_bytes_, pg.entries_[i].total_bytes_);
    }
}

TEST_F(ServiceRoundtrip, BootEnvListWalksAllPages) {
    // Walk every page the way the shell does; exactly one BE is booted when any
    // are listed.
    auto p = makePipe();
    uint32_t walked = 0, booted = 0, total = 0;
    for (uint32_t page = 0;; ++page) {
        uint32_t tid = 0x62000000 + page;
        XBusMessage req = makeReq("com.freebsdx.BootEnv",
                                  static_cast<uint32_t>(BeOpcode::ListBE), tid, &page, sizeof(page));
        BootEnvService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.BootEnv",
                                    static_cast<uint32_t>(BeOpcode::ListBE), tid);
        BePage pg{};
        std::memcpy(&pg, rep.payload_, sizeof(pg));
        expectPageInvariants(pg, page);
        total = pg.total_;
        for (uint32_t i = 0; i < pg.count_; ++i) {
            EXPECT_NE(pg.entries_[i].name_[0], '\0');
            booted += pg.entries_[i].active_;
        }
        walked += pg.count_;
        if (!pg.has_more_) break;
        ASSERT_LT(page, 1000u);
    }
    EXPECT_EQ(walked, total);
    if (total > 0) EXPECT_EQ(booted, 1u);
}

TEST_F(ServiceRoundtrip, NtpGetStateFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.NTP",
                              static_cast<uint32_t>(NtpOpcode::GetState), 0x6e000001, nullptr, 0);
    NtpService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.NTP",
                                static_cast<uint32_t>(NtpOpcode::GetState), 0x6e000001);
    NtpState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_LE(st.enabled_, 1u);
    EXPECT_LE(st.synced_, st.enabled_);           // can't be synced with ntpd down
    EXPECT_EQ(st.server_[sizeof(st.server_) - 1], '\0');
}

// ── Smart: ListDrives returns a well-framed SmartPage ─────────────────────────
// Drive list from real kern.disks (readable); SMART fields need smartctl+root so
// may be N/A. Asserts framing + per-page invariants, and that any listed drive
// carries a non-empty name.

TEST_F(ServiceRoundtrip, SmartListDrivesFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Smart",
                              static_cast<uint32_t>(SmartOpcode::ListDrives),
                              0x5a000001, &page, sizeof(page));
    SmartService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Smart",
                                static_cast<uint32_t>(SmartOpcode::ListDrives), 0x5a000001);
    SmartPage sp{};
    std::memcpy(&sp, rep.payload_, sizeof(sp));
    EXPECT_EQ(sp.page_, 0u);
    EXPECT_LE(sp.count_, 3u);
    EXPECT_EQ(sp.count_, std::min<uint32_t>(sp.total_, 3u));
    for (uint32_t i = 0; i < sp.count_; ++i) {
        EXPECT_NE(sp.entries_[i].name_[0], '\0');          // every listed drive is named
        EXPECT_NE(sp.entries_[i].health_[0], '\0');        // health populated ("N/A" at worst)
    }
}

// ── SysLog: GetEntries returns a well-framed page (may be empty if unreadable) ─

TEST_F(ServiceRoundtrip, SysLogGetEntries) {
    auto p = makePipe();
    SysLogRequest req_body{};
    req_body.page_ = 0;
    XBusMessage req = makeReq("com.freebsdx.SysLog",
                              static_cast<uint32_t>(SysLogOpcode::GetEntries),
                              0x88880001, &req_body, sizeof(req_body));
    SysLogService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.SysLog",
                                static_cast<uint32_t>(SysLogOpcode::GetEntries), 0x88880001);
    SysLogPageFlat pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    EXPECT_EQ(pg.page_, 0u);
    // valid_ is 1 when /var/log/messages is readable, else a well-framed empty page.
    EXPECT_TRUE(pg.valid_ == 0u || pg.valid_ == 1u);
}

// ── DataSaver: SetState → GetState persists the toggle ────────────────────────

TEST_F(ServiceRoundtrip, DataSaverSetThenGet) {
    {
        auto p = makePipe();
        DataSaverRequest req{}; req.enabled_ = 1;
        XBusMessage m = makeReq("com.freebsdx.DataSaver",
                                static_cast<uint32_t>(DataSaverOpcode::SetState),
                                0x99990001, &req, sizeof(req));
        DataSaverService::handleMessage(m, p.svc);
        recvReply(p.peer, "com.freebsdx.DataSaver",
                  static_cast<uint32_t>(DataSaverOpcode::SetState), 0x99990001);
    }
    {
        auto p = makePipe();
        XBusMessage m = makeReq("com.freebsdx.DataSaver",
                                static_cast<uint32_t>(DataSaverOpcode::GetState), 0x99990002);
        DataSaverService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.DataSaver",
                                    static_cast<uint32_t>(DataSaverOpcode::GetState), 0x99990002);
        DataSaverState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.enabled_, 1u);   // survived the persist round-trip
    }
}

// ── DoNotDisturb: SetState computes a future expiry + persists ────────────────

TEST_F(ServiceRoundtrip, DndSetTimedThenGet) {
    {
        auto p = makePipe();
        DoNotDisturbRequest req{}; req.enabled_ = 1; req.duration_ = 1;  // 1 hour
        XBusMessage m = makeReq("com.freebsdx.DoNotDisturb",
                                static_cast<uint32_t>(DoNotDisturbOpcode::SetState),
                                0xAAAA0001, &req, sizeof(req));
        DoNotDisturbService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.DoNotDisturb",
                                    static_cast<uint32_t>(DoNotDisturbOpcode::SetState), 0xAAAA0001);
        DoNotDisturbState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.enabled_, 1u);
        EXPECT_EQ(st.duration_, 1u);
        EXPECT_GT(st.ends_at_ts_, 0u);      // timed DND has a real expiry
    }
    {
        auto p = makePipe();
        XBusMessage m = makeReq("com.freebsdx.DoNotDisturb",
                                static_cast<uint32_t>(DoNotDisturbOpcode::GetState), 0xAAAA0002);
        DoNotDisturbService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.DoNotDisturb",
                                    static_cast<uint32_t>(DoNotDisturbOpcode::GetState), 0xAAAA0002);
        DoNotDisturbState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.enabled_, 1u);         // persisted, not yet expired
    }
}

// ── MicMute: SetMuted acks + persists ─────────────────────────────────────────

TEST_F(ServiceRoundtrip, MicMuteSetThenGet) {
    {
        auto p = makePipe();
        MicMuteRequest req{}; req.muted_ = 1;
        XBusMessage m = makeReq("com.freebsdx.MicMute",
                                static_cast<uint32_t>(MicMuteOpcode::SetMuted),
                                0xBBBB0001, &req, sizeof(req));
        MicMuteService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.MicMute",
                                    static_cast<uint32_t>(MicMuteOpcode::SetMuted), 0xBBBB0001);
        MicMuteState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.muted_, 1u);
    }
    {
        auto p = makePipe();
        XBusMessage m = makeReq("com.freebsdx.MicMute",
                                static_cast<uint32_t>(MicMuteOpcode::GetState), 0xBBBB0002);
        MicMuteService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.MicMute",
                                    static_cast<uint32_t>(MicMuteOpcode::GetState), 0xBBBB0002);
        MicMuteState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.muted_, 1u);   // persisted
    }
}

// ── RotationLock: SetOrientation persists both fields ─────────────────────────

TEST_F(ServiceRoundtrip, RotationLockOrientation) {
    {
        auto p = makePipe();
        RotationOrientationRequest req{}; req.orientation_ = 2;
        XBusMessage m = makeReq("com.freebsdx.RotationLock",
                                static_cast<uint32_t>(RotationLockOpcode::SetOrientation),
                                0xCCCC0001, &req, sizeof(req));
        RotationLockService::handleMessage(m, p.svc);
        recvReply(p.peer, "com.freebsdx.RotationLock",
                  static_cast<uint32_t>(RotationLockOpcode::SetOrientation), 0xCCCC0001);
    }
    {
        auto p = makePipe();
        XBusMessage m = makeReq("com.freebsdx.RotationLock",
                                static_cast<uint32_t>(RotationLockOpcode::GetState), 0xCCCC0002);
        RotationLockService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.RotationLock",
                                    static_cast<uint32_t>(RotationLockOpcode::GetState), 0xCCCC0002);
        RotationLockState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.orientation_, 2u);
    }
}

// ── WakeLock: timed lock reports a positive remaining ─────────────────────────

TEST_F(ServiceRoundtrip, WakeLockTimedGet) {
    {
        auto p = makePipe();
        WakeLockRequest req{}; req.active_ = 1; req.duration_secs_ = 3600;
        XBusMessage m = makeReq("com.freebsdx.WakeLock",
                                static_cast<uint32_t>(WakeLockOpcode::SetLock),
                                0xDDDD0001, &req, sizeof(req));
        WakeLockService::handleMessage(m, p.svc);   // no ack (broadcast-only)
    }
    {
        auto p = makePipe();
        XBusMessage m = makeReq("com.freebsdx.WakeLock",
                                static_cast<uint32_t>(WakeLockOpcode::GetState), 0xDDDD0002);
        WakeLockService::handleMessage(m, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.WakeLock",
                                    static_cast<uint32_t>(WakeLockOpcode::GetState), 0xDDDD0002);
        WakeLockState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
        EXPECT_EQ(st.active_, 1u);
        EXPECT_GT(st.remaining_secs_, 0u);
        EXPECT_LE(st.remaining_secs_, 3600u);
    }
}

// ── Accessibility: a malicious tree page must not crash/OOM (regression) ──────

TEST_F(ServiceRoundtrip, AccessibilityRejectsOversizedTree) {
    auto p = makePipe();
    // Pre-fix: total_nodes_ huge → resize ~256GB (bad_alloc in a noexcept handler
    // → terminate); node_count_ > 3 → OOB read of nodes_[]. The clamp must reject.
    AccessibilityTreePage bad{};
    bad.surface_id_  = 1;
    bad.total_nodes_ = 0xFFFFFFFFu;
    bad.page_index_  = 0;
    bad.node_count_  = 100;                 // nodes_[] is only 3
    XBusMessage m = makeReq("com.freebsdx.Accessibility",
                            static_cast<uint32_t>(AccessibilityOpcode::UpdateTree),
                            0xEEEE0001, &bad, sizeof(bad));
    AccessibilityService::ProcessMessage(p.svc, m);   // returns fast, no alloc
    SUCCEED();                                          // reaching here = no crash

    // A well-formed small tree is still accepted.
    AccessibilityTreePage ok{};
    ok.surface_id_ = 2; ok.total_nodes_ = 2; ok.page_index_ = 0; ok.node_count_ = 2;
    XBusMessage m2 = makeReq("com.freebsdx.Accessibility",
                             static_cast<uint32_t>(AccessibilityOpcode::UpdateTree),
                             0xEEEE0002, &ok, sizeof(ok));
    AccessibilityService::ProcessMessage(p.svc, m2);
    SUCCEED();
}

// ── ScrollDir drives its apply hook (init + SetNatural) ───────────────────────
// The libinput leg isn't testable headless, but the service→hook contract is:
// init() applies the persisted value once, and SetNatural() invokes it live.

TEST_F(ServiceRoundtrip, ScrollDirDrivesApplyHook) {
    auto applied = std::make_shared<std::vector<bool>>();
    ScrollDirService::init([applied](bool n) { applied->push_back(n); });
    ASSERT_EQ(applied->size(), 1u);      // startup apply of the persisted value
    EXPECT_FALSE((*applied)[0]);         // default (fresh temp HOME)

    auto p = makePipe();
    ScrollDirRequest req{}; req.natural_ = 1;
    XBusMessage m = makeReq("com.freebsdx.ScrollDir",
                            static_cast<uint32_t>(ScrollDirOpcode::SetNatural),
                            0xF0000001, &req, sizeof(req));
    ScrollDirService::handleMessage(m, p.svc);
    recvReply(p.peer, "com.freebsdx.ScrollDir",
              static_cast<uint32_t>(ScrollDirOpcode::SetNatural), 0xF0000001);   // ack
    ASSERT_EQ(applied->size(), 2u);
    EXPECT_TRUE((*applied)[1]);           // SetNatural(1) → hook(true)

    ScrollDirService::init(nullptr);      // clear the static hook for other tests
}

// ── MicMute drives its apply hook (init + SetMuted) ───────────────────────────

TEST_F(ServiceRoundtrip, MicMuteDrivesApplyHook) {
    auto applied = std::make_shared<std::vector<bool>>();
    MicMuteService::init([applied](bool m) { applied->push_back(m); });
    ASSERT_EQ(applied->size(), 1u);
    EXPECT_FALSE((*applied)[0]);

    auto p = makePipe();
    MicMuteRequest req{}; req.muted_ = 1;
    XBusMessage m = makeReq("com.freebsdx.MicMute",
                            static_cast<uint32_t>(MicMuteOpcode::SetMuted),
                            0xF1000001, &req, sizeof(req));
    MicMuteService::handleMessage(m, p.svc);
    recvReply(p.peer, "com.freebsdx.MicMute",
              static_cast<uint32_t>(MicMuteOpcode::SetMuted), 0xF1000001);
    ASSERT_EQ(applied->size(), 2u);
    EXPECT_TRUE((*applied)[1]);

    MicMuteService::init(nullptr);        // clear the static hook
}

// ── WakeLock::isActive reflects state + timed expiry (drives idle-inhibit) ─────

TEST_F(ServiceRoundtrip, WakeLockIsActiveAndExpiry) {
    EXPECT_FALSE(WakeLockService::isActive());   // nothing persisted

    auto p = makePipe();
    WakeLockRequest on{}; on.active_ = 1; on.duration_secs_ = 0;   // indefinite
    WakeLockService::handleMessage(
        makeReq("com.freebsdx.WakeLock", static_cast<uint32_t>(WakeLockOpcode::SetLock),
                0xF2000001, &on, sizeof(on)), p.svc);
    EXPECT_TRUE(WakeLockService::isActive());

    WakeLockRequest off{}; off.active_ = 0;
    WakeLockService::handleMessage(
        makeReq("com.freebsdx.WakeLock", static_cast<uint32_t>(WakeLockOpcode::SetLock),
                0xF2000002, &off, sizeof(off)), p.svc);
    EXPECT_FALSE(WakeLockService::isActive());

    // A timed lock whose end is in the past reads as inactive (auto-expiry).
    const char* home = std::getenv("HOME");
    std::ofstream(std::string(home ? home : "/tmp") + "/.freebsdx-wakelock")
        << "1 " << 1u << "\n";   // active=1, ends_ts=1 (long past)
    EXPECT_FALSE(WakeLockService::isActive());
}

// ── Unknown opcode must not reply (no crash, no spurious frame) ────────────────

TEST_F(ServiceRoundtrip, UnknownOpcodeIsSilent) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.Proxy", 0xEEEE, 0x44440001);
    ProxyService::handleMessage(req, p.svc);
    // Nothing should have been written; a non-blocking read should find no data.
    int fd = p.peer.fd();
    char buf[8];
    ssize_t n = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    EXPECT_TRUE(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        << "unexpected reply to an unknown opcode (n=" << n << ")";
}

// ── GPS: manual-config location path + framing (no device, no network) ─────────
// The reader thread (init()) is never started here, so bestFix() falls through to
// the manually-configured gps.lat/gps.lon, which the fixture isolates into a
// throwaway HOME. Each case sets exactly the config it needs.

TEST_F(ServiceRoundtrip, GpsNoFixWhenUnconfigured) {
    ConfigService::set("gps.lat", "");
    ConfigService::set("gps.lon", "");
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.GPS", GpsService::RequestLocation, 0xABCD0001);
    GpsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.GPS",
                                GpsService::RequestLocation, 0xABCD0001);
    GpsFixReply r{};
    std::memcpy(&r, rep.payload_, sizeof(r));
    EXPECT_EQ(r.has_fix_, 0) << "no device + no manual config ⇒ no fix";
}

TEST_F(ServiceRoundtrip, GpsManualLocationReported) {
    ConfigService::set("gps.lat", "48.1173");
    ConfigService::set("gps.lon", "11.5167");
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.GPS", GpsService::GetLocation, 0xABCD0002);
    GpsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.GPS",
                                GpsService::GetLocation, 0xABCD0002);
    GpsFixReply r{};
    std::memcpy(&r, rep.payload_, sizeof(r));
    ASSERT_EQ(r.has_fix_, 1);
    EXPECT_EQ(r.source_, GpsService::Manual);
    EXPECT_NEAR(r.lat_, 48.1173, 1e-4);
    EXPECT_NEAR(r.lon_, 11.5167, 1e-4);
}

TEST_F(ServiceRoundtrip, GpsRejectsOutOfRangeManual) {
    ConfigService::set("gps.lat", "999");   // impossible latitude
    ConfigService::set("gps.lon", "11.5");
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.GPS", GpsService::GetLocation, 0xABCD0003);
    GpsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.GPS",
                                GpsService::GetLocation, 0xABCD0003);
    GpsFixReply r{};
    std::memcpy(&r, rep.payload_, sizeof(r));
    EXPECT_EQ(r.has_fix_, 0) << "out-of-range latitude must be rejected";
}

// ── Timezone: GetTZ framing + SetTZ→GetTZ round-trip ──────────────────────────

TEST_F(ServiceRoundtrip, TimezoneGetFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.Timezone",
                              static_cast<uint32_t>(TimezoneOpcode::GetTZ), 0x7A000001);
    TimezoneService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Timezone",
                                static_cast<uint32_t>(TimezoneOpcode::GetTZ), 0x7A000001);
    TimezoneState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_NE(st.tz_name_[0], '\0') << "a zone name is always reported";
    EXPECT_EQ(std::strlen(st.local_time_), 8u) << "local time is HH:MM:SS";
    EXPECT_EQ(st.local_time_[2], ':');
    EXPECT_EQ(st.local_time_[5], ':');
}

TEST_F(ServiceRoundtrip, TimezoneSetUtcThenReflected) {
    if (::access("/usr/share/zoneinfo/UTC", R_OK) != 0)
        GTEST_SKIP() << "no /usr/share/zoneinfo/UTC on this host";
    TimezoneSetRequest req{};
    std::strncpy(req.tz_name_, "UTC", sizeof(req.tz_name_) - 1);
    auto p = makePipe();
    XBusMessage m = makeReq("com.freebsdx.Timezone",
                            static_cast<uint32_t>(TimezoneOpcode::SetTZ), 0x7A000002,
                            &req, sizeof(req));
    TimezoneService::handleMessage(m, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Timezone",
                                static_cast<uint32_t>(TimezoneOpcode::SetTZ), 0x7A000002);
    TimezoneState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_STREQ(st.tz_name_, "UTC");
    EXPECT_EQ(st.utc_offset_sec_, 0) << "UTC has a zero offset";
}

TEST_F(ServiceRoundtrip, TimezoneRejectsBogusZone) {
    TimezoneSetRequest req{};
    std::strncpy(req.tz_name_, "../etc/passwd", sizeof(req.tz_name_) - 1);
    auto p = makePipe();
    XBusMessage m = makeReq("com.freebsdx.Timezone",
                            static_cast<uint32_t>(TimezoneOpcode::SetTZ), 0x7A000003,
                            &req, sizeof(req));
    TimezoneService::handleMessage(m, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Timezone",
                                static_cast<uint32_t>(TimezoneOpcode::SetTZ), 0x7A000003);
    TimezoneState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_STRNE(st.tz_name_, "../etc/passwd") << "path-traversal zone must be rejected";
}

// ── DiskUsage: scan a temp tree, then page the results ────────────────────────

TEST_F(ServiceRoundtrip, DiskUsageScanSortsBySizeAndPages) {
    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / ("fbxdu_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    std::ofstream(root / "big")       << std::string(4000, 'x');   // 4000-byte file
    std::ofstream(root / "sub" / "s") << std::string(100,  'y');   // 100 bytes under sub/

    DiskUsageService::scanNow(root.string());
    EXPECT_EQ(DiskUsageService::resultCount(), 2u) << "immediate children: file 'big' + dir 'sub'";

    uint32_t page = 0;
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.DiskUsage",
                              static_cast<uint32_t>(DiskUsageOpcode::GetResults), 0xD5000001,
                              &page, sizeof(page));
    DiskUsageService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.DiskUsage",
                                static_cast<uint32_t>(DiskUsageOpcode::GetResults), 0xD5000001);
    DiskUsagePage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    EXPECT_EQ(pg.total_, 2u);
    EXPECT_EQ(pg.count_, 2u);
    EXPECT_EQ(pg.has_more_, 0u);
    ASSERT_GE(pg.count_, 2u);
    EXPECT_GT(pg.entries_[0].size_bytes_, pg.entries_[1].size_bytes_) << "sorted largest-first";
    EXPECT_GE(pg.entries_[0].size_bytes_, 4000u);
    EXPECT_EQ(pg.entries_[0].pct_of_total_, 97u);   // 4000 / 4100 = 97%

    fs::remove_all(root);
}

// ── Thermal: zone framing (real sysctl sensors when present) ──────────────────

TEST_F(ServiceRoundtrip, ThermalGetZonesFraming) {
    auto zones = ThermalService::readZones();   // whatever this host exposes
    uint32_t page = 0;
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.Thermal",
                              static_cast<uint32_t>(ThermalOpcode::GetZones), 0x74000001,
                              &page, sizeof(page));
    ThermalService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Thermal",
                                static_cast<uint32_t>(ThermalOpcode::GetZones), 0x74000001);
    ThermalZonePage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    EXPECT_EQ(pg.total_, zones.size());
    if (!zones.empty()) {
        EXPECT_EQ(pg.valid_, 1u);
        EXPECT_NE(pg.label_[0], '\0');
        EXPECT_LT(pg.temp_c_, 150u) << "a sane temperature reading";
    } else {
        EXPECT_EQ(pg.valid_, 0u) << "no sensors on this host — framed but empty";
    }
}

TEST_F(ServiceRoundtrip, ThermalSetFanModeAcks) {
    ThermalFanRequest req{}; req.zone_id_ = 0; req.fan_mode_ = 1; req.fan_pct_ = 50;
    auto p = makePipe();
    XBusMessage m = makeReq("com.freebsdx.Thermal",
                            static_cast<uint32_t>(ThermalOpcode::SetFanMode), 0x74000002,
                            &req, sizeof(req));
    ThermalService::handleMessage(m, p.svc);
    // Just needs to reply (framed) so the caller isn't left hanging.
    (void)recvReply(p.peer, "com.freebsdx.Thermal",
                    static_cast<uint32_t>(ThermalOpcode::SetFanMode), 0x74000002);
}

// ── MemPressure: live vm-sysctl memory state ──────────────────────────────────

TEST_F(ServiceRoundtrip, MemPressureReportsRealMemory) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.MemPressure",
                              static_cast<uint32_t>(MemPressureOpcode::GetPressure), 0x4D000001);
    MemPressureService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.MemPressure",
                                static_cast<uint32_t>(MemPressureOpcode::GetPressure), 0x4D000001);
    MemPressureState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_GT(st.total_kb_, 0u) << "any host has physical RAM";
    EXPECT_LE(st.free_kb_, st.total_kb_) << "free cannot exceed total";
    EXPECT_LE(st.pressure_level_, 2u);
}

// ── CursorSize / TextSize: config-backed value round-trips ────────────────────

TEST_F(ServiceRoundtrip, CursorSizeSetThenGet) {
    CursorSizeRequest req{}; req.size_ = 2;   // Large
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.CursorSize",
                              static_cast<uint32_t>(CursorSizeOpcode::SetSize), 0xC5000001,
                              &req, sizeof(req));
    CursorSizeService::handleMessage(set, p.svc);   // SetSize broadcasts, no reply

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.CursorSize",
                              static_cast<uint32_t>(CursorSizeOpcode::GetSize), 0xC5000002);
    CursorSizeService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.CursorSize",
                                static_cast<uint32_t>(CursorSizeOpcode::GetSize), 0xC5000002);
    CursorSizeState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.size_, 2u);
}

TEST_F(ServiceRoundtrip, CursorSizeAppliesThroughHook) {
    int applied = -1;
    CursorSizeService::init([&applied](uint32_t s) { applied = static_cast<int>(s); });
    EXPECT_NE(applied, -1) << "init pushes the persisted size to the hook";

    applied = -1;
    CursorSizeRequest req{}; req.size_ = 2;   // Large
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.CursorSize",
                              static_cast<uint32_t>(CursorSizeOpcode::SetSize), 0xC5000401,
                              &req, sizeof(req));
    CursorSizeService::handleMessage(set, p.svc);
    EXPECT_EQ(applied, 2) << "SetSize drives the apply hook";

    CursorSizeService::init(nullptr);   // detach (lambda captures a local by ref)
}

TEST_F(ServiceRoundtrip, CursorSizeClampsOutOfRange) {
    CursorSizeRequest req{}; req.size_ = 99;   // invalid → clamps to default (1)
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.CursorSize",
                              static_cast<uint32_t>(CursorSizeOpcode::SetSize), 0xC5000003,
                              &req, sizeof(req));
    CursorSizeService::handleMessage(set, p.svc);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.CursorSize",
                              static_cast<uint32_t>(CursorSizeOpcode::GetSize), 0xC5000004);
    CursorSizeService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.CursorSize",
                                static_cast<uint32_t>(CursorSizeOpcode::GetSize), 0xC5000004);
    CursorSizeState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_LE(st.size_, 2u) << "out-of-range size must be clamped";
}

TEST_F(ServiceRoundtrip, TextSizeSetThenGet) {
    TextSizeRequest req{}; req.size_ = 2;   // Large
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.TextSize",
                              static_cast<uint32_t>(TextSizeOpcode::SetSize), 0x75000001,
                              &req, sizeof(req));
    TextSizeService::handleMessage(set, p.svc);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.TextSize",
                              static_cast<uint32_t>(TextSizeOpcode::GetSize), 0x75000002);
    TextSizeService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.TextSize",
                                static_cast<uint32_t>(TextSizeOpcode::GetSize), 0x75000002);
    TextSizeState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.size_, 2u);
}

// ── ScreenTimeout / Pointer / ColorFilter: config-backed round-trips ──────────

TEST_F(ServiceRoundtrip, ScreenTimeoutSetThenGet) {
    ScreenTimeoutRequest req{}; req.timeout_secs_ = 600;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.ScreenTimeout",
                              static_cast<uint32_t>(ScreenTimeoutOpcode::SetTimeout), 0x53000001,
                              &req, sizeof(req));
    ScreenTimeoutService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.ScreenTimeout",
                    static_cast<uint32_t>(ScreenTimeoutOpcode::SetTimeout), 0x53000001);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.ScreenTimeout",
                              static_cast<uint32_t>(ScreenTimeoutOpcode::GetTimeout), 0x53000002);
    ScreenTimeoutService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.ScreenTimeout",
                                static_cast<uint32_t>(ScreenTimeoutOpcode::GetTimeout), 0x53000002);
    ScreenTimeoutState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.timeout_secs_, 600u);
}

TEST_F(ServiceRoundtrip, PointerSetConfigThenGet) {
    PointerConfig cfg{}; cfg.speed_ = 8; cfg.natural_scroll_ = 1; cfg.acceleration_ = 0;
    cfg.tap_to_click_ = 1;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Pointer",
                              static_cast<uint32_t>(PointerOpcode::SetConfig), 0x50000001,
                              &cfg, sizeof(cfg));
    PointerService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.Pointer",
                    static_cast<uint32_t>(PointerOpcode::SetConfig), 0x50000001);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.Pointer",
                              static_cast<uint32_t>(PointerOpcode::GetConfig), 0x50000002);
    PointerService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.Pointer",
                                static_cast<uint32_t>(PointerOpcode::GetConfig), 0x50000002);
    PointerConfig got{};
    std::memcpy(&got, rep.payload_, sizeof(got));
    EXPECT_EQ(got.speed_, 8u);
    EXPECT_EQ(got.natural_scroll_, 1u);
    EXPECT_EQ(got.acceleration_, 0u);
    EXPECT_EQ(got.tap_to_click_, 1u);
}

TEST_F(ServiceRoundtrip, PointerAppliesThroughHook) {
    uint32_t got_speed = 0; int got_accel = -1, got_tap = -1;
    PointerService::init([&](uint32_t s, bool a, bool t) {
        got_speed = s; got_accel = a ? 1 : 0; got_tap = t ? 1 : 0;
    });
    EXPECT_NE(got_accel, -1) << "init pushes the persisted config to the hook";

    got_speed = 0;
    PointerConfig cfg{}; cfg.speed_ = 7; cfg.acceleration_ = 0; cfg.tap_to_click_ = 1;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Pointer",
                              static_cast<uint32_t>(PointerOpcode::SetConfig), 0x50000401,
                              &cfg, sizeof(cfg));
    PointerService::handleMessage(set, p.svc);
    EXPECT_EQ(got_speed, 7u) << "SetConfig drives the apply hook";
    EXPECT_EQ(got_accel, 0);
    EXPECT_EQ(got_tap, 1);

    PointerService::init(nullptr);   // detach (lambda captures locals by ref)
}

TEST_F(ServiceRoundtrip, PointerClampsSpeed) {
    PointerConfig cfg{}; cfg.speed_ = 99;   // out of 1..10
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Pointer",
                              static_cast<uint32_t>(PointerOpcode::SetConfig), 0x50000003,
                              &cfg, sizeof(cfg));
    PointerService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Pointer",
                                static_cast<uint32_t>(PointerOpcode::SetConfig), 0x50000003);
    PointerConfig got{};
    std::memcpy(&got, rep.payload_, sizeof(got));
    EXPECT_LE(got.speed_, 10u);
    EXPECT_GE(got.speed_, 1u);
}

TEST_F(ServiceRoundtrip, ColorFilterComputeMatrix) {
    // Disabled → identity.
    ColorFilterState off{}; off.enabled_ = 0; off.type_ = 1; off.intensity_ = 100;
    auto mI = ColorFilterService::computeMatrix(off);
    EXPECT_DOUBLE_EQ(mI[0], 1.0); EXPECT_DOUBLE_EQ(mI[4], 1.0); EXPECT_DOUBLE_EQ(mI[8], 1.0);
    EXPECT_DOUBLE_EQ(mI[1], 0.0);

    // Enabled but 0% intensity → still identity (lerp t=0).
    ColorFilterState zero{}; zero.enabled_ = 1; zero.type_ = 2; zero.intensity_ = 0;
    auto mZ = ColorFilterService::computeMatrix(zero);
    EXPECT_DOUBLE_EQ(mZ[0], 1.0); EXPECT_DOUBLE_EQ(mZ[1], 0.0);

    // Monochrome at 100% → every row is the Rec.601 luma triple.
    ColorFilterState mono{}; mono.enabled_ = 1; mono.monochrome_ = 1; mono.intensity_ = 100;
    auto mM = ColorFilterService::computeMatrix(mono);
    for (int row = 0; row < 3; ++row) {
        EXPECT_NEAR(mM[row*3+0], 0.299, 1e-9);
        EXPECT_NEAR(mM[row*3+1], 0.587, 1e-9);
        EXPECT_NEAR(mM[row*3+2], 0.114, 1e-9);
    }

    // Deuteranopia at 50% → halfway between identity and the filter.
    ColorFilterState deut{}; deut.enabled_ = 1; deut.type_ = 1; deut.intensity_ = 50;
    auto mD = ColorFilterService::computeMatrix(deut);
    EXPECT_NEAR(mD[0], 1.0 * 0.5 + 0.625 * 0.5, 1e-9);   // 0.8125
    EXPECT_NEAR(mD[1], 0.0 * 0.5 + 0.375 * 0.5, 1e-9);   // 0.1875
}

TEST_F(ServiceRoundtrip, ColorFilterAppliesMatrixThroughHook) {
    std::array<double, 9> got{};
    bool fired = false;
    ColorFilterService::init([&](const std::array<double, 9>& m) { got = m; fired = true; });
    EXPECT_TRUE(fired) << "init pushes the current matrix";

    // Enable + monochrome → hook receives a non-identity (luma) matrix.
    fired = false;
    ColorFilterEnableRequest er{}; er.enabled_ = 1;
    auto p = makePipe();
    XBusMessage en = makeReq("com.freebsdx.ColorFilter",
                             static_cast<uint32_t>(ColorFilterOpcode::SetEnabled), 0xCF000401,
                             &er, sizeof(er));
    ColorFilterService::handleMessage(en, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.ColorFilter",
                    static_cast<uint32_t>(ColorFilterOpcode::SetEnabled), 0xCF000401);
    ColorFilterToggleRequest mr{}; mr.enabled_ = 1;
    auto p2 = makePipe();
    XBusMessage mo = makeReq("com.freebsdx.ColorFilter",
                             static_cast<uint32_t>(ColorFilterOpcode::SetMonochrome), 0xCF000402,
                             &mr, sizeof(mr));
    ColorFilterService::handleMessage(mo, p2.svc);
    (void)recvReply(p2.peer, "com.freebsdx.ColorFilter",
                    static_cast<uint32_t>(ColorFilterOpcode::SetMonochrome), 0xCF000402);
    EXPECT_TRUE(fired) << "a setter drives the apply hook";
    EXPECT_NEAR(got[0], 0.299, 1e-9) << "monochrome matrix reached the hook";

    ColorFilterService::init(nullptr);   // detach (lambda captures locals by ref)
}

TEST_F(ServiceRoundtrip, NightLightComputeMatrix) {
    // Disabled → identity regardless of temperature.
    NightLightState off{}; off.enabled_ = 0; off.temperature_k_ = 2700;
    auto mI = NightLightService::computeMatrix(off);
    EXPECT_DOUBLE_EQ(mI[0], 1.0); EXPECT_DOUBLE_EQ(mI[4], 1.0); EXPECT_DOUBLE_EQ(mI[8], 1.0);
    EXPECT_DOUBLE_EQ(mI[1], 0.0);

    // Enabled at 6500K (neutral) → ≈ identity.
    NightLightState neutral{}; neutral.enabled_ = 1; neutral.temperature_k_ = 6500;
    auto mN = NightLightService::computeMatrix(neutral);
    EXPECT_NEAR(mN[0], 1.0, 1e-9);
    EXPECT_NEAR(mN[4], 1.0, 1e-9);
    EXPECT_NEAR(mN[8], 1.0, 1e-9);

    // Enabled at 2700K (warmest) → red full, green/blue pulled down.
    NightLightState warm{}; warm.enabled_ = 1; warm.temperature_k_ = 2700;
    auto mW = NightLightService::computeMatrix(warm);
    EXPECT_NEAR(mW[0], 1.0,  1e-9);   // red untouched
    EXPECT_NEAR(mW[4], 0.78, 1e-9);   // green reduced
    EXPECT_NEAR(mW[8], 0.54, 1e-9);   // blue reduced most
    // Diagonal only (no cross-channel bleed).
    EXPECT_DOUBLE_EQ(mW[1], 0.0); EXPECT_DOUBLE_EQ(mW[2], 0.0);

    // Midpoint (4600K) → blue strictly between its 2700K and 6500K values.
    NightLightState mid{}; mid.enabled_ = 1; mid.temperature_k_ = 4600;
    auto mM = NightLightService::computeMatrix(mid);
    EXPECT_GT(mM[8], 0.54);
    EXPECT_LT(mM[8], 1.0);
    EXPECT_GT(mM[8], mW[8]) << "warmer temp = less blue";
}

TEST_F(ServiceRoundtrip, NightLightAppliesTintThroughHook) {
    std::array<double, 9> got{};
    bool fired = false;
    NightLightService::init([&](const std::array<double, 9>& m) { got = m; fired = true; });
    EXPECT_TRUE(fired) << "init pushes the current matrix";

    // Enable → hook fires; then set a warm temperature → blue drops below 1.0.
    fired = false;
    NightLightEnableRequest er{}; er.enabled_ = 1;
    auto p = makePipe();
    XBusMessage en = makeReq("com.freebsdx.NightLight",
                             static_cast<uint32_t>(NightLightOpcode::SetEnabled), 0x4E000401,
                             &er, sizeof(er));
    NightLightService::handleMessage(en, p.svc);
    EXPECT_TRUE(fired) << "enabling drives the apply hook";

    fired = false;
    NightLightTempRequest tr{}; tr.temperature_k_ = 2700;
    auto p2 = makePipe();
    XBusMessage tm = makeReq("com.freebsdx.NightLight",
                             static_cast<uint32_t>(NightLightOpcode::SetTemperature), 0x4E000402,
                             &tr, sizeof(tr));
    NightLightService::handleMessage(tm, p2.svc);
    EXPECT_TRUE(fired) << "a temperature change drives the apply hook";
    EXPECT_NEAR(got[0], 1.0,  1e-9) << "red stays full";
    EXPECT_NEAR(got[8], 0.54, 1e-9) << "warm tint reached the hook (blue reduced)";

    NightLightService::init(nullptr);   // detach (lambda captures locals by ref)
}

TEST_F(ServiceRoundtrip, ColorFilterSetTypeReflectedInState) {
    ColorFilterTypeRequest tr{}; tr.type_ = 1;   // Deuteranopia
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.ColorFilter",
                              static_cast<uint32_t>(ColorFilterOpcode::SetType), 0xCF000001,
                              &tr, sizeof(tr));
    ColorFilterService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.ColorFilter",
                    static_cast<uint32_t>(ColorFilterOpcode::SetType), 0xCF000001);

    ColorFilterEnableRequest er{}; er.enabled_ = 1;
    auto p2 = makePipe();
    XBusMessage en = makeReq("com.freebsdx.ColorFilter",
                             static_cast<uint32_t>(ColorFilterOpcode::SetEnabled), 0xCF000002,
                             &er, sizeof(er));
    ColorFilterService::handleMessage(en, p2.svc);
    (void)recvReply(p2.peer, "com.freebsdx.ColorFilter",
                    static_cast<uint32_t>(ColorFilterOpcode::SetEnabled), 0xCF000002);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.ColorFilter",
                              static_cast<uint32_t>(ColorFilterOpcode::GetState), 0xCF000003);
    ColorFilterService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.ColorFilter",
                                static_cast<uint32_t>(ColorFilterOpcode::GetState), 0xCF000003);
    ColorFilterState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);
    EXPECT_EQ(st.type_, 1u);
}

// ── Touchpad / DisplayScale / SystemSounds / Privacy: config-backed ───────────

TEST_F(ServiceRoundtrip, TouchpadToggleRoundTrip) {
    TouchpadRequest req{}; req.enabled_ = 0;   // disable
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Touchpad",
                              static_cast<uint32_t>(TouchpadOpcode::SetEnabled), 0x54000001,
                              &req, sizeof(req));
    TouchpadService::handleMessage(set, p.svc);   // no reply (broadcast only)

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.Touchpad",
                              static_cast<uint32_t>(TouchpadOpcode::GetState), 0x54000002);
    TouchpadService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.Touchpad",
                                static_cast<uint32_t>(TouchpadOpcode::GetState), 0x54000002);
    TouchpadState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 0u);
}

TEST_F(ServiceRoundtrip, TouchpadAppliesThroughHook) {
    // init() pushes the persisted state immediately; SetEnabled re-applies live.
    int applied = -1;
    TouchpadService::init([&applied](bool en) { applied = en ? 1 : 0; });
    EXPECT_NE(applied, -1) << "init pushes the current persisted value to the hook";

    applied = -1;
    TouchpadRequest req{}; req.enabled_ = 1;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Touchpad",
                              static_cast<uint32_t>(TouchpadOpcode::SetEnabled), 0x54000201,
                              &req, sizeof(req));
    TouchpadService::handleMessage(set, p.svc);
    EXPECT_EQ(applied, 1) << "SetEnabled drives the apply hook";

    TouchpadService::init(nullptr);   // detach: the lambda captures a local by ref
}

TEST_F(ServiceRoundtrip, DisplayScaleAppliesThroughHook) {
    int applied = -1;
    DisplayScaleService::init([&applied](uint32_t pct) { applied = static_cast<int>(pct); });
    EXPECT_NE(applied, -1) << "init pushes the persisted scale to the hook";

    applied = -1;
    DisplayScalePctRequest req{}; req.scale_pct_ = 200;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.DisplayScale",
                              static_cast<uint32_t>(DisplayScaleOpcode::SetScale), 0x44000401,
                              &req, sizeof(req));
    DisplayScaleService::handleMessage(set, p.svc);
    EXPECT_EQ(applied, 200) << "SetScale drives the apply hook";

    // Invalid value is clamped before the hook sees it.
    applied = -1;
    DisplayScalePctRequest bad{}; bad.scale_pct_ = 137;
    auto p2 = makePipe();
    XBusMessage set2 = makeReq("com.freebsdx.DisplayScale",
                               static_cast<uint32_t>(DisplayScaleOpcode::SetScale), 0x44000402,
                               &bad, sizeof(bad));
    DisplayScaleService::handleMessage(set2, p2.svc);
    EXPECT_EQ(applied, 100) << "invalid scale clamps to 100 before applying";

    DisplayScaleService::init(nullptr);   // detach (lambda captures a local by ref)
}

TEST_F(ServiceRoundtrip, DisplayScaleValidatesAllowedValues) {
    DisplayScalePctRequest bad{}; bad.scale_pct_ = 137;   // not an allowed step
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.DisplayScale",
                              static_cast<uint32_t>(DisplayScaleOpcode::SetScale), 0x44000001,
                              &bad, sizeof(bad));
    DisplayScaleService::handleMessage(set, p.svc);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.DisplayScale",
                              static_cast<uint32_t>(DisplayScaleOpcode::GetScale), 0x44000002);
    DisplayScaleService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.DisplayScale",
                                static_cast<uint32_t>(DisplayScaleOpcode::GetScale), 0x44000002);
    DisplayScaleState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.scale_pct_, 100u) << "invalid scale falls back to 100%";

    DisplayScalePctRequest ok{}; ok.scale_pct_ = 200;
    auto p2 = makePipe();
    XBusMessage set2 = makeReq("com.freebsdx.DisplayScale",
                               static_cast<uint32_t>(DisplayScaleOpcode::SetScale), 0x44000003,
                               &ok, sizeof(ok));
    DisplayScaleService::handleMessage(set2, p2.svc);
    auto q2 = makePipe();
    XBusMessage g2 = makeReq("com.freebsdx.DisplayScale",
                             static_cast<uint32_t>(DisplayScaleOpcode::GetScale), 0x44000004);
    DisplayScaleService::handleMessage(g2, q2.svc);
    XBusMessage r2 = recvReply(q2.peer, "com.freebsdx.DisplayScale",
                               static_cast<uint32_t>(DisplayScaleOpcode::GetScale), 0x44000004);
    std::memcpy(&st, r2.payload_, sizeof(st));
    EXPECT_EQ(st.scale_pct_, 200u);
}

TEST_F(ServiceRoundtrip, SystemSoundsSetThenGet) {
    SystemSoundsRequest req{}; req.enabled_ = 1; req.volume_pct_ = 55;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.SystemSounds",
                              static_cast<uint32_t>(SystemSoundsOpcode::SetState), 0x55000001,
                              &req, sizeof(req));
    SystemSoundsService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.SystemSounds",
                    static_cast<uint32_t>(SystemSoundsOpcode::SetState), 0x55000001);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.SystemSounds",
                              static_cast<uint32_t>(SystemSoundsOpcode::GetState), 0x55000002);
    SystemSoundsService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.SystemSounds",
                                static_cast<uint32_t>(SystemSoundsOpcode::GetState), 0x55000002);
    SystemSoundsState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);
    EXPECT_EQ(st.volume_pct_, 55u);
}

TEST_F(ServiceRoundtrip, PrivacyBlockCameraReflected) {
    PrivacyDeviceRequest req{}; req.blocked_ = 1;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Privacy",
                              static_cast<uint32_t>(PrivacyOpcode::SetCameraBlocked), 0x50000011,
                              &req, sizeof(req));
    PrivacyService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Privacy",
                                static_cast<uint32_t>(PrivacyOpcode::SetCameraBlocked), 0x50000011);
    PrivacyStatus st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.camera_blocked_, 1u);
    EXPECT_TRUE(PrivacyService::cameraBlocked());
}

// Enforcement: a privacy camera block must deny CameraService::openDevice.
TEST_F(ServiceRoundtrip, PrivacyBlockDeniesCameraOpen) {
    CameraOpenRequest req{};
    std::strncpy(req.path_, "/dev/video0", sizeof(req.path_) - 1);

    ConfigService::set("privacy.camera_blocked", "1");
    EXPECT_EQ(CameraService::openDevice(req).open_, 0u) << "blocked → camera denied";

    ConfigService::set("privacy.camera_blocked", "0");
    EXPECT_EQ(CameraService::openDevice(req).open_, 1u) << "unblocked → camera opens";
    CameraService::closeDevice();
}

// ── FocusMode / AutoBrightness / RefreshRate ──────────────────────────────────

TEST_F(ServiceRoundtrip, FocusModeStartStopRoundTrip) {
    FocusStartRequest req{}; req.duration_secs_ = 1500; req.category_ = 0;   // 25 min, Work
    auto p = makePipe();
    XBusMessage start = makeReq("com.freebsdx.FocusMode",
                                static_cast<uint32_t>(FocusModeOpcode::Start), 0x46000001,
                                &req, sizeof(req));
    FocusModeService::handleMessage(start, p.svc);   // no reply

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.FocusMode",
                              static_cast<uint32_t>(FocusModeOpcode::GetState), 0x46000002);
    FocusModeService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.FocusMode",
                                static_cast<uint32_t>(FocusModeOpcode::GetState), 0x46000002);
    FocusModeState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.active_, 1u);
    EXPECT_EQ(st.category_, 0u);
    EXPECT_GT(st.remaining_secs_, 0u);
    EXPECT_LE(st.remaining_secs_, 1500u);

    auto r = makePipe();
    XBusMessage stop = makeReq("com.freebsdx.FocusMode",
                               static_cast<uint32_t>(FocusModeOpcode::Stop), 0x46000003);
    FocusModeService::handleMessage(stop, r.svc);
    auto s = makePipe();
    XBusMessage get2 = makeReq("com.freebsdx.FocusMode",
                               static_cast<uint32_t>(FocusModeOpcode::GetState), 0x46000004);
    FocusModeService::handleMessage(get2, s.svc);
    XBusMessage rep2 = recvReply(s.peer, "com.freebsdx.FocusMode",
                                 static_cast<uint32_t>(FocusModeOpcode::GetState), 0x46000004);
    std::memcpy(&st, rep2.payload_, sizeof(st));
    EXPECT_EQ(st.active_, 0u) << "Stop ends the session";
}

TEST_F(ServiceRoundtrip, FocusModeExpiredSessionAutoClears) {
    // Simulate a session that already ended (end_ts in the past).
    ConfigService::set("focus.active", "1");
    ConfigService::set("focus.end_ts", "1");   // 1970 — long past
    auto st = FocusModeService::currentState();
    EXPECT_EQ(st.active_, 0u) << "a timed session past its end auto-expires";
}

TEST_F(ServiceRoundtrip, AutoBrightnessToggleAndNoSensor) {
    AutoBrightnessRequest req{}; req.enabled_ = 1;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.AutoBrightness",
                              static_cast<uint32_t>(AutoBrightnessOpcode::SetEnabled), 0x41000001,
                              &req, sizeof(req));
    AutoBrightnessService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.AutoBrightness",
                                static_cast<uint32_t>(AutoBrightnessOpcode::SetEnabled), 0x41000001);
    AutoBrightnessState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.enabled_, 1u);
    EXPECT_EQ(st.lux_, UINT32_MAX) << "no ambient-light sensor";
}

TEST_F(ServiceRoundtrip, RefreshRateReportsSupportedAndRejectsUnsupported) {
    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.RefreshRate",
                              static_cast<uint32_t>(RefreshRateOpcode::GetRate), 0x52000001);
    RefreshRateService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.RefreshRate",
                                static_cast<uint32_t>(RefreshRateOpcode::GetRate), 0x52000001);
    RefreshRateState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_NE(st.supported_mask_ & 0x1u, 0u) << "60 Hz is always supported";
    EXPECT_EQ(st.current_hz_, 60u);

    // Attempt an unsupported rate → stays at a supported one.
    RefreshRateRequest req{}; req.hz_ = 240;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.RefreshRate",
                              static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000002,
                              &req, sizeof(req));
    RefreshRateService::handleMessage(set, p.svc);
    XBusMessage sr = recvReply(p.peer, "com.freebsdx.RefreshRate",
                               static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000002);
    std::memcpy(&st, sr.payload_, sizeof(st));
    EXPECT_EQ(st.current_hz_, 60u) << "unsupported 240 Hz rejected, stays 60";
}

TEST_F(ServiceRoundtrip, RefreshRateAppliesSupportedViaHook) {
    // Advertise 60+120 and record the modeset hook.
    int applied_hz = 0;
    RefreshRateService::init(0b0011u, [&applied_hz](uint32_t hz) { applied_hz = hz; return true; });

    // 120 is supported → hook fires, state reflects it.
    RefreshRateRequest req{}; req.hz_ = 120;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.RefreshRate",
                              static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000101,
                              &req, sizeof(req));
    RefreshRateService::handleMessage(set, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.RefreshRate",
                                static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000101);
    RefreshRateState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(applied_hz, 120) << "supported rate drives the modeset hook";
    EXPECT_EQ(st.current_hz_, 120u);

    // 240 not in the mask → hook not called, stays 120.
    applied_hz = 0;
    RefreshRateRequest bad{}; bad.hz_ = 240;
    auto p2 = makePipe();
    XBusMessage set2 = makeReq("com.freebsdx.RefreshRate",
                               static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000102,
                               &bad, sizeof(bad));
    RefreshRateService::handleMessage(set2, p2.svc);
    (void)recvReply(p2.peer, "com.freebsdx.RefreshRate",
                    static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000102);
    EXPECT_EQ(applied_hz, 0) << "unsupported rate never reaches the hook";

    // A failing modeset (hook returns false) must not persist the change.
    RefreshRateService::init(0b0011u, [](uint32_t) { return false; });
    RefreshRateRequest r3{}; r3.hz_ = 60;
    auto p3 = makePipe();
    XBusMessage set3 = makeReq("com.freebsdx.RefreshRate",
                               static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000103,
                               &r3, sizeof(r3));
    RefreshRateService::handleMessage(set3, p3.svc);
    XBusMessage rep3 = recvReply(p3.peer, "com.freebsdx.RefreshRate",
                                 static_cast<uint32_t>(RefreshRateOpcode::SetRate), 0x52000103);
    std::memcpy(&st, rep3.payload_, sizeof(st));
    EXPECT_EQ(st.current_hz_, 120u) << "failed modeset keeps the previous rate";

    RefreshRateService::init(0x1u, nullptr);   // reset to default for other tests
}

// ── Dock / Gesture / AccessibilitySettings ────────────────────────────────────

TEST_F(ServiceRoundtrip, DockSetThenGetValidates) {
    DockSettings s{}; s.position_ = 1; s.autohide_ = 1; s.icon_size_ = 100;  // 100 invalid
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Dock",
                              static_cast<uint32_t>(DockOpcode::SetSettings), 0x44000101, &s, sizeof(s));
    DockService::handleMessage(set, p.svc);   // no reply (broadcast only)

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.Dock",
                              static_cast<uint32_t>(DockOpcode::GetSettings), 0x44000102);
    DockService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.Dock",
                                static_cast<uint32_t>(DockOpcode::GetSettings), 0x44000102);
    DockSettings got{}; std::memcpy(&got, rep.payload_, sizeof(got));
    EXPECT_EQ(got.position_, 1u);
    EXPECT_EQ(got.autohide_, 1u);
    EXPECT_EQ(got.icon_size_, 48u) << "invalid icon size falls back to 48";
}

TEST_F(ServiceRoundtrip, GestureSetConfigThenGet) {
    GestureConfig c{}; c.enabled_ = 1; c.swipe3_sensitivity_ = 200; c.pinch_sensitivity_ = 70;
    c.natural_scroll_ = 1;
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.Gesture",
                              static_cast<uint32_t>(GestureOpcode::SetConfig), 0x47000101, &c, sizeof(c));
    GestureService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.Gesture",
                    static_cast<uint32_t>(GestureOpcode::SetConfig), 0x47000101);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.Gesture",
                              static_cast<uint32_t>(GestureOpcode::GetConfig), 0x47000102);
    GestureService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.Gesture",
                                static_cast<uint32_t>(GestureOpcode::GetConfig), 0x47000102);
    GestureConfig got{}; std::memcpy(&got, rep.payload_, sizeof(got));
    EXPECT_EQ(got.enabled_, 1u);
    EXPECT_EQ(got.swipe3_sensitivity_, 100u) << "sensitivity clamps to 100";
    EXPECT_EQ(got.pinch_sensitivity_, 70u);
    EXPECT_EQ(got.natural_scroll_, 1u);
}

TEST_F(ServiceRoundtrip, A11ySettingsPerFieldSettersAndNameFits) {
    // The wire name must fit the 32-byte field (this is the bug this service works
    // around): a full STREQ in recvReply would fail if it were truncated.
    ASSERT_LT(std::strlen("com.freebsdx.A11ySettings"), sizeof(XBusMessage{}.service_name_));

    AccessibilityCursorRequest cr{}; cr.scale_ = 5.0f;   // out of 1..3 → clamps to 3
    auto p = makePipe();
    XBusMessage sc = makeReq("com.freebsdx.A11ySettings",
                             static_cast<uint32_t>(AccessibilitySettingsOpcode::SetCursorScale),
                             0x41000201, &cr, sizeof(cr));
    A11ySettingsService::handleMessage(sc, p.svc);   // no reply

    AccessibilityToggleRequest tg{}; tg.enabled_ = 1;
    auto p2 = makePipe();
    XBusMessage hc = makeReq("com.freebsdx.A11ySettings",
                             static_cast<uint32_t>(AccessibilitySettingsOpcode::SetHighContrast),
                             0x41000202, &tg, sizeof(tg));
    A11ySettingsService::handleMessage(hc, p2.svc);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.A11ySettings",
                              static_cast<uint32_t>(AccessibilitySettingsOpcode::GetSettings), 0x41000203);
    A11ySettingsService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.A11ySettings",
                                static_cast<uint32_t>(AccessibilitySettingsOpcode::GetSettings), 0x41000203);
    AccessibilityQuickSettings st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_FLOAT_EQ(st.cursor_scale_, 3.0f) << "cursor scale clamped to 3.0";
    EXPECT_EQ(st.high_contrast_, 1u);
}

// ── UpdateChannel / SpatialAudio / Stylus ─────────────────────────────────────

TEST_F(ServiceRoundtrip, UpdateChannelSetThenGet) {
    UpdateChannelRequest req{}; req.channel_ = 1;   // Beta
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.UpdateChannel",
                              static_cast<uint32_t>(UpdateChannelOpcode::SetChannel), 0x55000201,
                              &req, sizeof(req));
    UpdateChannelService::handleMessage(set, p.svc);   // broadcast only

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.UpdateChannel",
                              static_cast<uint32_t>(UpdateChannelOpcode::GetChannel), 0x55000202);
    UpdateChannelService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.UpdateChannel",
                                static_cast<uint32_t>(UpdateChannelOpcode::GetChannel), 0x55000202);
    UpdateChannelState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.channel_, 1u);
    EXPECT_STREQ(st.channel_name_, "Beta");
}

TEST_F(ServiceRoundtrip, SpatialAudioSetThenGet) {
    SpatialAudioConfig cfg{}; cfg.enabled_ = 1; cfg.mode_ = 9;   // 9 clamps to 2
    auto p = makePipe();
    XBusMessage set = makeReq("com.freebsdx.SpatialAudio",
                              static_cast<uint32_t>(SpatialAudioOpcode::SetConfig), 0x54000201,
                              &cfg, sizeof(cfg));
    SpatialAudioService::handleMessage(set, p.svc);
    (void)recvReply(p.peer, "com.freebsdx.SpatialAudio",
                    static_cast<uint32_t>(SpatialAudioOpcode::SetConfig), 0x54000201);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.SpatialAudio",
                              static_cast<uint32_t>(SpatialAudioOpcode::GetConfig), 0x54000202);
    SpatialAudioService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.SpatialAudio",
                                static_cast<uint32_t>(SpatialAudioOpcode::GetConfig), 0x54000202);
    SpatialAudioConfig got{}; std::memcpy(&got, rep.payload_, sizeof(got));
    EXPECT_EQ(got.enabled_, 1u);
    EXPECT_EQ(got.mode_, 2u) << "mode clamps to 2";
    EXPECT_NE(got.device_[0], '\0');
}

TEST_F(ServiceRoundtrip, StylusPalmRejectAndPressure) {
    StylusRequest pr{}; pr.palm_rejection_ = 0;
    auto p = makePipe();
    XBusMessage sp = makeReq("com.freebsdx.Stylus",
                             static_cast<uint32_t>(StylusOpcode::SetPalmReject), 0x53000201,
                             &pr, sizeof(pr));
    StylusService::handleMessage(sp, p.svc);

    StylusRequest cr{}; cr.pressure_curve_ = 2;
    auto p2 = makePipe();
    XBusMessage sc = makeReq("com.freebsdx.Stylus",
                             static_cast<uint32_t>(StylusOpcode::SetPressure), 0x53000202,
                             &cr, sizeof(cr));
    StylusService::handleMessage(sc, p2.svc);

    auto q = makePipe();
    XBusMessage get = makeReq("com.freebsdx.Stylus",
                              static_cast<uint32_t>(StylusOpcode::GetState), 0x53000203);
    StylusService::handleMessage(get, q.svc);
    XBusMessage rep = recvReply(q.peer, "com.freebsdx.Stylus",
                                static_cast<uint32_t>(StylusOpcode::GetState), 0x53000203);
    StylusState st{}; std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_EQ(st.palm_rejection_, 0u);
    EXPECT_EQ(st.pressure_curve_, 2u);
    EXPECT_EQ(st.connected_, 0u) << "no digitiser detected on this host";
}

// ── Per-user services (CronJobs/SshKeys/Captures/EnvVars) ─────────────────────

void writeHomeFile(const std::string& path, const std::string& body) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream(path) << body;
}

std::string readHomeFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Reads the real user's crontab (never writes it): framing only.
TEST_F(ServiceRoundtrip, CronListFramingAndStaleToggleAcked) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.CronJobs", static_cast<uint32_t>(CronOpcode::ListJobs),
                              0x63000001, &page, sizeof(page));
    CronJobsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.CronJobs",
                                static_cast<uint32_t>(CronOpcode::ListJobs), 0x63000001);
    CronPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 0);

    // Line 0 is never a job → no crontab install, but still acked.
    CronJobToggleRequest t{};
    t.job_id_ = 0;
    req = makeReq("com.freebsdx.CronJobs", static_cast<uint32_t>(CronOpcode::SetJobEnabled),
                  0x63000002, &t, sizeof(t));
    CronJobsService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.CronJobs",
              static_cast<uint32_t>(CronOpcode::SetJobEnabled), 0x63000002);
}

TEST_F(ServiceRoundtrip, SshKeysListThenRemove) {
    writeHomeFile(home_ + "/.ssh/authorized_keys",
                  "# c\nssh-ed25519 AAAA one@a\nssh-rsa BBBB two@b\nssh-ed25519 CCCC three@c\n"
                  "ssh-ed25519 DDDD four@d\n");
    writeHomeFile(home_ + "/.ssh/known_hosts", "h1 ssh-ed25519 X\nh2 ssh-rsa Y\n");
    auto p = makePipe();
    uint32_t page = 1;
    XBusMessage req = makeReq("com.freebsdx.SshKeys", static_cast<uint32_t>(SshKeyOpcode::ListKeys),
                              0x6b000001, &page, sizeof(page));
    SshKeysService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.SshKeys",
                                static_cast<uint32_t>(SshKeyOpcode::ListKeys), 0x6b000001);
    SshKeyPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 1);
    EXPECT_EQ(pg.total_, 4u);
    ASSERT_EQ(pg.count_, 1u);
    EXPECT_STREQ(pg.entries_[0].comment_, "four@d");
    EXPECT_EQ(pg.known_hosts_count_, 2u);

    SshKeyRemoveRequest rm{};
    rm.key_id_ = 3;   // "two@b"
    req = makeReq("com.freebsdx.SshKeys", static_cast<uint32_t>(SshKeyOpcode::RemoveKey),
                  0x6b000002, &rm, sizeof(rm));
    SshKeysService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.SshKeys",
              static_cast<uint32_t>(SshKeyOpcode::RemoveKey), 0x6b000002);
    std::string after = readHomeFile(home_ + "/.ssh/authorized_keys");
    EXPECT_EQ(after.find("two@b"), std::string::npos);
    EXPECT_NE(after.find("one@a"), std::string::npos);
    struct stat st{};
    ASSERT_EQ(::stat((home_ + "/.ssh/authorized_keys").c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0600u);
}

TEST_F(ServiceRoundtrip, CapturesListNewestFirstAndSafeDelete) {
    std::string shots = home_ + "/Pictures/Screenshots";
    writeHomeFile(shots + "/old.png", std::string(3000, 'x'));
    writeHomeFile(shots + "/new.png", "y");
    writeHomeFile(home_ + "/Videos/Captures/rec.mp4", "z");
    writeHomeFile(home_ + "/keep.txt", "k");
    struct timespec ts[2] = {{1000, 0}, {1000, 0}};
    ::utimensat(AT_FDCWD, (shots + "/old.png").c_str(), ts, 0);
    ts[0].tv_sec = ts[1].tv_sec = 2000;
    ::utimensat(AT_FDCWD, (home_ + "/Videos/Captures/rec.mp4").c_str(), ts, 0);

    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.Captures", static_cast<uint32_t>(CaptureOpcode::ListCaptures),
                              0x70000001, &page, sizeof(page));
    CapturesService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Captures",
                                static_cast<uint32_t>(CaptureOpcode::ListCaptures), 0x70000001);
    CapturePage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 0);
    ASSERT_EQ(pg.count_, 3u);
    EXPECT_STREQ(pg.entries_[0].filename_, "new.png");
    EXPECT_STREQ(pg.entries_[1].filename_, "rec.mp4");
    EXPECT_EQ(pg.entries_[1].type_, 1u);
    EXPECT_STREQ(pg.entries_[2].filename_, "old.png");
    EXPECT_EQ(pg.entries_[2].size_kb_, 3u);
    EXPECT_EQ(std::strlen(pg.entries_[2].timestamp_), 16u);   // "YYYY-MM-DD HH:MM"

    // A traversal name is refused; a plain one is deleted. Both are acked.
    for (const char* name : {"../../keep.txt", "old.png"}) {
        CaptureDeleteRequest d{};
        std::strncpy(d.filename_, name, sizeof(d.filename_) - 1);
        req = makeReq("com.freebsdx.Captures", static_cast<uint32_t>(CaptureOpcode::DeleteCapture),
                      0x70000002, &d, sizeof(d));
        CapturesService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.Captures",
                  static_cast<uint32_t>(CaptureOpcode::DeleteCapture), 0x70000002);
    }
    EXPECT_TRUE(std::filesystem::exists(home_ + "/keep.txt"));
    EXPECT_FALSE(std::filesystem::exists(shots + "/old.png"));
}

TEST_F(ServiceRoundtrip, EnvVarsSetListUnset) {
    auto p = makePipe();
    auto set = [&](const char* k, const char* v, uint32_t tid) {
        EnvSetRequest r{};
        std::strncpy(r.key_, k, sizeof(r.key_) - 1);
        std::strncpy(r.value_, v, sizeof(r.value_) - 1);
        XBusMessage req = makeReq("com.freebsdx.EnvVars", static_cast<uint32_t>(EnvVarOpcode::SetVar),
                                  tid, &r, sizeof(r));
        EnvVarsService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.EnvVars", static_cast<uint32_t>(EnvVarOpcode::SetVar), tid);
    };
    set("FBX_TEST_VAR", "hello", 0x65000001);
    set("XBUS_SOCKET", "/evil", 0x65000002);      // reserved: refused
    set("bad key", "x", 0x65000003);              // invalid: refused
    std::string file = readHomeFile(home_ + "/.config/freebsdx/environment");
    EXPECT_NE(file.find("FBX_TEST_VAR=hello\n"), std::string::npos);
    EXPECT_EQ(file.find("XBUS_SOCKET"), std::string::npos);

    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.EnvVars", static_cast<uint32_t>(EnvVarOpcode::ListVars),
                              0x65000004, &page, sizeof(page));
    EnvVarsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.EnvVars",
                                static_cast<uint32_t>(EnvVarOpcode::ListVars), 0x65000004);
    EnvVarPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    expectPageInvariants(pg, 0);
    ASSERT_GE(pg.count_, 2u);                      // user var + at least HOME
    EXPECT_STREQ(pg.entries_[0].key_, "FBX_TEST_VAR");   // user vars come first
    EXPECT_EQ(pg.entries_[0].readonly_, 0u);
    EXPECT_EQ(pg.entries_[1].readonly_, 1u);

    // Inherited vars can't be unset; the user var can.
    for (const char* k : {"HOME", "FBX_TEST_VAR"}) {
        EnvUnsetRequest u{};
        std::strncpy(u.key_, k, sizeof(u.key_) - 1);
        req = makeReq("com.freebsdx.EnvVars", static_cast<uint32_t>(EnvVarOpcode::UnsetVar),
                      0x65000005, &u, sizeof(u));
        EnvVarsService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.EnvVars",
                  static_cast<uint32_t>(EnvVarOpcode::UnsetVar), 0x65000005);
    }
    EXPECT_EQ(readHomeFile(home_ + "/.config/freebsdx/environment").find("FBX_TEST_VAR"),
              std::string::npos);
    EXPECT_STREQ(std::getenv("HOME"), home_.c_str());
}

// ── Batch 3: RemoteLogin / StartupItems / PrintQueue / InputDevice ────────────

TEST_F(ServiceRoundtrip, RemoteLoginGetStateFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.RemoteLogin",
                              static_cast<uint32_t>(RemoteLoginOpcode::GetState), 0x72000001);
    RemoteLoginService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.RemoteLogin",
                                static_cast<uint32_t>(RemoteLoginOpcode::GetState), 0x72000001);
    RemoteLoginState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_LE(st.enabled_, 1u);
    EXPECT_GT(st.port_, 0u);
    EXPECT_LT(st.port_, 65536u);
    EXPECT_NE(st.bind_addr_[0], '\0');
    if (!st.enabled_) EXPECT_EQ(st.sessions_, 0u);
}

TEST_F(ServiceRoundtrip, StartupItemsListToggleRemove) {
    std::string dir = home_ + "/.config/autostart";
    writeHomeFile(dir + "/b-sync.desktop",
                  "[Desktop Entry]\nName=Sync\nExec=/usr/local/bin/syncthing -no-browser\n");
    writeHomeFile(dir + "/a-notes.desktop",
                  "[Desktop Entry]\nName=Notes\nExec=notes %F\nHidden=true\n");
    writeHomeFile(dir + "/readme.txt", "not an entry");
    auto p = makePipe();
    auto list = [&](uint32_t tid) {
        uint32_t page = 0;
        XBusMessage req = makeReq("com.freebsdx.StartupItems",
                                  static_cast<uint32_t>(StartupOpcode::ListItems), tid, &page, sizeof(page));
        StartupItemsService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.StartupItems",
                                    static_cast<uint32_t>(StartupOpcode::ListItems), tid);
        StartupPage pg{};
        std::memcpy(&pg, rep.payload_, sizeof(pg));
        expectPageInvariants(pg, 0);
        return pg;
    };
    StartupPage pg = list(0x73100001);
    ASSERT_EQ(pg.count_, 2u);
    EXPECT_STREQ(pg.entries_[0].name_, "Notes");          // sorted by file name
    EXPECT_EQ(pg.entries_[0].enabled_, 0u);
    EXPECT_STREQ(pg.entries_[1].exec_, "syncthing");
    EXPECT_EQ(pg.entries_[1].enabled_, 1u);
    EXPECT_EQ(pg.entries_[1].builtin_, 0u);

    StartupToggleRequest t{};
    t.item_id_ = pg.entries_[0].item_id_;
    t.enabled_ = 1;
    XBusMessage req = makeReq("com.freebsdx.StartupItems",
                              static_cast<uint32_t>(StartupOpcode::SetEnabled), 0x73100002, &t, sizeof(t));
    StartupItemsService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.StartupItems",
              static_cast<uint32_t>(StartupOpcode::SetEnabled), 0x73100002);
    EXPECT_NE(readHomeFile(dir + "/a-notes.desktop").find("Hidden=false"), std::string::npos);

    StartupRemoveRequest rm{};
    rm.item_id_ = pg.entries_[1].item_id_;
    req = makeReq("com.freebsdx.StartupItems",
                  static_cast<uint32_t>(StartupOpcode::RemoveItem), 0x73100003, &rm, sizeof(rm));
    StartupItemsService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.StartupItems",
              static_cast<uint32_t>(StartupOpcode::RemoveItem), 0x73100003);
    EXPECT_FALSE(std::filesystem::exists(dir + "/b-sync.desktop"));

    pg = list(0x73100004);
    ASSERT_EQ(pg.count_, 1u);
    EXPECT_EQ(pg.entries_[0].enabled_, 1u);
    EXPECT_TRUE(std::filesystem::exists(dir + "/readme.txt"));
}

// cupsd isn't necessarily running: the list must still frame (empty is fine)
// and a bogus cancel must be acked, not hang.
TEST_F(ServiceRoundtrip, PrintQueueFramingAndBogusCancel) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.PrintQueue",
                              static_cast<uint32_t>(PrintOpcode::ListJobs), 0x70100001, &page, sizeof(page));
    PrintQueueService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.PrintQueue",
                                static_cast<uint32_t>(PrintOpcode::ListJobs), 0x70100001);
    PrintJobPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    EXPECT_EQ(pg.page_, 0u);
    EXPECT_LE(pg.count_, 2u);
    EXPECT_EQ(pg.has_more_, pg.total_ > 2 ? 1u : 0u);

    PrintCancelRequest c{};
    c.job_id_ = 0;
    req = makeReq("com.freebsdx.PrintQueue",
                  static_cast<uint32_t>(PrintOpcode::CancelJob), 0x70100002, &c, sizeof(c));
    PrintQueueService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.PrintQueue",
              static_cast<uint32_t>(PrintOpcode::CancelJob), 0x70100002);
}

TEST_F(ServiceRoundtrip, InputDeviceListAndPerDeviceSpeed) {
    using fbx::xserver::inputdev::DeviceInfo;
    std::vector<DeviceInfo> devs{
        {fbx::xserver::inputdev::deviceId("AT keyboard"), "AT keyboard", 0x0001, 0x0001, 0, 50, true},
        {fbx::xserver::inputdev::deviceId("TouchPad"),    "TouchPad",    0x06cb, 0x7e7e, 1, 50, true},
    };
    std::vector<std::tuple<uint32_t, uint32_t, bool>> applied;
    InputDeviceService::init([&] { return devs; },
                             [&](uint32_t id, uint32_t s, bool a) { applied.emplace_back(id, s, a); });
    applied.clear();   // init may replay persisted overrides

    auto p = makePipe();
    uint32_t page = 1;
    XBusMessage req = makeReq("com.freebsdx.InputDevice",
                              static_cast<uint32_t>(InputDeviceOpcode::ListDevices), 0x69000001, &page, sizeof(page));
    InputDeviceService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.InputDevice",
                                static_cast<uint32_t>(InputDeviceOpcode::ListDevices), 0x69000001);
    InputDevicePage dp{};
    std::memcpy(&dp, rep.payload_, sizeof(dp));
    EXPECT_EQ(dp.total_, 2u);
    EXPECT_EQ(dp.valid_, 1u);
    EXPECT_EQ(dp.has_more_, 0u);
    EXPECT_STREQ(dp.name_, "TouchPad");
    EXPECT_STREQ(dp.vendor_, "06cb:7e7e");
    EXPECT_EQ(dp.type_, 1u);

    for (uint32_t id : {devs[1].id, 12345u}) {       // known device, then unknown
        InputSensitivityRequest r{};
        r.device_id_ = id;
        r.sensitivity_ = 80;
        r.accel_profile_ = 0;
        req = makeReq("com.freebsdx.InputDevice",
                      static_cast<uint32_t>(InputDeviceOpcode::SetSensitivity), 0x69000002, &r, sizeof(r));
        InputDeviceService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.InputDevice",
                  static_cast<uint32_t>(InputDeviceOpcode::SetSensitivity), 0x69000002);
    }
    ASSERT_EQ(applied.size(), 1u);                    // unknown id ignored
    EXPECT_EQ(std::get<0>(applied[0]), devs[1].id);
    EXPECT_EQ(std::get<1>(applied[0]), 80u);
    EXPECT_FALSE(std::get<2>(applied[0]));

    // Persisted: a fresh init replays the override.
    applied.clear();
    InputDeviceService::init([&] { return devs; },
                             [&](uint32_t id, uint32_t s, bool a) { applied.emplace_back(id, s, a); });
    ASSERT_EQ(applied.size(), 1u);
    EXPECT_EQ(std::get<1>(applied[0]), 80u);
    InputDeviceService::init(nullptr, nullptr);
}

// ── Batch 4: NetworkShare(s) / VirtualMachines / Containers / GpuStats ────────

TEST_F(ServiceRoundtrip, NetworkSharesListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.NetworkShares",
                              static_cast<uint32_t>(NetworkShareOpcode::ListShares), 0x6e000001, &page, sizeof(page));
    NetworkSharesService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.NetworkShares",
                                static_cast<uint32_t>(NetworkShareOpcode::ListShares), 0x6e000001);
    NetworkSharePage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    EXPECT_LE(pg.count_, 2u);
    EXPECT_EQ(pg.has_more_, pg.total_ > 2 ? 1u : 0u);
    for (uint32_t i = 0; i < pg.count_; ++i) {
        std::string type(pg.entries_[i].type_);
        EXPECT_TRUE(type == "SMB" || type == "NFS") << type;
    }
}

// Every request here is refused before anything is spawned, so nothing mounts.
TEST_F(ServiceRoundtrip, NetworkShareMountRefusalsCarryStatus) {
    auto p = makePipe();
    auto mount = [&](const char* host, const char* share, const char* mp, const char* pass,
                     uint32_t proto, uint32_t tid) {
        NetworkShareMountRequest r{};
        std::strncpy(r.host_, host, sizeof(r.host_) - 1);
        std::strncpy(r.share_, share, sizeof(r.share_) - 1);
        std::strncpy(r.mountpoint_, mp, sizeof(r.mountpoint_) - 1);
        std::strncpy(r.password_, pass, sizeof(r.password_) - 1);
        r.protocol_ = proto;
        XBusMessage req = makeReq("com.freebsdx.NetworkShare",
                                  static_cast<uint32_t>(NetworkShareMountOpcode::Mount), tid, &r, sizeof(r));
        NetworkShareService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.NetworkShare",
                                    static_cast<uint32_t>(NetworkShareMountOpcode::Mount), tid);
        uint32_t st = 0;
        std::memcpy(&st, rep.payload_, sizeof(st));
        return st;
    };
    EXPECT_EQ(mount("-oexec", "/x", "/tmp", "", 1, 0x6e100001), static_cast<uint32_t>(EINVAL));
    EXPECT_EQ(mount("nas", "/../etc", "/tmp", "", 1, 0x6e100002), static_cast<uint32_t>(EINVAL));
    EXPECT_EQ(mount("nas", "media", "/tmp", "secret", 0, 0x6e100003), static_cast<uint32_t>(ENOTSUP));
    uint32_t st = mount("nas", "/export", (home_ + "/no-such-dir").c_str(), "", 1, 0x6e100004);
    EXPECT_TRUE(st == static_cast<uint32_t>(EPERM) || st == static_cast<uint32_t>(ENOTDIR)) << st;

    NetworkShareRequest u{};
    u.share_id_ = 0xdeadbeef;
    XBusMessage req = makeReq("com.freebsdx.NetworkShare",
                              static_cast<uint32_t>(NetworkShareMountOpcode::Unmount), 0x6e100005, &u, sizeof(u));
    NetworkShareService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.NetworkShare",
                                static_cast<uint32_t>(NetworkShareMountOpcode::Unmount), 0x6e100005);
    uint32_t ust = 0;
    std::memcpy(&ust, rep.payload_, sizeof(ust));
    EXPECT_EQ(ust, static_cast<uint32_t>(ENOENT));
}

TEST_F(ServiceRoundtrip, VmsAndContainersListFraming) {
    auto p = makePipe();
    uint32_t page = 0;
    XBusMessage req = makeReq("com.freebsdx.VirtualMachines",
                              static_cast<uint32_t>(VmOpcode::ListVMs), 0x76000001, &page, sizeof(page));
    VirtualMachinesService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.VirtualMachines",
                                static_cast<uint32_t>(VmOpcode::ListVMs), 0x76000001);
    VmPage vp{};
    std::memcpy(&vp, rep.payload_, sizeof(vp));
    expectPageInvariants(vp, 0);
    for (uint32_t i = 0; i < vp.count_; ++i) EXPECT_STREQ(vp.entries_[i].type_, "bhyve");

    req = makeReq("com.freebsdx.Containers",
                  static_cast<uint32_t>(ContainerOpcode::ListContainers), 0x63100001, &page, sizeof(page));
    ContainersService::handleMessage(req, p.svc);
    rep = recvReply(p.peer, "com.freebsdx.Containers",
                    static_cast<uint32_t>(ContainerOpcode::ListContainers), 0x63100001);
    ContainerPage cp{};
    std::memcpy(&cp, rep.payload_, sizeof(cp));
    expectPageInvariants(cp, 0);
}

TEST_F(ServiceRoundtrip, GpuStatsNameAndNoTempSensor) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.GpuStats",
                              static_cast<uint32_t>(GpuOpcode::GetStats), 0x67000001);
    GpuStatsService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.GpuStats",
                                static_cast<uint32_t>(GpuOpcode::GetStats), 0x67000001);
    GpuStats st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_NE(st.name_[0], '\0');
    EXPECT_EQ(st.temp_c_, INT32_MIN);
}

// ── Batch 5: Totp / TextReplacement / Reminder / ScreenLock / License ─────────


TEST_F(ServiceRoundtrip, TotpListAndCodeOnlyFromPrivateFile) {
    std::string path = home_ + "/.config/freebsdx/totp";
    writeHomeFile(path, "otpauth://totp/GitHub:octo?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n");
    auto p = makePipe();
    auto list = [&](uint32_t tid) {
        uint32_t page = 0;
        XBusMessage req = makeReq("com.freebsdx.Totp", static_cast<uint32_t>(TotpOpcode::ListAccounts),
                                  tid, &page, sizeof(page));
        TotpService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.Totp",
                                    static_cast<uint32_t>(TotpOpcode::ListAccounts), tid);
        TotpAccountPage pg{};
        std::memcpy(&pg, rep.payload_, sizeof(pg));
        return pg;
    };
    ::chmod(path.c_str(), 0644);
    EXPECT_EQ(list(0x74000001).total_, 0u);        // world-readable secrets are ignored
    ::chmod(path.c_str(), 0600);
    TotpAccountPage pg = list(0x74000002);
    ASSERT_EQ(pg.total_, 1u);
    EXPECT_STREQ(pg.issuer_, "GitHub");
    EXPECT_STREQ(pg.account_, "octo");

    TotpCodeRequest cr{};
    std::strncpy(cr.id_, pg.id_, sizeof(cr.id_) - 1);
    XBusMessage req = makeReq("com.freebsdx.Totp", static_cast<uint32_t>(TotpOpcode::GetCode),
                              0x74000003, &cr, sizeof(cr));
    TotpService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.Totp",
                                static_cast<uint32_t>(TotpOpcode::GetCode), 0x74000003);
    TotpCodeResponse code{};
    std::memcpy(&code, rep.payload_, sizeof(code));
    EXPECT_EQ(std::strlen(code.code_), 6u);
    EXPECT_GE(code.seconds_remaining_, 1u);
    EXPECT_LE(code.seconds_remaining_, 30u);
}

TEST_F(ServiceRoundtrip, TextReplacementAddListDelete) {
    auto p = makePipe();
    auto add = [&](const char* k, const char* v, uint32_t tid) {
        TextReplacementAddRequest r{};
        std::strncpy(r.shortcut_, k, sizeof(r.shortcut_) - 1);
        std::strncpy(r.phrase_, v, sizeof(r.phrase_) - 1);
        XBusMessage req = makeReq("com.freebsdx.TextReplacement",
                                  static_cast<uint32_t>(TextReplacementOpcode::AddShortcut), tid, &r, sizeof(r));
        TextReplacementService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.TextReplacement",
                  static_cast<uint32_t>(TextReplacementOpcode::AddShortcut), tid);
    };
    add("omw", "On my way!", 0x78000001);
    add("two words", "nope", 0x78000002);           // invalid shortcut: refused
    add("brb", "Be right back", 0x78000003);
    std::string file = readHomeFile(home_ + "/.config/freebsdx/text-replacements");
    EXPECT_NE(file.find("omw\tOn my way!\n"), std::string::npos);
    EXPECT_EQ(file.find("two words"), std::string::npos);

    uint32_t page = 1;
    XBusMessage req = makeReq("com.freebsdx.TextReplacement",
                              static_cast<uint32_t>(TextReplacementOpcode::ListShortcuts), 0x78000004, &page, sizeof(page));
    TextReplacementService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.TextReplacement",
                                static_cast<uint32_t>(TextReplacementOpcode::ListShortcuts), 0x78000004);
    TextReplacementPage pg{};
    std::memcpy(&pg, rep.payload_, sizeof(pg));
    EXPECT_EQ(pg.total_, 2u);
    EXPECT_EQ(pg.valid_, 1u);
    EXPECT_STREQ(pg.shortcut_, "brb");

    TextReplacementDeleteRequest d{};
    d.entry_id_ = pg.entry_id_;
    req = makeReq("com.freebsdx.TextReplacement",
                  static_cast<uint32_t>(TextReplacementOpcode::DeleteShortcut), 0x78000005, &d, sizeof(d));
    TextReplacementService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.TextReplacement",
              static_cast<uint32_t>(TextReplacementOpcode::DeleteShortcut), 0x78000005);
    EXPECT_EQ(readHomeFile(home_ + "/.config/freebsdx/text-replacements").find("brb"), std::string::npos);
}

TEST_F(ServiceRoundtrip, ReminderFromCalendarThenComplete) {
    // An event 2 h from now with a 30-minute reminder, and one without.
    std::time_t start = std::time(nullptr) + 2 * 3600;
    std::tm tm{};
    localtime_r(&start, &tm);
    char line[256];
    std::snprintf(line, sizeof(line), "7|%d|%d|%d|%d|%d|%d|%d|30|0|Dentist|desc|\n"
                                      "8|%d|%d|%d|%d|%d|%d|%d|0|0|No reminder||\n",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_hour, tm.tm_min,
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_hour, tm.tm_min);
    writeHomeFile(home_ + "/.freebsdx-calendar", line);

    auto p = makePipe();
    auto list = [&](uint32_t tid) {
        uint32_t page = 0;
        XBusMessage req = makeReq("com.freebsdx.Reminder", static_cast<uint32_t>(ReminderOpcode::ListReminders),
                                  tid, &page, sizeof(page));
        ReminderService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.Reminder",
                                    static_cast<uint32_t>(ReminderOpcode::ListReminders), tid);
        ReminderPage pg{};
        std::memcpy(&pg, rep.payload_, sizeof(pg));
        return pg;
    };
    ReminderPage pg = list(0x52000001);
    ASSERT_EQ(pg.total_, 1u);
    EXPECT_STREQ(pg.id_, "ev-7");
    EXPECT_STREQ(pg.title_, "Dentist");
    EXPECT_EQ(pg.overdue_, 0u);
    EXPECT_EQ(std::strlen(pg.due_), 16u);

    ReminderActionRequest done{};
    std::strncpy(done.id_, "ev-7", sizeof(done.id_) - 1);
    XBusMessage req = makeReq("com.freebsdx.Reminder", static_cast<uint32_t>(ReminderOpcode::CompleteReminder),
                              0x52000002, &done, sizeof(done));
    ReminderService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.Reminder",
              static_cast<uint32_t>(ReminderOpcode::CompleteReminder), 0x52000002);
    EXPECT_EQ(list(0x52000003).total_, 0u);
}

TEST_F(ServiceRoundtrip, ScreenLockPolicyDrivesIdleTimeout) {
    auto p = makePipe();
    auto set = [&](uint32_t enabled, uint32_t timeout, uint32_t tid) {
        ScreenLockPolicy pol{};
        pol.enabled_ = enabled;
        pol.timeout_seconds_ = timeout;
        XBusMessage req = makeReq("com.freebsdx.ScreenLock", static_cast<uint32_t>(ScreenLockOpcode::SetPolicy),
                                  tid, &pol, sizeof(pol));
        ScreenLockService::handleMessage(req, p.svc);
        recvReply(p.peer, "com.freebsdx.ScreenLock",
                  static_cast<uint32_t>(ScreenLockOpcode::SetPolicy), tid);
    };
    auto get = [&](uint32_t tid) {
        XBusMessage req = makeReq("com.freebsdx.ScreenLock", static_cast<uint32_t>(ScreenLockOpcode::GetPolicy), tid);
        ScreenLockService::handleMessage(req, p.svc);
        XBusMessage rep = recvReply(p.peer, "com.freebsdx.ScreenLock",
                                    static_cast<uint32_t>(ScreenLockOpcode::GetPolicy), tid);
        ScreenLockPolicy pol{};
        std::memcpy(&pol, rep.payload_, sizeof(pol));
        return pol;
    };
    set(1, 300, 0x53000001);
    EXPECT_EQ(LockService::timeoutSeconds(), 300u);
    ScreenLockPolicy pol = get(0x53000002);
    EXPECT_EQ(pol.enabled_, 1u);
    EXPECT_EQ(pol.timeout_seconds_, 300u);
    EXPECT_EQ(pol.require_password_, 1u);

    set(0, 300, 0x53000003);                         // off: idle lock disabled...
    EXPECT_EQ(LockService::timeoutSeconds(), 0u);
    pol = get(0x53000004);
    EXPECT_EQ(pol.enabled_, 0u);
    EXPECT_EQ(pol.timeout_seconds_, 300u);           // ...but the chosen timeout is kept

    set(1, 5, 0x53000005);                           // clamped to 30 s
    EXPECT_EQ(LockService::timeoutSeconds(), 30u);

    int before = g_stub_lock_calls.load();
    XBusMessage req = makeReq("com.freebsdx.ScreenLock", static_cast<uint32_t>(ScreenLockOpcode::LockNow), 0x53000006);
    ScreenLockService::handleMessage(req, p.svc);
    recvReply(p.peer, "com.freebsdx.ScreenLock", static_cast<uint32_t>(ScreenLockOpcode::LockNow), 0x53000006);
    EXPECT_EQ(g_stub_lock_calls.load(), before + 1);
}

TEST_F(ServiceRoundtrip, LicenseFraming) {
    auto p = makePipe();
    XBusMessage req = makeReq("com.freebsdx.License", static_cast<uint32_t>(LicenseOpcode::GetInfo), 0x4c000001);
    LicenseService::handleMessage(req, p.svc);
    XBusMessage rep = recvReply(p.peer, "com.freebsdx.License",
                                static_cast<uint32_t>(LicenseOpcode::GetInfo), 0x4c000001);
    LicenseState st{};
    std::memcpy(&st, rep.payload_, sizeof(st));
    EXPECT_NE(st.tier_[0], '\0');
    EXPECT_NE(st.expiry_[0], '\0');
    // Never more than the last 4 characters of a key.
    std::string key(st.license_key_);
    if (!key.empty()) EXPECT_EQ(key.rfind("****-****-****-", 0), 0u);
}

}  // namespace
