// Tests for shared/protocol.h — struct sizes, field layout, and the
// window-geometry extension added to RenderMessage.
// No hardware, no Vulkan, no IPC — pure struct inspection.

#include "shared/protocol.h"
#include <gtest/gtest.h>
#include <cstring>

namespace fbx::shared::testing {

// ── Struct size contracts ─────────────────────────────────────────────────────
// XBusMessage is the IPC envelope — intentionally 300 bytes (larger than
// service structs) to hold the payload_ array.  The 256-byte contract applies
// only to the service protocol structs embedded in XBusMessage::payload_.

TEST(ProtocolSizeTest, XBusMessageEnvelopeSize) {
    // 300 bytes: magic(4) + opcode(4) + payload(256) + sender metadata = 300.
    // Verified at runtime so a future size change produces a clear failure here.
    EXPECT_EQ(sizeof(XBusMessage), 300u);
}

TEST(ProtocolSizeTest, RenderMessageHasExpectedLayout) {
    // RenderMessage is the compositor ↔ bridge frame-notification struct.
    // Verify the geometry fields land at the expected offset after damage fields.
    EXPECT_EQ(offsetof(RenderMessage, geom_x_),
              offsetof(RenderMessage, damage_height_) + sizeof(float));
}

// ── RenderMessage field defaults ──────────────────────────────────────────────

TEST(RenderMessageTest, DefaultGeometryFieldsAreZero) {
    RenderMessage msg{};
    EXPECT_EQ(msg.geom_x_, 0);
    EXPECT_EQ(msg.geom_y_, 0);
    EXPECT_EQ(msg.geom_w_, 0);
    EXPECT_EQ(msg.geom_h_, 0);
}

TEST(RenderMessageTest, GeometryNotSetSentinel) {
    // geom_w_ == 0 is the "not set" sentinel used throughout compositor + main.cc
    RenderMessage msg{};
    EXPECT_FALSE(msg.geom_w_ > 0); // not set

    msg.geom_w_ = 800;
    EXPECT_TRUE(msg.geom_w_ > 0);  // set
}

// ── RenderMessage round-trip through a byte buffer (simulates IPC memcpy) ────

TEST(RenderMessageTest, GeometryRoundtripThroughBuffer) {
    RenderMessage out_msg{};
    out_msg.surface_id_ = 42u;
    out_msg.status_     = RenderMessageStatus::FrameReady;
    out_msg.width_      = 820u;
    out_msg.height_     = 540u;
    out_msg.geom_x_     = 10;
    out_msg.geom_y_     = 8;
    out_msg.geom_w_     = 800;
    out_msg.geom_h_     = 524;

    // Simulate the IPC path: copy to raw buffer, copy back out
    alignas(RenderMessage) char buf[sizeof(RenderMessage)];
    std::memcpy(buf, &out_msg, sizeof(out_msg));

    RenderMessage in_msg{};
    std::memcpy(&in_msg, buf, sizeof(in_msg));

    EXPECT_EQ(in_msg.surface_id_, 42u);
    EXPECT_EQ(in_msg.status_,     RenderMessageStatus::FrameReady);
    EXPECT_EQ(in_msg.width_,      820u);
    EXPECT_EQ(in_msg.height_,     540u);
    EXPECT_EQ(in_msg.geom_x_,     10);
    EXPECT_EQ(in_msg.geom_y_,     8);
    EXPECT_EQ(in_msg.geom_w_,     800);
    EXPECT_EQ(in_msg.geom_h_,     524);
}

TEST(RenderMessageTest, GeometryNegativeOffsetAllowed) {
    // Some toolkits may use negative geom offsets (unusual but valid).
    RenderMessage msg{};
    msg.geom_x_ = -5;
    msg.geom_y_ = -3;

    alignas(RenderMessage) char buf[sizeof(RenderMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    RenderMessage out{};
    std::memcpy(&out, buf, sizeof(out));

    EXPECT_EQ(out.geom_x_, -5);
    EXPECT_EQ(out.geom_y_, -3);
}

// ── Field layout — surface_id_ must be first 4 bytes ─────────────────────────

TEST(RenderMessageTest, SurfaceIdIsFirstField) {
    RenderMessage msg{};
    msg.surface_id_ = 0xDEADBEEFu;

    uint32_t readback = 0;
    std::memcpy(&readback, &msg, sizeof(uint32_t));
    EXPECT_EQ(readback, 0xDEADBEEFu);
}

// ── XBus magic ────────────────────────────────────────────────────────────────

TEST(ProtocolTest, XBusMagicKnownValue) {
    // XBUS_MAGIC = 0x58425553 encodes 'X','B','U','S' in big-endian order.
    // The dispatch loop in main.cc reads the first 4 bytes of every IPC
    // message as a uint32_t and compares against this constant.
    // Verifying the literal guards against accidental renaming/renumbering.
    EXPECT_EQ(XBUS_MAGIC, 0x58425553u);
}

// ── RenderMessageStatus enum coverage ────────────────────────────────────────

TEST(RenderMessageTest, StatusEnumValues) {
    // Ensure the enum values haven't been accidentally renumbered.
    EXPECT_EQ(static_cast<uint32_t>(RenderMessageStatus::SurfaceCreated),      0u);
    EXPECT_EQ(static_cast<uint32_t>(RenderMessageStatus::FrameReady),          1u);
    EXPECT_EQ(static_cast<uint32_t>(RenderMessageStatus::SurfaceClosed),       2u);
    EXPECT_EQ(static_cast<uint32_t>(RenderMessageStatus::ShellSurfaceCreated), 3u);
    EXPECT_EQ(static_cast<uint32_t>(RenderMessageStatus::SurfaceReady),        4u);
}

// ── Damage region fields coexist with geometry fields ────────────────────────

TEST(RenderMessageTest, DamageAndGeometryIndependent) {
    RenderMessage msg{};
    msg.damage_x_      = 10.0f;
    msg.damage_y_      = 20.0f;
    msg.damage_width_  = 100.0f;
    msg.damage_height_ = 200.0f;
    msg.geom_x_        = 5;
    msg.geom_y_        = 5;
    msg.geom_w_        = 800;
    msg.geom_h_        = 600;

    // Neither field group should corrupt the other
    EXPECT_FLOAT_EQ(msg.damage_x_,      10.0f);
    EXPECT_FLOAT_EQ(msg.damage_width_,  100.0f);
    EXPECT_EQ(msg.geom_x_,  5);
    EXPECT_EQ(msg.geom_w_,  800);
}

// ── Service struct size contracts (Pairs 35–41) ───────────────────────────────
//
// These runtime checks mirror the static_asserts in protocol.h and produce clear
// failure messages if a padding change silently breaks the 256-byte IPC contract.

// Night Light (Pair 35 QS)
TEST(ServiceProtocolSizeTest, NightLightStateIs256) {
    EXPECT_EQ(sizeof(NightLightState), 256u);
}
TEST(ServiceProtocolSizeTest, NightLightRequestsFitPayload) {
    EXPECT_LE(sizeof(NightLightEnableRequest), 256u);
    EXPECT_LE(sizeof(NightLightTempRequest),   256u);
    EXPECT_LE(sizeof(NightLightScheduleRequest),256u);
}

// Crash Reporter (Pair 35 AC)
TEST(ServiceProtocolSizeTest, CrashReportIs64) {
    EXPECT_EQ(sizeof(CrashReport), 64u);
}
TEST(ServiceProtocolSizeTest, CrashReportPageFitsPayload) {
    EXPECT_LE(sizeof(CrashReportPage), 256u);
}

// Game Mode (Pair 36 QS)
TEST(ServiceProtocolSizeTest, GameModeStateIs256) {
    EXPECT_EQ(sizeof(GameModeState), 256u);
}

// Security Events (Pair 36 AC)
TEST(ServiceProtocolSizeTest, SecurityEventIs64) {
    EXPECT_EQ(sizeof(SecurityEvent), 64u);
}
TEST(ServiceProtocolSizeTest, SecurityEventPageFitsPayload) {
    EXPECT_LE(sizeof(SecurityEventPage), 256u);
}

// Accessibility Settings (Pair 37 QS)
TEST(ServiceProtocolSizeTest, AccessibilitySettingsIs256) {
    EXPECT_EQ(sizeof(AccessibilitySettings), 256u);
}
TEST(ServiceProtocolSizeTest, AccessibilityRequestsFitPayload) {
    EXPECT_LE(sizeof(AccessibilityFontRequest),   256u);
    EXPECT_LE(sizeof(AccessibilityCursorRequest), 256u);
    EXPECT_LE(sizeof(AccessibilityToggleRequest), 256u);
}

// Downloads (Pair 37 AC)
TEST(ServiceProtocolSizeTest, DownloadEntryIs96) {
    EXPECT_EQ(sizeof(DownloadEntry), 96u);
}
TEST(ServiceProtocolSizeTest, DownloadPageFitsPayload) {
    EXPECT_LE(sizeof(DownloadPage), 256u);
}

// Ad-Blocker (Pair 38 QS)
TEST(ServiceProtocolSizeTest, AdBlockStatusIs256) {
    EXPECT_EQ(sizeof(AdBlockStatus), 256u);
}

// System Services (Pair 38 AC)
TEST(ServiceProtocolSizeTest, ServiceEntryIs64) {
    EXPECT_EQ(sizeof(ServiceEntry), 64u);
}
TEST(ServiceProtocolSizeTest, ServicePageFitsPayload) {
    EXPECT_LE(sizeof(ServicePage), 256u);
}

// Screen Cast (Pair 39 QS)
TEST(ServiceProtocolSizeTest, CastReceiverIs48) {
    EXPECT_EQ(sizeof(CastReceiver), 48u);
}
TEST(ServiceProtocolSizeTest, CastReceiverPageFitsPayload) {
    EXPECT_LE(sizeof(CastReceiverPage), 256u);
}
TEST(ServiceProtocolSizeTest, CastStateIs256) {
    EXPECT_EQ(sizeof(CastState), 256u);
}

// Package Updates (Pair 39 AC)
TEST(ServiceProtocolSizeTest, PackageUpdateIs96) {
    EXPECT_EQ(sizeof(PackageUpdate), 96u);
}
TEST(ServiceProtocolSizeTest, PackagePageFitsPayload) {
    EXPECT_LE(sizeof(PackagePage), 256u);
}

// Focus Mode (Pair 40 QS)
TEST(ServiceProtocolSizeTest, FocusModeStateIs256) {
    EXPECT_EQ(sizeof(FocusModeState), 256u);
}

// SMART Health (Pair 40 AC)
TEST(ServiceProtocolSizeTest, SmartDriveIs64) {
    EXPECT_EQ(sizeof(SmartDrive), 64u);
}
TEST(ServiceProtocolSizeTest, SmartPageFitsPayload) {
    EXPECT_LE(sizeof(SmartPage), 256u);
}

// Color Filter (Pair 41 QS)
TEST(ServiceProtocolSizeTest, ColorFilterStateIs256) {
    EXPECT_EQ(sizeof(ColorFilterState), 256u);
}
TEST(ServiceProtocolSizeTest, ColorFilterRequestsFitPayload) {
    EXPECT_LE(sizeof(ColorFilterEnableRequest),   256u);
    EXPECT_LE(sizeof(ColorFilterTypeRequest),     256u);
    EXPECT_LE(sizeof(ColorFilterIntensityRequest),256u);
    EXPECT_LE(sizeof(ColorFilterToggleRequest),   256u);
}

// Login Sessions (Pair 41 AC)
TEST(ServiceProtocolSizeTest, SessionEntryIs64) {
    EXPECT_EQ(sizeof(SessionEntry), 64u);
}
TEST(ServiceProtocolSizeTest, SessionPageFitsPayload) {
    EXPECT_LE(sizeof(SessionPage), 256u);
}
TEST(ServiceProtocolSizeTest, SessionKillRequestFitsPayload) {
    EXPECT_LE(sizeof(SessionKillRequest), 256u);
}

// ── Struct field alignment — uint64_t members must not be preceded by padding ─
//
// Structs that had alignment bugs during development (SecurityEvent, DownloadEntry,
// PackageUpdate, SessionEntry) have their uint64_t field verified to sit at an
// 8-byte-aligned offset.  A sizeof mismatch or wrong offset means the bug returned.

TEST(ServiceProtocolAlignTest, SecurityEventTimestampAligned) {
    // timestamp_ must land at an offset divisible by 8; it follows type_[8].
    EXPECT_EQ(offsetof(SecurityEvent, timestamp_), 8u)
        << "SecurityEvent::timestamp_ must be at offset 8 (after type_[8])";
    EXPECT_EQ(offsetof(SecurityEvent, timestamp_) % 8, 0u)
        << "SecurityEvent::timestamp_ offset must be 8-byte-aligned";
}

TEST(ServiceProtocolAlignTest, DownloadEntryUint64sAreFirst) {
    // total_bytes_ at offset 0, received_bytes_ at offset 8 — both 8-byte aligned.
    EXPECT_EQ(offsetof(DownloadEntry, total_bytes_),    0u);
    EXPECT_EQ(offsetof(DownloadEntry, received_bytes_), 8u);
    EXPECT_EQ(offsetof(DownloadEntry, total_bytes_)    % 8, 0u);
    EXPECT_EQ(offsetof(DownloadEntry, received_bytes_) % 8, 0u);
}

TEST(ServiceProtocolAlignTest, PackageUpdateUint64IsFirst) {
    EXPECT_EQ(offsetof(PackageUpdate, total_bytes_), 0u);
    EXPECT_EQ(offsetof(PackageUpdate, total_bytes_) % 8, 0u);
}

TEST(ServiceProtocolAlignTest, SessionEntryLoginTimeIsFirst) {
    // login_time_ at offset 0, naturally 8-byte-aligned.
    EXPECT_EQ(offsetof(SessionEntry, login_time_), 0u);
    EXPECT_EQ(offsetof(SessionEntry, login_time_) % 8, 0u);
}

// ── XBusMessage::payload_ round-trip tests ────────────────────────────────────
//
// Each test embeds a service struct into an XBusMessage, copies the message
// through a byte buffer (simulating the IPC read path), then extracts and
// verifies every field.  This catches any memcpy-size mismatch or packing error
// that would silently corrupt service data in production.

TEST(ServiceProtocolRoundTripTest, NightLightStateRoundTrip) {
    NightLightState ns{};
    ns.enabled_          = 1;
    ns.temperature_k_    = 4500;
    ns.schedule_enabled_ = 1;
    std::strncpy(ns.from_time_, "20:00", sizeof(ns.from_time_) - 1);
    std::strncpy(ns.to_time_,   "07:00", sizeof(ns.to_time_)   - 1);

    XBusMessage msg{};
    msg.magic_   = XBUS_MAGIC;
    msg.opcode_  = static_cast<uint32_t>(NightLightOpcode::StateChanged);
    std::strncpy(msg.service_name_, "com.freebsdx.NightLight",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &ns, sizeof(ns));

    // Simulate IPC: copy through raw buffer
    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in_msg{};
    std::memcpy(&in_msg, buf, sizeof(in_msg));

    NightLightState out{};
    std::memcpy(&out, in_msg.payload_, sizeof(out));

    EXPECT_EQ(out.enabled_,          1u);
    EXPECT_EQ(out.temperature_k_,    4500u);
    EXPECT_EQ(out.schedule_enabled_, 1u);
    EXPECT_STREQ(out.from_time_, "20:00");
    EXPECT_STREQ(out.to_time_,   "07:00");
}

TEST(ServiceProtocolRoundTripTest, SecurityEventRoundTrip) {
    SecurityEvent ev{};
    std::strncpy(ev.type_,   "sudo",    sizeof(ev.type_)   - 1);
    ev.timestamp_ = 1721000000ULL;
    std::strncpy(ev.user_,   "wheel",   sizeof(ev.user_)   - 1);
    std::strncpy(ev.source_, "pts/0",   sizeof(ev.source_) - 1);
    ev.success_   = 1;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(SecurityOpcode::EventAdded);
    // embed as part of a SecurityEventPage
    SecurityEventPage pg{};
    pg.page_  = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = ev;
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in_msg{};
    std::memcpy(&in_msg, buf, sizeof(in_msg));

    SecurityEventPage pg_out{};
    std::memcpy(&pg_out, in_msg.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_EQ(pg_out.entries_[0].timestamp_, 1721000000ULL);
    EXPECT_STREQ(pg_out.entries_[0].type_,   "sudo");
    EXPECT_STREQ(pg_out.entries_[0].user_,   "wheel");
    EXPECT_EQ(pg_out.entries_[0].success_,   1u);
}

TEST(ServiceProtocolRoundTripTest, DownloadEntryRoundTrip) {
    DownloadEntry de{};
    de.total_bytes_    = 1073741824ULL; // 1 GiB
    de.received_bytes_ = 536870912ULL;  // 512 MiB
    de.id_             = 7;
    de.speed_kbps_     = 8192;
    de.state_          = 1; // downloading
    std::strncpy(de.filename_, "freebsd-15.0-RELEASE.iso", sizeof(de.filename_) - 1);

    DownloadPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = de;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(DownloadOpcode::DownloadProgress);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in_msg{};
    std::memcpy(&in_msg, buf, sizeof(in_msg));

    DownloadPage pg_out{};
    std::memcpy(&pg_out, in_msg.payload_, sizeof(pg_out));
    const auto& d = pg_out.entries_[0];

    EXPECT_EQ(d.total_bytes_,    1073741824ULL);
    EXPECT_EQ(d.received_bytes_, 536870912ULL);
    EXPECT_EQ(d.id_,             7u);
    EXPECT_EQ(d.speed_kbps_,     8192u);
    EXPECT_STREQ(d.filename_,    "freebsd-15.0-RELEASE.iso");
}

TEST(ServiceProtocolRoundTripTest, ColorFilterStateRoundTrip) {
    ColorFilterState cf{};
    cf.enabled_    = 1;
    cf.type_       = static_cast<uint32_t>(ColorFilterType::Deuteranopia);
    cf.intensity_  = 75;
    cf.monochrome_ = 0;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(ColorFilterOpcode::StateChanged);
    std::strncpy(msg.service_name_, "com.freebsdx.ColorFilter",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &cf, sizeof(cf));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in_msg{};
    std::memcpy(&in_msg, buf, sizeof(in_msg));

    ColorFilterState out{};
    std::memcpy(&out, in_msg.payload_, sizeof(out));

    EXPECT_EQ(out.enabled_,    1u);
    EXPECT_EQ(out.type_,       static_cast<uint32_t>(ColorFilterType::Deuteranopia));
    EXPECT_EQ(out.intensity_,  75u);
    EXPECT_EQ(out.monochrome_, 0u);
}

TEST(ServiceProtocolRoundTripTest, SessionEntryRoundTrip) {
    SessionEntry se{};
    se.login_time_  = 1721400000ULL;
    se.session_id_  = 42;
    std::strncpy(se.type_,   "ssh",        sizeof(se.type_)   - 1);
    std::strncpy(se.user_,   "alice",      sizeof(se.user_)   - 1);
    std::strncpy(se.source_, "10.0.0.5",  sizeof(se.source_) - 1);

    SessionPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = se;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(SessionOpcode::ListSessions);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in_msg{};
    std::memcpy(&in_msg, buf, sizeof(in_msg));

    SessionPage pg_out{};
    std::memcpy(&pg_out, in_msg.payload_, sizeof(pg_out));
    const auto& s = pg_out.entries_[0];

    EXPECT_EQ(s.login_time_, 1721400000ULL);
    EXPECT_EQ(s.session_id_, 42u);
    EXPECT_STREQ(s.type_,    "ssh");
    EXPECT_STREQ(s.user_,    "alice");
    EXPECT_STREQ(s.source_,  "10.0.0.5");
}

// ── Opcode stability (Pairs 35–41) ────────────────────────────────────────────
//
// Opcodes are part of the binary IPC protocol.  Accidental renumbering would
// silently break running services without a compile error.

TEST(ServiceProtocolOpcodeTest, NightLightOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(NightLightOpcode::GetState),        0x01u);
    EXPECT_EQ(static_cast<uint32_t>(NightLightOpcode::SetEnabled),      0x02u);
    EXPECT_EQ(static_cast<uint32_t>(NightLightOpcode::SetTemperature),  0x03u);
    EXPECT_EQ(static_cast<uint32_t>(NightLightOpcode::SetSchedule),     0x04u);
    EXPECT_EQ(static_cast<uint32_t>(NightLightOpcode::StateChanged),    0x05u);
}

TEST(ServiceProtocolOpcodeTest, ColorFilterOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterOpcode::GetState),      0x01u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterOpcode::SetEnabled),    0x02u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterOpcode::SetType),       0x03u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterOpcode::SetIntensity),  0x04u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterOpcode::SetMonochrome), 0x05u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterOpcode::StateChanged),  0x06u);
}

