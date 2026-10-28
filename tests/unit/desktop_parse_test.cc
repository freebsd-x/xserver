// Manifests, evdev controllers, backup jobs, and the EQ's frequency response.

#include "xserver/desktop_parse.h"
#include "xserver/audio_eq.h"
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

using namespace fbx::xserver;

TEST(Manifest, JsonStrings) {
    std::string_view m = R"({ "name": "Camera", "identifier": "com.freebsdx.camera",
                              "quote": "say \"hi\"", "n": 3 })";
    EXPECT_EQ(desktop::jsonString(m, "name"), "Camera");
    EXPECT_EQ(desktop::jsonString(m, "identifier"), "com.freebsdx.camera");
    EXPECT_EQ(desktop::jsonString(m, "quote"), "say \"hi\"");
    EXPECT_EQ(desktop::jsonString(m, "missing"), "");
}

// Real output from this laptop, plus a USB and a Bluetooth pad.
constexpr std::string_view kEvdev =
    "kern.evdev.input.7.uniq: \n"
    "kern.evdev.input.7.phys: hmt0\n"
    "kern.evdev.input.7.id: { bustype = 0x0018, vendor = 0x04f3, product = 0x309c, version = 0x0001 }\n"
    "kern.evdev.input.7.name: ELAN1200:01 04F3:309C TouchPad\n"
    "kern.evdev.input.5.name: Control Method Lid Switch\n"
    "kern.evdev.input.5.id: { bustype = 0x0019, vendor = 0x0000, product = 0x0000, version = 0x0001 }\n"
    "kern.evdev.input.9.name: Microsoft X-Box 360 pad\n"
    "kern.evdev.input.9.phys: usb-0000:00:14.0-2/input0\n"
    "kern.evdev.input.9.id: { bustype = 0x0003, vendor = 0x045e, product = 0x028e, version = 0x0110 }\n"
    "kern.evdev.input.10.name: Wireless Controller\n"
    "kern.evdev.input.10.uniq: a4:ae:12:00:11:22\n"
    "kern.evdev.input.10.id: { bustype = 0x0005, vendor = 0x054c, product = 0x09cc, version = 0x8100 }\n"
    "kern.evdev.input.11.name: Wireless Controller Touchpad\n"
    "kern.evdev.input.11.id: { bustype = 0x0005, vendor = 0x054c, product = 0x09cc, version = 0x8100 }\n"
    "kern.evdev.input.4.name: AT keyboard\n";

TEST(GameControllers, ParsesSysctlAndPicksPads) {
    auto devs = desktop::parseEvdevSysctl(kEvdev);
    ASSERT_EQ(devs.size(), 6u);
    EXPECT_EQ(devs[0].index, 4u);                      // index order
    const auto& tp = devs[2];
    EXPECT_EQ(tp.index, 7u);
    EXPECT_EQ(tp.vendor, 0x04f3u);
    EXPECT_EQ(tp.bustype, 0x18u);
    EXPECT_EQ(tp.phys, "hmt0");

    std::vector<std::string> pads;
    for (const auto& d : devs)
        if (desktop::isGameController(d)) pads.push_back(d.name);
    ASSERT_EQ(pads.size(), 2u);                         // not the touchpad, lid or keyboard
    EXPECT_EQ(pads[0], "Microsoft X-Box 360 pad");
    EXPECT_EQ(pads[1], "Wireless Controller");          // DualShock, not its touchpad
    EXPECT_EQ(devs[4].bustype, desktop::kBusBluetooth);
    EXPECT_NE(desktop::controllerId(devs[4]), desktop::controllerId(devs[5]));
}