TEST(ServiceProtocolOpcodeTest, SessionOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(SessionOpcode::ListSessions),   0x01u);
    EXPECT_EQ(static_cast<uint32_t>(SessionOpcode::KillSession),    0x02u);
    EXPECT_EQ(static_cast<uint32_t>(SessionOpcode::SessionChanged), 0x03u);
}

TEST(ServiceProtocolOpcodeTest, ColorFilterTypeEnum) {
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterType::Normal),       0u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterType::Deuteranopia), 1u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterType::Protanopia),   2u);
    EXPECT_EQ(static_cast<uint32_t>(ColorFilterType::Tritanopia),   3u);
}

TEST(ServiceProtocolOpcodeTest, FocusModeOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(FocusModeOpcode::GetState),     0x01u);
    EXPECT_EQ(static_cast<uint32_t>(FocusModeOpcode::Start),        0x02u);
    EXPECT_EQ(static_cast<uint32_t>(FocusModeOpcode::Stop),         0x03u);
    EXPECT_EQ(static_cast<uint32_t>(FocusModeOpcode::StateChanged), 0x04u);
}

TEST(ServiceProtocolOpcodeTest, SmartOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(SmartOpcode::ListDrives), 0x01u);
}

// ── SessionPage capacity contract ────────────────────────────────────────────
//
// The page holds exactly 3 entries.  If the array grows or shrinks, both the
// compositor and xshell's pager must be updated — a size change here catches
// that immediately.

TEST(ServiceProtocolSizeTest, SessionPageHoldsThreeEntries) {
    // entries_ is a C array; verify element count via sizeof arithmetic.
    constexpr size_t entry_count =
        sizeof(SessionPage::entries_) / sizeof(SessionEntry);
    EXPECT_EQ(entry_count, 3u);
}

TEST(ServiceProtocolSizeTest, CrashReportPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(CrashReportPage::entries_) / sizeof(CrashReport);
    EXPECT_EQ(n, 3u);
}

TEST(ServiceProtocolSizeTest, SmartPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(SmartPage::entries_) / sizeof(SmartDrive);
    EXPECT_EQ(n, 3u);
}

// ── Service struct size contracts (Pairs 42–48) ───────────────────────────────

// Pair 42: Battery Drain
TEST(ServiceProtocolSizeTest, BatteryDrainEntryIs64) {
    EXPECT_EQ(sizeof(BatteryDrainEntry), 64u);
}
TEST(ServiceProtocolSizeTest, BatteryDrainPageFitsPayload) {
    EXPECT_LE(sizeof(BatteryDrainPage), 256u);
}
TEST(ServiceProtocolSizeTest, BatteryDrainPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(BatteryDrainPage::entries_) / sizeof(BatteryDrainEntry);
    EXPECT_EQ(n, 3u);
}