TEST(Backup, JobsDefaultAndParse) {
    auto d = desktop::parseBackupJobs("", "/home/u");
    ASSERT_EQ(d.size(), 1u);
    EXPECT_EQ(d[0].source, "/home/u");
    EXPECT_EQ(d[0].destination, "/home/u/Backups");

    auto j = desktop::parseBackupJobs("# mine\nDocs\t~/Documents\t/mnt/usb/backups\n"
                                      "bad\trelative\t/x\nNoDest\t/a\n", "/home/u");
    ASSERT_EQ(j.size(), 1u);
    EXPECT_EQ(j[0].name, "Docs");
    EXPECT_EQ(j[0].source, "/home/u/Documents");
    EXPECT_EQ(desktop::jobId(j[0]).size(), 8u);
    EXPECT_EQ(desktop::slug("Home folder"), "home-folder");
    EXPECT_EQ(desktop::slug("  !!"), "backup");
    EXPECT_EQ(desktop::relativeInside("/home/u", "/home/u/Backups"), "Backups");
    EXPECT_EQ(desktop::relativeInside("/home/u", "/home/user2"), "");
    EXPECT_EQ(desktop::relativeInside("/home/u", "/mnt/usb"), "");
}

// ── EQ ───────────────────────────────────────────────────────────────────────

namespace {
// Steady-state gain (dB) of the EQ at `hz`, measured with a sine.
double gainAt(const eq::Gains& g, double hz) {
    const double rate = 48000;
    eq::Equalizer e;
    e.configure(g, rate);
    const size_t n = 48000;
    std::vector<int32_t> buf(n * 2);
    for (size_t i = 0; i < n; ++i)
        buf[i * 2] = buf[i * 2 + 1] = static_cast<int32_t>(8000 * std::sin(2 * M_PI * hz * i / rate));
    e.process(buf.data(), n);
    double in = 0, out = 0;
    for (size_t i = n / 2; i < n; ++i) {                  // skip the transient
        double x = 8000 * std::sin(2 * M_PI * hz * i / rate);
        in += x * x;
        out += static_cast<double>(buf[i * 2]) * buf[i * 2];
    }
    return 10 * std::log10(out / in);
}
}  // namespace

TEST(AudioEq, FlatIsBitExact) {
    eq::Equalizer e;
    e.configure(eq::presetGains(eq::kFlat), 48000);
    EXPECT_TRUE(e.flat());
    std::vector<int32_t> buf{1, -2, 3000, -32768};
    e.process(buf.data(), 2);
    EXPECT_EQ(buf, (std::vector<int32_t>{1, -2, 3000, -32768}));
}

TEST(AudioEq, BandsHitTheirGainAtCenter) {
    EXPECT_NEAR(gainAt({0, 0, 6, 0, 0}, 1000), 6.0, 0.3);     // mid peak
    EXPECT_NEAR(gainAt({0, 0, -9, 0, 0}, 1000), -9.0, 0.3);
    EXPECT_NEAR(gainAt({0, 0, 6, 0, 0}, 10000), 0.0, 0.5);    // far from the mid band
    EXPECT_NEAR(gainAt({12, 0, 0, 0, 0}, 30), 12.0, 0.5);     // low shelf, well below 80 Hz
    EXPECT_NEAR(gainAt({0, 0, 0, 0, 6}, 18000), 6.0, 0.5);    // high shelf, well above 10 kHz
}

TEST(AudioEq, PresetsAndClamp) {
    EXPECT_EQ(eq::presetGains(eq::kBassBoost)[0], 6);
    EXPECT_EQ(eq::presetGains(99), eq::presetGains(eq::kFlat));
    auto g = eq::clampGains({40, -40, 0, 12, -12});
    EXPECT_EQ(g[0], 12);
    EXPECT_EQ(g[1], -12);
}

// ── Audio routing ────────────────────────────────────────────────────────────

#include "xserver/audio_route.h"
#include "shared/autostart.h"