// Pair 43: Cron Jobs
TEST(ServiceProtocolSizeTest, CronEntryIs64) {
    EXPECT_EQ(sizeof(CronEntry), 64u);
}
TEST(ServiceProtocolSizeTest, CronPageFitsPayload) {
    EXPECT_LE(sizeof(CronPage), 256u);
}
TEST(ServiceProtocolSizeTest, CronPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(CronPage::entries_) / sizeof(CronEntry);
    EXPECT_EQ(n, 3u);
}
TEST(ServiceProtocolSizeTest, CronRequestsFitPayload) {
    EXPECT_LE(sizeof(CronJobToggleRequest), 256u);
    EXPECT_LE(sizeof(CronRunRequest),       256u);
}

// Pair 44: Location Services
TEST(ServiceProtocolSizeTest, LocationStateIs256) {
    EXPECT_EQ(sizeof(LocationState), 256u);
}
TEST(ServiceProtocolSizeTest, LocationRequestsFitPayload) {
    EXPECT_LE(sizeof(LocationEnableRequest), 256u);
}

// Pair 44: GPU Statistics
TEST(ServiceProtocolSizeTest, GpuStatsIs256) {
    EXPECT_EQ(sizeof(GpuStats), 256u);
}

// Headless Mode
TEST(ServiceProtocolSizeTest, HeadlessInfoIs256) {
    EXPECT_EQ(sizeof(HeadlessInfo), 256u);
}

// Pair 45: Microphone Mute
TEST(ServiceProtocolSizeTest, MicMuteStateIs256) {
    EXPECT_EQ(sizeof(MicMuteState), 256u);
}
TEST(ServiceProtocolSizeTest, MicMuteRequestFitsPayload) {
    EXPECT_LE(sizeof(MicMuteRequest), 256u);
}

// Pair 45: Startup Items
TEST(ServiceProtocolSizeTest, StartupEntryIs64) {
    EXPECT_EQ(sizeof(StartupEntry), 64u);
}
TEST(ServiceProtocolSizeTest, StartupPageFitsPayload) {
    EXPECT_LE(sizeof(StartupPage), 256u);
}
TEST(ServiceProtocolSizeTest, StartupPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(StartupPage::entries_) / sizeof(StartupEntry);
    EXPECT_EQ(n, 3u);
}
TEST(ServiceProtocolSizeTest, StartupRequestsFitPayload) {
    EXPECT_LE(sizeof(StartupToggleRequest), 256u);
    EXPECT_LE(sizeof(StartupRemoveRequest), 256u);
}

// Pair 46: Screen Rotation Lock
TEST(ServiceProtocolSizeTest, RotationLockStateIs256) {
    EXPECT_EQ(sizeof(RotationLockState), 256u);
}
TEST(ServiceProtocolSizeTest, RotationLockRequestsFitPayload) {
    EXPECT_LE(sizeof(RotationLockRequest),        256u);
    EXPECT_LE(sizeof(RotationOrientationRequest), 256u);
}

// Pair 46: System Logs
TEST(ServiceProtocolSizeTest, SyslogEntryIs64) {
    EXPECT_EQ(sizeof(SyslogEntry), 64u);
}
TEST(ServiceProtocolSizeTest, SyslogPageFitsPayload) {
    EXPECT_LE(sizeof(SyslogPage), 256u);
}
TEST(ServiceProtocolSizeTest, SyslogPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(SyslogPage::entries_) / sizeof(SyslogEntry);
    EXPECT_EQ(n, 3u);
}

// Pair 47: Screen Timeout
TEST(ServiceProtocolSizeTest, ScreenTimeoutStateIs256) {
    EXPECT_EQ(sizeof(ScreenTimeoutState), 256u);
}
TEST(ServiceProtocolSizeTest, ScreenTimeoutRequestFitsPayload) {
    EXPECT_LE(sizeof(ScreenTimeoutRequest), 256u);
}

// Pair 47: Mounted Volumes
TEST(ServiceProtocolSizeTest, VolumeEntryIs64) {
    EXPECT_EQ(sizeof(VolumeEntry), 64u);
}
TEST(ServiceProtocolSizeTest, VolumePageFitsPayload) {
    EXPECT_LE(sizeof(VolumePage), 256u);
}
TEST(ServiceProtocolSizeTest, VolumePageHoldsThreeEntries) {
    constexpr size_t n = sizeof(VolumePage::entries_) / sizeof(VolumeEntry);
    EXPECT_EQ(n, 3u);
}

// Pair 48: Audio Output
TEST(ServiceProtocolSizeTest, AudioSinkEntryIs64) {
    EXPECT_EQ(sizeof(AudioSinkEntry), 64u);
}
TEST(ServiceProtocolSizeTest, AudioSinkPageFitsPayload) {
    EXPECT_LE(sizeof(AudioSinkPage), 256u);
}
TEST(ServiceProtocolSizeTest, AudioSinkPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(AudioSinkPage::entries_) / sizeof(AudioSinkEntry);
    EXPECT_EQ(n, 3u);
}
TEST(ServiceProtocolSizeTest, AudioSinkSelectRequestFitsPayload) {
    EXPECT_LE(sizeof(AudioSinkSelectRequest), 256u);
}

// Pair 48: Open Network Connections
TEST(ServiceProtocolSizeTest, ConnEntryIs64) {
    EXPECT_EQ(sizeof(ConnEntry), 64u);
}
TEST(ServiceProtocolSizeTest, ConnPageFitsPayload) {
    EXPECT_LE(sizeof(ConnPage), 256u);
}
TEST(ServiceProtocolSizeTest, ConnPageHoldsThreeEntries) {
    constexpr size_t n = sizeof(ConnPage::entries_) / sizeof(ConnEntry);
    EXPECT_EQ(n, 3u);
}

// ── Alignment: VolumeEntry uint64_t fields must come first ───────────────────
//
// VolumeEntry had an alignment bug during Pair 47 development where char arrays
// preceded uint64_t fields, introducing padding that broke sizeof == 64.
// These tests guard against the same class of bug being reintroduced.