TEST(AudioRoute, ParsesSndstatPlaybackDevices) {
    auto outs = audioroute::parseSndstat(
        "Installed devices:\n"
        "pcm0: <Realtek ALC294 (Analog 2.0+HP/4ch)> (play/rec)\n"
        "pcm1: <Intel Kaby Lake (HDMI/DP 8ch)> (play) default\n"
        "pcm2: <USB Mic> (rec)\n"
        "No devices installed from userspace.\n");
    ASSERT_EQ(outs.size(), 2u);                        // the capture-only one is skipped
    EXPECT_EQ(outs[0].path(), "/dev/dsp0");
    EXPECT_FALSE(outs[0].is_default);
    EXPECT_TRUE(outs[1].is_default);
    EXPECT_EQ(audioroute::shortLabel(outs[0]), "Analog (pcm0)");
    EXPECT_EQ(audioroute::shortLabel(outs[1]), "HDMI/DP (pcm1)");
    EXPECT_LT(audioroute::shortLabel(outs[1]).size(), 32u);
}

TEST(AudioRoute, RoutesPersistAndResolve) {
    auto r = audioroute::parseRoutes("com.freebsdx.musicplayer=/dev/dsp0;bad=/etc/passwd;x=/dev/dspX;y=/dev/dsp1");
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(audioroute::parseRoutes(audioroute::formatRoutes(r)), r);
    std::vector<audioroute::Output> outs{{0, "A", false}, {1, "H", true}};
    EXPECT_EQ(audioroute::resolve(r, "com.freebsdx.musicplayer", outs), "/dev/dsp0");
    EXPECT_EQ(audioroute::resolve(r, "unknown", outs), "");
    outs.pop_back();
    EXPECT_EQ(audioroute::resolve(r, "y", outs), "");  // its device is gone → default
    EXPECT_FALSE(audioroute::validApp("a=b"));
}

TEST(LoginItems, AppEntryLaunchesByBundleId) {
    namespace as = fbx::shared::autostart;
    std::string text = as::appEntry("Notes", "/apps/Notes.xapp/bin/notes", "com.freebsdx.notes");
    as::Entry e;
    ASSERT_TRUE(as::parse(text, e));
    EXPECT_EQ(e.app_id, "com.freebsdx.notes");
    EXPECT_EQ(e.name, "Notes");
    EXPECT_TRUE(as::shouldLaunch(e));
    EXPECT_TRUE(as::isEntryFile(as::appEntryFile("com.freebsdx.notes")));
}

// ── Shortcut registry / old-cache rule ───────────────────────────────────────

#include "xserver/shortcuts.h"
#include "xserver/opt_storage_service.h"

TEST(Shortcuts, RegistryFitsTheWireAndToggles) {
    for (const auto& d : shortcuts::kAll) {
        EXPECT_LT(std::strlen(d.action), 48u) << d.action;
        EXPECT_LT(std::strlen(d.binding), 16u) << d.binding;
        EXPECT_EQ(shortcuts::byAction(d.action), d.id);
    }
    EXPECT_EQ(shortcuts::byAction("Mission Control"), shortcuts::kCount);   // not a real shortcut here
    shortcuts::g_disabled = 0;
    EXPECT_TRUE(shortcuts::enabled(shortcuts::kLock));
    shortcuts::setEnabled(shortcuts::kLock, false);
    EXPECT_FALSE(shortcuts::enabled(shortcuts::kLock));
    EXPECT_TRUE(shortcuts::enabled(shortcuts::kClose));
    shortcuts::setEnabled(shortcuts::kLock, true);
    EXPECT_EQ(shortcuts::g_disabled.load(), 0u);
}

TEST(OptStorage, StaleMeansUntouchedFor30Days) {
    const std::time_t now = 2'000'000'000, day = 86400;
    EXPECT_TRUE(OptStorageService::isStale(now - 31 * day, now - 31 * day, now));
    EXPECT_FALSE(OptStorageService::isStale(now - 31 * day, now - 2 * day, now));   // read recently
    EXPECT_FALSE(OptStorageService::isStale(now - 2 * day, now - 31 * day, now));   // written recently
}