TEST(ServiceProtocolAlignTest, VolumeEntryTotalKbIsFirst) {
    EXPECT_EQ(offsetof(VolumeEntry, total_kb_), 0u)
        << "VolumeEntry::total_kb_ must be the first field (offset 0)";
    EXPECT_EQ(offsetof(VolumeEntry, total_kb_) % 8, 0u);
}
TEST(ServiceProtocolAlignTest, VolumeEntryUsedKbIsAtOffset8) {
    EXPECT_EQ(offsetof(VolumeEntry, used_kb_), 8u)
        << "VolumeEntry::used_kb_ must follow total_kb_ at offset 8";
    EXPECT_EQ(offsetof(VolumeEntry, used_kb_) % 8, 0u);
}
TEST(ServiceProtocolAlignTest, VolumeEntryMountFollowsUint64s) {
    // mount_[32] must start at offset 16 (after the two 8-byte fields).
    EXPECT_EQ(offsetof(VolumeEntry, mount_), 16u);
}

// ── Opcode stability (Pairs 42–48) ───────────────────────────────────────────

TEST(ServiceProtocolOpcodeTest, BatteryDrainOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(BatteryDrainOpcode::ListDrain),    0x01u);
    EXPECT_EQ(static_cast<uint32_t>(BatteryDrainOpcode::RefreshDrain), 0x02u);
    EXPECT_EQ(static_cast<uint32_t>(BatteryDrainOpcode::DrainChanged), 0x03u);
}
TEST(ServiceProtocolOpcodeTest, BatteryDrainImpactEnum) {
    EXPECT_EQ(static_cast<uint32_t>(BatteryDrainImpact::Low),    0u);
    EXPECT_EQ(static_cast<uint32_t>(BatteryDrainImpact::Medium), 1u);
    EXPECT_EQ(static_cast<uint32_t>(BatteryDrainImpact::High),   2u);
}

TEST(ServiceProtocolOpcodeTest, CronOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(CronOpcode::ListJobs),      0x01u);
    EXPECT_EQ(static_cast<uint32_t>(CronOpcode::SetJobEnabled), 0x02u);
    EXPECT_EQ(static_cast<uint32_t>(CronOpcode::RunNow),        0x03u);
    EXPECT_EQ(static_cast<uint32_t>(CronOpcode::JobChanged),    0x04u);
}

TEST(ServiceProtocolOpcodeTest, LocationOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(LocationOpcode::GetState),    0x01u);
    EXPECT_EQ(static_cast<uint32_t>(LocationOpcode::SetEnabled),  0x02u);
    EXPECT_EQ(static_cast<uint32_t>(LocationOpcode::StateChanged),0x03u);
}

TEST(ServiceProtocolOpcodeTest, GpuOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(GpuOpcode::GetStats), 0x01u);
}

TEST(ServiceProtocolOpcodeTest, HeadlessOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(HeadlessOpcode::GetInfo),      0x01u);
    EXPECT_EQ(static_cast<uint32_t>(HeadlessOpcode::CaptureFrame), 0x02u);
}

TEST(ServiceProtocolOpcodeTest, MicMuteOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(MicMuteOpcode::GetState),    0x01u);
    EXPECT_EQ(static_cast<uint32_t>(MicMuteOpcode::SetMuted),    0x02u);
    EXPECT_EQ(static_cast<uint32_t>(MicMuteOpcode::StateChanged),0x03u);
}

TEST(ServiceProtocolOpcodeTest, StartupOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(StartupOpcode::ListItems),    0x01u);
    EXPECT_EQ(static_cast<uint32_t>(StartupOpcode::SetEnabled),   0x02u);
    EXPECT_EQ(static_cast<uint32_t>(StartupOpcode::RemoveItem),   0x03u);
    EXPECT_EQ(static_cast<uint32_t>(StartupOpcode::ItemsChanged), 0x04u);
}

TEST(ServiceProtocolOpcodeTest, RotationLockOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(RotationLockOpcode::GetState),       0x01u);
    EXPECT_EQ(static_cast<uint32_t>(RotationLockOpcode::SetLocked),      0x02u);
    EXPECT_EQ(static_cast<uint32_t>(RotationLockOpcode::SetOrientation), 0x03u);
    EXPECT_EQ(static_cast<uint32_t>(RotationLockOpcode::StateChanged),   0x04u);
}

TEST(ServiceProtocolOpcodeTest, SyslogOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(SyslogOpcode::GetEntries), 0x01u);
}

TEST(ServiceProtocolOpcodeTest, ScreenTimeoutOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(ScreenTimeoutOpcode::GetTimeout),     0x01u);
    EXPECT_EQ(static_cast<uint32_t>(ScreenTimeoutOpcode::SetTimeout),     0x02u);
    EXPECT_EQ(static_cast<uint32_t>(ScreenTimeoutOpcode::TimeoutChanged), 0x03u);
}

TEST(ServiceProtocolOpcodeTest, MountedVolOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(MountedVolOpcode::ListVolumes), 0x01u);
}

TEST(ServiceProtocolOpcodeTest, AudioOutputOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(AudioOutputOpcode::ListSinks),     0x01u);
    EXPECT_EQ(static_cast<uint32_t>(AudioOutputOpcode::SetActiveSink), 0x02u);
    EXPECT_EQ(static_cast<uint32_t>(AudioOutputOpcode::SinkChanged),   0x03u);
}

TEST(ServiceProtocolOpcodeTest, NetConnOpcodes) {
    EXPECT_EQ(static_cast<uint32_t>(NetConnOpcode::ListConns), 0x01u);
}

// ── XBus round-trip tests (Pairs 42–48) ──────────────────────────────────────
//
// Each test embeds a service struct in an XBusMessage, copies it through a raw
// byte buffer (simulating the IPC read path), then extracts and verifies fields.

TEST(ServiceProtocolRoundTripTest, BatteryDrainPageRoundTrip) {
    BatteryDrainEntry e{};
    std::strncpy(e.app_name_, "Firefox", sizeof(e.app_name_) - 1);
    e.energy_pct_ = 42;
    e.impact_     = static_cast<uint32_t>(BatteryDrainImpact::High);
    e.pid_        = 1234;

    BatteryDrainPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(BatteryDrainOpcode::ListDrain);
    std::strncpy(msg.service_name_, "com.freebsdx.BatteryDrain",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    BatteryDrainPage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_STREQ(pg_out.entries_[0].app_name_, "Firefox");
    EXPECT_EQ(pg_out.entries_[0].energy_pct_, 42u);
    EXPECT_EQ(pg_out.entries_[0].impact_,
              static_cast<uint32_t>(BatteryDrainImpact::High));
    EXPECT_EQ(pg_out.entries_[0].pid_, 1234u);
}

TEST(ServiceProtocolRoundTripTest, CronPageRoundTrip) {
    CronEntry e{};
    std::strncpy(e.schedule_, "0 3 * * *",  sizeof(e.schedule_) - 1);
    std::strncpy(e.command_,  "pkg upgrade", sizeof(e.command_)  - 1);
    e.job_id_  = 7;
    e.enabled_ = 1;

    CronPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(CronOpcode::ListJobs);
    std::strncpy(msg.service_name_, "com.freebsdx.CronJobs",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    CronPage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_STREQ(pg_out.entries_[0].schedule_, "0 3 * * *");
    EXPECT_STREQ(pg_out.entries_[0].command_,  "pkg upgrade");
    EXPECT_EQ(pg_out.entries_[0].job_id_,      7u);
    EXPECT_EQ(pg_out.entries_[0].enabled_,     1u);
}

TEST(ServiceProtocolRoundTripTest, LocationStateRoundTrip) {
    LocationState ls{};
    ls.enabled_          = 1;
    ls.requesting_count_ = 2;
    std::strncpy(ls.last_app_, "Maps", sizeof(ls.last_app_) - 1);

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(LocationOpcode::StateChanged);
    std::strncpy(msg.service_name_, "com.freebsdx.Location",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &ls, sizeof(ls));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    LocationState out{};
    std::memcpy(&out, in.payload_, sizeof(out));

    EXPECT_EQ(out.enabled_,          1u);
    EXPECT_EQ(out.requesting_count_, 2u);
    EXPECT_STREQ(out.last_app_,      "Maps");
}

TEST(ServiceProtocolRoundTripTest, GpuStatsRoundTrip) {
    GpuStats gs{};
    gs.usage_pct_     = 87;
    gs.vram_used_mb_  = 3072;
    gs.vram_total_mb_ = 8192;
    gs.temp_c_        = 74;
    gs.clock_mhz_     = 1750;
    std::strncpy(gs.name_, "AMD Radeon RX 580", sizeof(gs.name_) - 1);

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(GpuOpcode::GetStats);
    std::strncpy(msg.service_name_, "com.freebsdx.GpuStats",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &gs, sizeof(gs));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    GpuStats out{};
    std::memcpy(&out, in.payload_, sizeof(out));

    EXPECT_EQ(out.usage_pct_,     87u);
    EXPECT_EQ(out.vram_used_mb_,  3072u);
    EXPECT_EQ(out.vram_total_mb_, 8192u);
    EXPECT_EQ(out.temp_c_,        74);
    EXPECT_EQ(out.clock_mhz_,     1750u);
    EXPECT_STREQ(out.name_,       "AMD Radeon RX 580");
}

TEST(ServiceProtocolRoundTripTest, GpuStatsTempUnavailableSentinel) {
    GpuStats gs{};
    gs.temp_c_ = INT32_MIN;

    XBusMessage msg{};
    msg.magic_ = XBUS_MAGIC;
    std::memcpy(msg.payload_, &gs, sizeof(gs));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    GpuStats out{};
    std::memcpy(&out, in.payload_, sizeof(out));

    EXPECT_EQ(out.temp_c_, INT32_MIN) << "INT32_MIN sentinel must survive IPC round-trip";
}

TEST(ServiceProtocolRoundTripTest, HeadlessInfoRoundTrip) {
    HeadlessInfo hi{};
    hi.headless_    = 1;
    hi.width_       = 1920;
    hi.height_      = 1080;
    hi.frame_count_ = 42;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(HeadlessOpcode::GetInfo);
    std::strncpy(msg.service_name_, "com.freebsdx.Headless",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &hi, sizeof(hi));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    HeadlessInfo out{};
    std::memcpy(&out, in.payload_, sizeof(out));

    EXPECT_EQ(out.headless_,    1u);
    EXPECT_EQ(out.width_,       1920u);
    EXPECT_EQ(out.height_,      1080u);
    EXPECT_EQ(out.frame_count_, 42u);
}

TEST(ServiceProtocolRoundTripTest, MicMuteStateRoundTrip) {
    MicMuteState ms{};
    ms.muted_ = 1;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(MicMuteOpcode::StateChanged);
    std::strncpy(msg.service_name_, "com.freebsdx.MicMute",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &ms, sizeof(ms));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    MicMuteState out{};
    std::memcpy(&out, in.payload_, sizeof(out));
    EXPECT_EQ(out.muted_, 1u);
}

TEST(ServiceProtocolRoundTripTest, StartupPageRoundTrip) {
    StartupEntry e{};
    std::strncpy(e.name_,  "Dropbox",  sizeof(e.name_)  - 1);
    std::strncpy(e.exec_,  "dropbox",  sizeof(e.exec_)  - 1);
    e.item_id_ = 3;
    e.enabled_ = 1;
    e.builtin_ = 0;

    StartupPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(StartupOpcode::ListItems);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    StartupPage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_STREQ(pg_out.entries_[0].name_,    "Dropbox");
    EXPECT_STREQ(pg_out.entries_[0].exec_,    "dropbox");
    EXPECT_EQ(pg_out.entries_[0].item_id_,    3u);
    EXPECT_EQ(pg_out.entries_[0].enabled_,    1u);
    EXPECT_EQ(pg_out.entries_[0].builtin_,    0u);
}

TEST(ServiceProtocolRoundTripTest, RotationLockStateRoundTrip) {
    RotationLockState rls{};
    rls.locked_      = 1;
    rls.orientation_ = 1; // portrait

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(RotationLockOpcode::StateChanged);
    std::strncpy(msg.service_name_, "com.freebsdx.RotationLock",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &rls, sizeof(rls));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    RotationLockState out{};
    std::memcpy(&out, in.payload_, sizeof(out));

    EXPECT_EQ(out.locked_,      1u);
    EXPECT_EQ(out.orientation_, 1u);
}

TEST(ServiceProtocolRoundTripTest, SyslogPageRoundTrip) {
    SyslogEntry e{};
    e.timestamp_ = 1721500000u;
    std::strncpy(e.level_,   "ERROR",   sizeof(e.level_)   - 1);
    std::strncpy(e.source_,  "kernel",  sizeof(e.source_)  - 1);
    std::strncpy(e.message_, "panic: out of memory", sizeof(e.message_) - 1);

    SyslogPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(SyslogOpcode::GetEntries);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    SyslogPage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_,                      1u);
    EXPECT_EQ(pg_out.entries_[0].timestamp_,      1721500000u);
    EXPECT_STREQ(pg_out.entries_[0].level_,       "ERROR");
    EXPECT_STREQ(pg_out.entries_[0].source_,      "kernel");
    EXPECT_STREQ(pg_out.entries_[0].message_,     "panic: out of memory");
}

TEST(ServiceProtocolRoundTripTest, ScreenTimeoutStateRoundTrip) {
    ScreenTimeoutState st{};
    st.timeout_secs_ = 120;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(ScreenTimeoutOpcode::TimeoutChanged);
    std::strncpy(msg.service_name_, "com.freebsdx.ScreenTimeout",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &st, sizeof(st));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    ScreenTimeoutState out{};
    std::memcpy(&out, in.payload_, sizeof(out));

    EXPECT_EQ(out.timeout_secs_, 120u);
}

TEST(ServiceProtocolRoundTripTest, ScreenTimeoutNeverSentinel) {
    ScreenTimeoutState st{};
    st.timeout_secs_ = 0; // 0 = never

    XBusMessage msg{};
    msg.magic_ = XBUS_MAGIC;
    std::memcpy(msg.payload_, &st, sizeof(st));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    ScreenTimeoutState out{};
    std::memcpy(&out, in.payload_, sizeof(out));
    EXPECT_EQ(out.timeout_secs_, 0u) << "timeout_secs_==0 (never) must survive round-trip";
}

TEST(ServiceProtocolRoundTripTest, VolumePageRoundTrip) {
    VolumeEntry e{};
    e.total_kb_ = 1073741824ULL; // 1 TiB in KiB
    e.used_kb_  = 536870912ULL;  // 512 GiB in KiB
    std::strncpy(e.mount_,   "/",    sizeof(e.mount_)   - 1);
    std::strncpy(e.fstype_,  "zfs",  sizeof(e.fstype_)  - 1);
    e.readonly_ = 0;

    VolumePage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(MountedVolOpcode::ListVolumes);
    std::strncpy(msg.service_name_, "com.freebsdx.MountedVolumes",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    VolumePage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_EQ(pg_out.entries_[0].total_kb_, 1073741824ULL);
    EXPECT_EQ(pg_out.entries_[0].used_kb_,  536870912ULL);
    EXPECT_STREQ(pg_out.entries_[0].mount_,  "/");
    EXPECT_STREQ(pg_out.entries_[0].fstype_, "zfs");
    EXPECT_EQ(pg_out.entries_[0].readonly_,  0u);
}

TEST(ServiceProtocolRoundTripTest, AudioSinkPageRoundTrip) {
    AudioSinkEntry e{};
    std::strncpy(e.name_, "Built-in Speakers", sizeof(e.name_) - 1);
    std::strncpy(e.id_,   "hdaudio0",          sizeof(e.id_)   - 1);
    e.active_ = 1;

    AudioSinkPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(AudioOutputOpcode::ListSinks);
    std::strncpy(msg.service_name_, "com.freebsdx.AudioOutput",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    AudioSinkPage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_STREQ(pg_out.entries_[0].name_,    "Built-in Speakers");
    EXPECT_STREQ(pg_out.entries_[0].id_,      "hdaudio0");
    EXPECT_EQ(pg_out.entries_[0].active_,     1u);
}

TEST(ServiceProtocolRoundTripTest, ConnPageRoundTrip) {
    ConnEntry e{};
    std::strncpy(e.proto_,  "TCP",              sizeof(e.proto_)  - 1);
    std::strncpy(e.local_,  "127.0.0.1:8080",  sizeof(e.local_)  - 1);
    std::strncpy(e.remote_, "10.0.0.1:443",    sizeof(e.remote_) - 1);
    std::strncpy(e.state_,  "ESTABL",           sizeof(e.state_)  - 1);
    e.pid_ = 4242;

    ConnPage pg{};
    pg.page_ = 0; pg.total_ = 1; pg.count_ = 1; pg.has_more_ = 0;
    pg.entries_[0] = e;

    XBusMessage msg{};
    msg.magic_  = XBUS_MAGIC;
    msg.opcode_ = static_cast<uint32_t>(NetConnOpcode::ListConns);
    std::strncpy(msg.service_name_, "com.freebsdx.NetConns",
                 sizeof(msg.service_name_) - 1);
    std::memcpy(msg.payload_, &pg, sizeof(pg));

    alignas(XBusMessage) uint8_t buf[sizeof(XBusMessage)];
    std::memcpy(buf, &msg, sizeof(msg));
    XBusMessage in{};
    std::memcpy(&in, buf, sizeof(in));

    ConnPage pg_out{};
    std::memcpy(&pg_out, in.payload_, sizeof(pg_out));

    EXPECT_EQ(pg_out.count_, 1u);
    EXPECT_STREQ(pg_out.entries_[0].proto_,  "TCP");
    EXPECT_STREQ(pg_out.entries_[0].local_,  "127.0.0.1:8080");
    EXPECT_STREQ(pg_out.entries_[0].remote_, "10.0.0.1:443");
    EXPECT_STREQ(pg_out.entries_[0].state_,  "ESTABL");
    EXPECT_EQ(pg_out.entries_[0].pid_,       4242u);
}

} // namespace fbx::shared::testing
