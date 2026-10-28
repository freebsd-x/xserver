#include "xserver/xbus_dispatcher.h"
#include "xserver/vulkan_compositor.h"
#include "xserver/display_service.h"
#include "xserver/drm_device.h"
#include "xserver/mime_service.h"
#include "xserver/app_manager_service.h"
#include "xserver/audio_service.h"
#include "xserver/storage_service.h"
#include "xserver/update_service.h"
#include "xserver/automation_service.h"
#include "xserver/system_service.h"
#include "xserver/network_service.h"
#include "xserver/vpn_service.h"
#include "xserver/screenshot_service.h"
#include "xserver/headless_service.h"
#include "xserver/login_service.h"
#include "xserver/lock_service.h"
#include "xserver/screensaver_service.h"
#include "xserver/terminal_service.h"
#include "xserver/pkg_signing_service.h"
#include "xserver/xwayland_service.h"
#include "xserver/remote_desktop_service.h"
#include "xserver/bookmark_service.h"
#include "xserver/calendar_service.h"
#include "xserver/config_service.h"
#include "xserver/clipboard_service.h"
#include "xserver/notification_service.h"
#include "xserver/portal_service.h"
#include "xserver/print_service.h"
#include "xserver/hardware_service.h"
#include "xserver/power_service.h"
#include "xserver/backlight_service.h"
#include "xserver/bluetooth_service.h"
#include "xserver/ime_service.h"
#include "xserver/accessibility_service.h"
#include "xserver/security_context_service.h"
#include "xserver/power_profile_service.h"
#include "xserver/seat_service.h"
#include "xserver/virtual_keyboard_service.h"
#include "xserver/camera_service.h"
#include "xserver/tray_service.h"
#include "xserver/file_chooser_service.h"
#include "xserver/clipboard_history_service.h"
#include "xserver/crash_reporter_service.h"
#include "xserver/security_events_service.h"
#include "xserver/app_sandbox_service.h"
#include "xserver/locale_service.h"
#include "xserver/session_service.h"
#include "xserver/netconn_service.h"
#include "xserver/netstat_service.h"
#include "xserver/sysinfo_service.h"
#include "xserver/mounted_vol_service.h"
#include "xserver/process_service.h"
#include "xserver/syslog_service.h"
#include "xserver/packages_service.h"
#include "xserver/pkgupdates_service.h"
#include "xserver/usb_service.h"
#include "xserver/smart_service.h"
#include "xserver/kmod_service.h"
#include "xserver/sysadmin_services.h"
#include "xserver/user_services.h"
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
#include "xserver/display_arrange_service.h"
#include "xserver/airplane_mode_service.h"
#include "xserver/datasaver_service.h"
#include "xserver/dnd_service.h"
#include "xserver/nightlight_service.h"
#include "xserver/reduce_motion_service.h"
#include "xserver/rotation_lock_service.h"
#include "xserver/wakelock_service.h"
#include "xserver/gamemode_service.h"
#include "xserver/micmute_service.h"
#include "xserver/scrolldir_service.h"
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
#include "xserver/touchpad_service.h"
#include "xserver/display_scale_service.h"
#include "xserver/system_sounds_service.h"
#include "xserver/privacy_service.h"
#include "xserver/focus_mode_service.h"
#include "xserver/auto_brightness_service.h"
#include "xserver/refresh_rate_service.h"
#include "xserver/dock_service.h"
#include "xserver/gesture_service.h"
#include "xserver/a11y_settings_service.h"
#include "xserver/update_channel_service.h"
#include "xserver/spatial_audio_service.h"
#include "xserver/stylus_service.h"
#include "xserver/dns_service.h"
#include "xserver/proxy_service.h"
#include "xserver/savedwifi_service.h"
#include "xserver/wol_service.h"
#include "xserver/contacts_service.h"
#include "xserver/keychain_service.h"
#include "xserver/notes_service.h"
#include "xserver/archive_service.h"
#include "xserver/media_player_service.h"
#include "xserver/weather_service.h"
#include "xserver/disk_service.h"
#include "xserver/font_manager_service.h"
#include "xserver/night_shift_service.h"
#include "xserver/mono_audio_service.h"
#include "xserver/true_tone_service.h"
#include "xserver/opt_storage_service.h"
#include "xserver/stage_manager_service.h"
#include "xserver/trusted_devices_service.h"
#include "xserver/login_items_service.h"
#include "xserver/default_apps_service.h"
#include "xserver/keyboard_shortcuts_service.h"
#include "xserver/ipc_socket.h"
#include <iostream>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <cstdlib>
#include <unistd.h>

namespace fbx::xserver {

// Config values are user-persisted/editable; stoul throws on non-numeric input,
// which would escape the dispatch path. Parse defensively.
static uint32_t safeConfigU32(const std::string& s) noexcept {
    try { return static_cast<uint32_t>(std::stoul(s)); } catch (...) { return 0u; }
}

// Services only the trusted shell may call (see the gate in ProcessMessage).
using ServiceHandler = void (*)(const shared::XBusMessage&, std::shared_ptr<IpcSocket>) noexcept;

static ServiceHandler shellOnlyService(const char* name) noexcept {
    static constexpr struct { const char* name; ServiceHandler fn; } kTable[] = {
        {"com.freebsdx.Services", &RcServicesService::handleMessage},
        {"com.freebsdx.Jails",    &JailsService::handleMessage},
        {"com.freebsdx.ZfsPools", &ZfsPoolsService::handleMessage},
        {"com.freebsdx.BootEnv",  &BootEnvService::handleMessage},
        {"com.freebsdx.NTP",      &NtpService::handleMessage},
        {"com.freebsdx.CronJobs", &CronJobsService::handleMessage},
        {"com.freebsdx.SshKeys",  &SshKeysService::handleMessage},
        {"com.freebsdx.Captures", &CapturesService::handleMessage},
        {"com.freebsdx.EnvVars",  &EnvVarsService::handleMessage},
    };
    for (const auto& e : kTable)
        if (std::strcmp(name, e.name) == 0) return e.fn;
    return nullptr;
}

// ─── DnD state ───────────────────────────────────────────────────────────────

static DnDDragState g_dnd;

DnDDragState& GetDnDState() noexcept { return g_dnd; }
void          ClearDnDState() noexcept { g_dnd = {}; }

void DnDSendToClient(std::shared_ptr<IpcSocket> client,
                     shared::DnDOpcode opcode,
                     float x, float y,
                     const char* mime_type,
                     const char* data) noexcept {
    if (!client) return;
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.DnD",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(opcode);

    shared::DnDEvent ev{};
    ev.x_ = x;
    ev.y_ = y;
    if (mime_type) std::strncpy(ev.mime_type_, mime_type, sizeof(ev.mime_type_) - 1);
    if (data)      std::strncpy(ev.data_,      data,      sizeof(ev.data_)      - 1);
    std::memcpy(msg.payload_, &ev, sizeof(ev));

    (void)client->sendMessage(
        {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
}

// ─── Dispatcher statics ───────────────────────────────────────────────────────

XBusDispatcher::ShutdownCallback    XBusDispatcher::shutdown_cb_     = nullptr;
XBusDispatcher::CursorLockCallback  XBusDispatcher::cursor_lock_cb_  = nullptr;
XBusDispatcher::CursorShapeCallback XBusDispatcher::cursor_shape_cb_ = nullptr;
XBusDispatcher::RedrawCallback      XBusDispatcher::redraw_cb_       = nullptr;
XBusDispatcher::RaiseCallback       XBusDispatcher::raise_cb_        = nullptr;

static std::vector<std::weak_ptr<IpcSocket>> g_active_clients;
static std::mutex g_clients_mutex;
static bool g_dark_mode  = false;
static int  g_accent_idx = 0; // 0=Blue, 1=Purple, 2=Orange

static std::string GetThemeConfigPath() {
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) + "/.freebsdx-theme";
}

void LoadTheme() {
    std::ifstream f(GetThemeConfigPath());
    if (f.is_open()) {
        int dark = 0, accent = 0;
        f >> dark >> accent;
        g_dark_mode  = (dark != 0);
        g_accent_idx = (accent >= 0 && accent <= 2) ? accent : 0;
    }
}

void SaveTheme() {
    std::ofstream f(GetThemeConfigPath());
    if (f.is_open()) {
        f << (g_dark_mode ? 1 : 0) << " " << g_accent_idx;
    }
}

void RegisterClientForBroadcast(std::shared_ptr<IpcSocket> client) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    g_active_clients.push_back(client);
}

void BroadcastThemeChanged() {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::InputMessage theme_msg{
        .type_    = shared::InputEventType::ThemeChanged,
        .value_a_ = g_dark_mode ? 1 : 0,
        .value_b_ = g_accent_idx,  // accent index piggybacked here
    };
    std::span<const uint8_t> payload_span(reinterpret_cast<const uint8_t*>(&theme_msg), sizeof(theme_msg));
    for (auto it = g_active_clients.begin(); it != g_active_clients.end();) {
        if (auto client = it->lock()) { (void)client->sendMessage(payload_span); ++it; }
        else it = g_active_clients.erase(it);
    }
}

void InjectInputAction(const shared::InputMessage& input_msg) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    std::span<const uint8_t> payload_span(reinterpret_cast<const uint8_t*>(&input_msg), sizeof(input_msg));
    for (auto it = g_active_clients.begin(); it != g_active_clients.end();) {
        if (auto client = it->lock()) { (void)client->sendMessage(payload_span); ++it; }
        else it = g_active_clients.erase(it);
    }
}

static void broadcastXBusMsg(const shared::XBusMessage& msg) noexcept {
    // Helper: send an XBusMessage to all registered broadcast clients
    std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(&msg), sizeof(msg));
    for (auto it = g_active_clients.begin(); it != g_active_clients.end();) {
        if (auto client = it->lock()) { (void)client->sendMessage(span); ++it; }
        else it = g_active_clients.erase(it);
    }
}

void BroadcastNotification(const shared::NotificationPayload& payload) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::XBusMessage msg{};
    msg.transaction_id_ = 0; // broadcast — no pending request
    std::strncpy(msg.service_name_, "com.freebsdx.Notification", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::NotificationOpcode::Post);
    std::memcpy(msg.payload_, &payload, sizeof(payload));
    broadcastXBusMsg(msg);
}

void BroadcastNotificationDismiss(uint32_t id) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::XBusMessage msg{};
    msg.transaction_id_ = 0;
    std::strncpy(msg.service_name_, "com.freebsdx.Notification", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::NotificationOpcode::Dismiss);
    std::memcpy(msg.payload_, &id, sizeof(id));
    broadcastXBusMsg(msg);
}

void BroadcastWorkspaceChanged(uint32_t index) noexcept {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Shell", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ShellOpcode::WorkspaceChanged);
    shared::WorkspaceChangedPayload payload{ .index_ = index };
    std::memcpy(msg.payload_, &payload, sizeof(payload));
    broadcastXBusMsg(msg);
}

void BroadcastSwitcherUpdate(const shared::SwitcherPayload& payload) noexcept {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Shell", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ShellOpcode::SwitcherUpdate);
    std::memcpy(msg.payload_, &payload, sizeof(payload));
    broadcastXBusMsg(msg);
}

void BroadcastSwitcherDismiss() noexcept {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Shell", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ShellOpcode::SwitcherDismiss);
    broadcastXBusMsg(msg);
}

void BroadcastWindowListUpdate(const shared::WindowListPayload& payload) noexcept {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    shared::XBusMessage msg{};
    std::strncpy(msg.service_name_, "com.freebsdx.Shell", sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::ShellOpcode::WindowListUpdate);
    std::memcpy(msg.payload_, &payload, sizeof(payload));
    broadcastXBusMsg(msg);
}

void RebuildAndBroadcastWindowList(const WindowStack* stack) noexcept {
    if (!stack) return;
    shared::WindowListPayload payload{};
    for (const auto& w : stack->getWindows()) {
        if (!w.is_shell_ && w.layer_ < 0 && !w.is_subsurface_ && payload.count_ < 7) {
            auto& e = payload.entries_[payload.count_++];
            e.surface_id_ = w.surface_id_;
            const std::string& t = w.title_;
            std::strncpy(e.title_,
                t.empty() ? ("App " + std::to_string(w.surface_id_)).c_str() : t.c_str(),
                sizeof(e.title_) - 1);
        }
    }
    BroadcastWindowListUpdate(payload);
}

void BroadcastXBusMessage(const shared::XBusMessage& msg) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    broadcastXBusMsg(msg);
}

void BroadcastPresentFeedback(const shared::PresentFeedback& fb) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    std::span<const uint8_t> span(reinterpret_cast<const uint8_t*>(&fb), sizeof(fb));
    for (auto it = g_active_clients.begin(); it != g_active_clients.end();) {
        if (auto client = it->lock()) { (void)client->sendMessage(span); ++it; }
        else it = g_active_clients.erase(it);
    }
}

XBusResult XBusDispatcher::ProcessMessage(std::shared_ptr<IpcSocket> client, const std::span<const uint8_t>& data, UniqueFd& fd_opt, VulkanCompositor* comp, WindowStack* stack, bool has_focus, DrmDevice* drm) noexcept {
    if (data.size() < sizeof(shared::XBusMessage)) return {false, std::nullopt};
    const auto* msg = reinterpret_cast<const shared::XBusMessage*>(data.data());
    if (msg->magic_ != shared::XBUS_MAGIC) return {false, std::nullopt};

    if (std::strcmp(msg->service_name_, "com.freebsdx.Automation") == 0) {
        auto injected = AutomationService::ProcessMessage(client, *msg, comp, stack);
        return {true, injected};
    }

    shared::XBusMessage response = *msg;

    const bool trusted = client->isTrusted(); // true only for the shell (surface 999)

    if (std::strcmp(msg->service_name_, "com.freebsdx.Audio") == 0) {
        // Microphone permission: gate capture-device enumeration for sandboxed
        // apps. (OpenStream can't yet be gated for capture specifically — it's
        // also the playback path — until AudioStreamRequest carries a direction
        // flag; tracked as a protocol follow-up.)
        if (client->isSandboxed() &&
            msg->opcode_ == static_cast<uint32_t>(shared::AudioOpcode::ListSources) &&
            !AppSandboxService::isAllowed(client->appId(), PermissionStore::Microphone)) {
            SecurityEventsService::record("perm-deny", client->appId(), "microphone", false);
            shared::XBusMessage resp = *msg;          // empty page reply
            shared::AudioDevicePage pg{};
            std::memcpy(resp.payload_, &pg, sizeof(pg));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        } else {
            AudioService::getInstance().ProcessMessage(client, *msg, fd_opt);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Storage") == 0) {
        // Untrusted apps may read but not write storage
        if (trusted || msg->opcode_ == 0x01 /* ReadFile */) {
            StorageService::ProcessMessage(client, *msg);
        } else {
            std::cerr << "[XBusDispatcher] DENIED: untrusted app attempted Storage write (opcode "
                      << msg->opcode_ << ")\n";
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Update") == 0) {
        // System update is shell-only
        if (trusted) UpdateService::ProcessMessage(client, *msg);
        else std::cerr << "[XBusDispatcher] DENIED: untrusted app attempted Update service\n";
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.System") == 0) {
        if (msg->opcode_ == 0x02) {
            // Shutdown is shell-only
            if (trusted) { if (shutdown_cb_) shutdown_cb_(); }
            else std::cerr << "[XBusDispatcher] DENIED: untrusted app attempted System/Shutdown\n";
        } else {
            SystemService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Network") == 0) {
        NetworkService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.VPN") == 0) {
        // Reads are open to all apps; tunnel and config writes denied to sandboxed clients.
        const bool is_write = (msg->opcode_ >= static_cast<uint32_t>(shared::VpnOpcode::Connect));
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted VPN write\n";
        } else {
            VpnService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Bookmarks") == 0) {
        BookmarkService::ProcessMessage(client, *msg);
        return {true};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Calendar") == 0) {
        CalendarService::ProcessMessage(client, *msg);
        return {true};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.RemoteDesktop") == 0) {
        const bool is_write =
            msg->opcode_ >= static_cast<uint32_t>(shared::RemoteDesktopOpcode::Enable) &&
            msg->opcode_ <= static_cast<uint32_t>(shared::RemoteDesktopOpcode::SetPassword);
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted RemoteDesktop write\n";
        } else {
            RemoteDesktopService::ProcessMessage(client, *msg);
        }
        return {true};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.XWayland") == 0) {
        XWaylandService::ProcessMessage(client, *msg);
        return {true};
    }
    // Login service (pre-session greeter)
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Login") == 0) {
        LoginService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Lock") == 0) {
        // Lock/Unlock/Register: open to lockscreen and trusted clients.
        // SetTimeout is denied to sandboxed apps.
        const bool is_write =
            msg->opcode_ == static_cast<uint32_t>(shared::LockOpcode::Lock) ||
            msg->opcode_ == static_cast<uint32_t>(shared::LockOpcode::SetTimeout);
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Lock write\n";
        } else {
            LockService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ScreenSaver") == 0) {
        // GetState/GetConfig/Inhibit/Uninhibit/Register: open to all apps.
        // SetConfig: denied to sandboxed apps (prevents rogue apps disabling saver).
        const bool is_write =
            msg->opcode_ == static_cast<uint32_t>(shared::ScreenSaverOpcode::SetConfig);
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted ScreenSaver SetConfig\n";
        } else {
            ScreenSaverService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Terminal") == 0) {
        // Sandboxed apps may only attach/detach and send input to sessions
        // they own. DestroySession is denied to sandboxed clients entirely.
        const bool is_destroy =
            msg->opcode_ == static_cast<uint32_t>(shared::TerminalOpcode::DestroySession);
        if (client->isSandboxed() && is_destroy) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Terminal DestroySession\n";
        } else {
            TerminalService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Screenshot") == 0) {
        // Sandboxed apps may call GetStatus or CaptureRegion (shell-initiated);
        // StartRecording and StopRecording are denied to sandboxed clients.
        const bool is_write = (msg->opcode_ == static_cast<uint32_t>(shared::ScreenshotOpcode::StartRecording) ||
                               msg->opcode_ == static_cast<uint32_t>(shared::ScreenshotOpcode::StopRecording));
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Screenshot write\n";
        } else {
            ScreenshotService::getInstance().ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Configuration") == 0) {
        // All apps may read and write configuration (single-user desktop, no privilege escalation risk)
        ConfigService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Clipboard") == 0) {
        ClipboardService::ProcessMessage(client, *msg, has_focus);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Notification") == 0) {
        NotificationService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Portal") == 0) {
        PortalService::ProcessMessage(client, *msg); // path validation is inside PortalService
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Print") == 0) {
        PrintService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Hardware") == 0) {
        // Hardware read/write is open to all local apps (backlight, volume).
        // ToggleDashboard (0x06) and destructive ops remain shell-only.
        if (trusted || msg->opcode_ != static_cast<uint32_t>(shared::HardwareOpcode::ToggleDashboard)) {
            HardwareService::ProcessMessage(client, *msg);
        } else {
            std::cerr << "[XBusDispatcher] DENIED: untrusted app attempted Hardware/ToggleDashboard\n";
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Bluetooth") == 0) {
        const bool is_write = (msg->opcode_ >= static_cast<uint32_t>(shared::BluetoothOpcode::SetEnabled));
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Bluetooth write\n";
        } else {
            BluetoothService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Backlight") == 0) {
        const bool is_write = (msg->opcode_ >= static_cast<uint32_t>(shared::BacklightOpcode::SetBrightness));
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Backlight write\n";
        } else {
            BacklightService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Power") == 0) {
        // Reads are open to all apps; power-state transitions and policy writes
        // are denied to sandboxed (Capsicum) clients.
        const bool is_write = (msg->opcode_ >= static_cast<uint32_t>(shared::PowerOpcode::Suspend) &&
                               msg->opcode_ <= static_cast<uint32_t>(shared::PowerOpcode::SetSleepTimeout));
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Power write\n";
        } else {
            PowerService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Screencopy") == 0) {
        // Screen-capture permission: a sandboxed app may grab the framebuffer
        // only if the user granted it (was previously ungated — any app could
        // read the whole screen). Deny-by-default; audit refusals.
        if (msg->opcode_ == 0x01 && client->isSandboxed() &&
            !AppSandboxService::isAllowed(client->appId(), PermissionStore::ScreenCapture)) {
            SecurityEventsService::record("perm-deny", client->appId(), "screencapture", false);
            shared::XBusMessage resp = *msg;
            shared::ScreencopyFrameInfo fail{};   // zero dims = failure
            std::memcpy(resp.payload_, &fail, sizeof(fail));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        else if (msg->opcode_ == 0x01 && comp) {
            shared::ScreencopyCaptureRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));

            auto res = comp->captureToShm(req.output_idx_);
            shared::XBusMessage resp = *msg;
            if (res.has_value()) {
                auto [shm_fd, info] = std::move(res.value());
                std::memcpy(resp.payload_, &info, sizeof(info));
                (void)client->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)}, shm_fd);
                ::close(shm_fd); // kernel duplicates the fd for the recipient
            } else {
                // Signal failure: zero dimensions
                shared::ScreencopyFrameInfo fail{};
                std::memcpy(resp.payload_, &fail, sizeof(fail));
                (void)client->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
            }
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Headless") == 0) {
        HeadlessService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Input") == 0) {
        if (msg->opcode_ == 0x01 || msg->opcode_ == 0x02) {
            // Pointer lock/unlock request from the Wayland bridge
            bool locked = (msg->opcode_ == 0x01);
            if (cursor_lock_cb_) cursor_lock_cb_(locked);
        } else if (msg->opcode_ == 0x03) {
            // Cursor shape change request (wp_cursor_shape_v1)
            uint32_t shape = 0;
            std::memcpy(&shape, msg->payload_, sizeof(shape));
            if (cursor_shape_cb_) cursor_shape_cb_(shape);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Display") == 0) {
        // Sandboxed apps may query display state but may not change system-wide settings.
        const uint32_t op = msg->opcode_;
        const bool is_write_op =
            op == static_cast<uint32_t>(shared::DisplayOpcode::SetOutputScale) ||
            op == static_cast<uint32_t>(shared::DisplayOpcode::SetVrrEnabled);
        if (client->isSandboxed() && is_write_op) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Display write (opcode 0x"
                      << std::hex << op << std::dec << ")\n";
        } else {
            DisplayService::ProcessMessage(client, *msg, stack, drm);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.MIME") == 0) {
        MimeService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.AppManager") == 0) {
        const bool is_sandboxed = client->isSandboxed();
        auto respond = [&](const auto& payload) {
            shared::XBusMessage resp = *msg;
            std::memcpy(resp.payload_, &payload, sizeof(payload));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        };
        auto respond_ok = [&]() {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        };

        if (msg->opcode_ == static_cast<uint32_t>(shared::AppManagerCatalogOpcode::UpdateCatalog)) {
            std::thread([]() {
                try {
                    auto st = AppManagerService::updateCatalog();
                    shared::XBusMessage bcast{};
                    std::strncpy(bcast.service_name_, "com.freebsdx.AppManager", sizeof(bcast.service_name_)-1);
                    bcast.opcode_ = static_cast<uint32_t>(shared::AppManagerCatalogOpcode::CatalogUpdated);
                    std::memcpy(bcast.payload_, &st, sizeof(st));
                    BroadcastXBusMessage(bcast);
                } catch (...) {
                    std::cerr << "[AppManager] updateCatalog threw\n";
                }
            }).detach();
            shared::AppManagerCatalogState updating{};
            updating.updating_ = 1;
            std::strncpy(updating.catalog_url_, "https://pkg.freebsd-x.org/catalog.json",
                         sizeof(updating.catalog_url_)-1);
            respond(updating);
        }
        else if (msg->opcode_ == static_cast<uint32_t>(shared::AppManagerCatalogOpcode::ListRemote)) {
            shared::AppManagerPageRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            auto pg = AppManagerService::listRemote(req.page_);
            respond(pg);
        }
        else if (msg->opcode_ == static_cast<uint32_t>(shared::AppManagerCatalogOpcode::SearchRemote)) {
            shared::AppManagerSearchRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            auto pg = AppManagerService::searchRemote(req);
            respond(pg);
        }
        else if (msg->opcode_ == static_cast<uint32_t>(shared::AppManagerCatalogOpcode::InstallRemote)) {
            if (is_sandboxed) { respond_ok(); }
            else {
                shared::AppManagerRemoteInstallRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                AppManagerService::installRemote(req, client);
                respond_ok();
            }
        }
        else {
            AppManagerService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.PackageSigning") == 0) {
        // Sandboxed apps may not mutate the trust store or change policy.
        const bool is_mutating =
            msg->opcode_ == static_cast<uint32_t>(shared::PackageSigningOpcode::ImportKey) ||
            msg->opcode_ == static_cast<uint32_t>(shared::PackageSigningOpcode::RemoveKey) ||
            msg->opcode_ == static_cast<uint32_t>(shared::PackageSigningOpcode::SetPolicy);

        if (client->isSandboxed() && is_mutating) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app tried to mutate package trust store\n";
            // Send a failure response so the client doesn't hang
            shared::PkgSigningStatusResponse sr{};
            sr.success_ = 0;
            std::strncpy(sr.message_, "denied: sandboxed", sizeof(sr.message_) - 1);
            shared::XBusMessage deny = *msg;
            std::memset(deny.payload_, 0, sizeof(deny.payload_));
            std::memcpy(deny.payload_, &sr, sizeof(sr));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&deny), sizeof(deny)});
        } else {
            PkgSigningService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Shell") == 0) {
        // LaunchWithFile: broadcast to all clients so the shell can handle it.
        if (msg->opcode_ == static_cast<uint32_t>(shared::ShellOpcode::LaunchWithFile)) {
            BroadcastXBusMessage(*msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Ping") == 0) {
        std::strncpy(reinterpret_cast<char*>(response.payload_), "PONG", 255);
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DnD") == 0) {
        if (msg->opcode_ == static_cast<uint32_t>(shared::DnDOpcode::StartDrag)) {
            shared::DnDStartRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            g_dnd.active         = true;
            g_dnd.source_surface = req.surface_id_;
            g_dnd.source_client  = client;
            g_dnd.current_target = UINT32_MAX;
            std::memcpy(g_dnd.mime_type, req.mime_type_, sizeof(g_dnd.mime_type));
            std::memcpy(g_dnd.data,      req.data_,      sizeof(g_dnd.data));
            std::cout << "[DnD] Drag started by surface " << req.surface_id_
                      << "  mime=" << req.mime_type_ << "\n";
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Sandbox") == 0) {
        if (msg->opcode_ == static_cast<uint32_t>(shared::SandboxOpcode::EnterSandbox)) {
            client->markSandboxed();
            std::cout << "[Sandbox] Client entered Capsicum capability mode.\n";
            // Echo the message back as acknowledgement
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.SecurityContext") == 0) {
        SecurityContextService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Theme") == 0) {
        if (msg->opcode_ == static_cast<uint32_t>(shared::ThemeOpcode::GetTheme)) {
            // Respond with current theme state — allowed even for sandboxed apps.
            shared::XBusMessage response = *msg;
            shared::ThemeState ts{};
            ts.dark_mode_  = g_dark_mode  ? 1u : 0u;
            ts.accent_idx_ = static_cast<uint32_t>(g_accent_idx);
            std::memcpy(response.payload_, &ts, sizeof(ts));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        }
        else if (msg->opcode_ == static_cast<uint32_t>(shared::ThemeOpcode::SetTheme)) {
            // System-wide theme changes are denied for sandboxed apps.
            if (client->isSandboxed()) {
                std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Theme write\n";
            } else {
                shared::ThemeSetRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                g_dark_mode  = (req.dark_mode_ != 0);
                g_accent_idx = (req.accent_idx_ <= 2u) ? static_cast<int>(req.accent_idx_) : 0;
                SaveTheme();
                BroadcastThemeChanged();
            }
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.IME") == 0) {
        ImeService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Accessibility") == 0) {
        AccessibilityService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.PowerProfile") == 0) {
        const bool is_write = (msg->opcode_ == static_cast<uint32_t>(shared::PowerProfileOpcode::SetProfile));
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted PowerProfile write\n";
        } else {
            PowerProfileService::ProcessMessage(client, *msg);
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Seat") == 0) {
        SeatService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.VirtualKeyboard") == 0) {
        if (msg->opcode_ == static_cast<uint32_t>(shared::VirtualKeyboardOpcode::InjectKey)) {
            shared::VirtualKeyInput vk{};
            std::memcpy(&vk, msg->payload_, sizeof(vk));
            VirtualKeyboardService::queueKey(vk);
        }
        (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        return {true};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Camera") == 0) {
        const bool is_sandboxed = client->isSandboxed();
        auto respond = [&](const auto& payload) {
            shared::XBusMessage resp = *msg;
            std::memcpy(resp.payload_, &payload, sizeof(payload));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        };
        auto respond_ok = [&]() {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        };
        // Runtime permission: a sandboxed app may use the camera only if the user
        // granted it (deny-by-default). Was previously a blanket deny for all
        // sandboxed apps; now it's a per-app grant via the AppSandbox panel.
        if (is_sandboxed &&
            !AppSandboxService::isAllowed(client->appId(), PermissionStore::Camera)) {
            SecurityEventsService::record("perm-deny", client->appId(), "camera", false);
            respond_ok();
            return {true};
        }
        switch (static_cast<shared::CameraOpcode>(msg->opcode_)) {
            case shared::CameraOpcode::ListDevices: {
                uint32_t page = 0;
                std::memcpy(&page, msg->payload_, sizeof(page));
                auto pg = CameraService::listDevices(page);
                return respond(pg), XBusResult{true};
            }
            case shared::CameraOpcode::Open: {
                shared::CameraOpenRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                auto st = CameraService::openDevice(req);
                return respond(st), XBusResult{true};
            }
            case shared::CameraOpcode::Close: {
                auto st = CameraService::closeDevice();
                return respond(st), XBusResult{true};
            }
            case shared::CameraOpcode::CaptureStill: {
                shared::CameraStillRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                auto res = CameraService::captureStill(req);
                return respond(res), XBusResult{true};
            }
            case shared::CameraOpcode::StartRecording: {
                shared::CameraRecordRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                auto st = CameraService::startRecording(req);
                return respond(st), XBusResult{true};
            }
            case shared::CameraOpcode::StopRecording: {
                auto res = CameraService::stopRecording();
                return respond(res), XBusResult{true};
            }
            default:
                respond_ok();
                return {true};
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Tray") == 0) {
        auto respond = [&](const auto& payload) {
            shared::XBusMessage resp = *msg;
            std::memcpy(resp.payload_, &payload, sizeof(payload));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        };
        auto respond_ok = [&]() {
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        };
        switch (static_cast<shared::TrayOpcode>(msg->opcode_)) {
            case shared::TrayOpcode::Register: {
                shared::TrayRegisterRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                auto entry = TrayService::registerIcon(req, client);
                return respond(entry), XBusResult{true};
            }
            case shared::TrayOpcode::Unregister: {
                shared::TrayIdRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                TrayService::unregister(req.id_);
                return respond_ok(), XBusResult{true};
            }
            case shared::TrayOpcode::SetIcon: {
                shared::TrayUpdateRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                TrayService::setIcon(req);
                return respond_ok(), XBusResult{true};
            }
            case shared::TrayOpcode::SetTooltip: {
                shared::TrayUpdateRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                TrayService::setTooltip(req);
                return respond_ok(), XBusResult{true};
            }
            case shared::TrayOpcode::SetStatus: {
                shared::TrayStatusRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                TrayService::setStatus(req);
                return respond_ok(), XBusResult{true};
            }
            case shared::TrayOpcode::ListEntries: {
                shared::TrayPageRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                auto pg = TrayService::listEntries(req.page_);
                return respond(pg), XBusResult{true};
            }
            case shared::TrayOpcode::Click: {
                shared::TrayIdRequest req{};
                std::memcpy(&req, msg->payload_, sizeof(req));
                TrayService::dispatchClick(req.id_);
                return respond_ok(), XBusResult{true};
            }
            default:
                return respond_ok(), XBusResult{true};
        }
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.FileChooser") == 0) {
        using O = shared::FileChooserOpcode;
        const auto op = static_cast<O>(msg->opcode_);
        if (op == O::OpenFile || op == O::OpenFiles || op == O::SaveFile) {
            shared::FileChooserRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            FileChooserService::requestOpen(msg->opcode_, msg->transaction_id_, req, client);
        } else if (op == O::Respond) {
            shared::FileChooserResponse resp{};
            std::memcpy(&resp, msg->payload_, sizeof(resp));
            FileChooserService::handleRespond(resp);
            // ack to xshell — full envelope (ack = *msg already echoes the
            // transaction id); a payload-only 256B send would be dropped by the
            // client's 300B framing and never reach the Call callback.
            shared::XBusMessage ack = *msg;
            client->sendMessage({reinterpret_cast<const uint8_t*>(&ack), sizeof(ack)});
        }
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ClipboardHistory") == 0) {
        ClipboardHistoryService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.CrashReporter") == 0) {
        CrashReporterService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Security") == 0) {
        SecurityEventsService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.AppSandbox") == 0) {
        AppSandboxService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Locale") == 0) {
        LocaleService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Sessions") == 0) {
        SessionService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.NetConns") == 0) {
        NetConnService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Netstat") == 0) {
        NetstatService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.SysInfo") == 0) {
        SysInfoService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.MountedVolumes") == 0) {
        MountedVolService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Process") == 0) {
        ProcessService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.SysLog") == 0) {
        SysLogService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Packages") == 0) {
        PackagesService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Usb") == 0) {
        UsbService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.UsbDevices") == 0) {
        UsbDevicesService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.UsbDevice") == 0) {
        UsbDeviceService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Smart") == 0) {
        SmartService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.KernelModules") == 0) {
        KmodService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (auto handler = shellOnlyService(msg->service_name_)) {
        // Root actions via the broker (Services/Jails/ZfsPools/BootEnv/NTP) and
        // the user's crontab, SSH keys, captures and env file: shell panels
        // only. Apps get no reply, like the other trusted-only services.
        if (trusted) handler(*msg, client);
        else std::cerr << "[XBusDispatcher] DENIED: untrusted app attempted "
                       << msg->service_name_ << "\n";
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.BatteryHealth") == 0) {
        BatteryHealthService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.AudioOutput") == 0) {
        AudioOutputService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.GpuMonitor") == 0) {
        GpuMonitorService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.FramePerf") == 0) {
        FramePerfService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.BatteryDrain") == 0) {
        BatteryDrainService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Keymap") == 0) {
        KeymapService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.AdBlock") == 0) {
        AdBlockService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.LocationServices") == 0 ||
             std::strcmp(msg->service_name_, "com.freebsdx.Location") == 0) {
        LocationService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.KeyboardBacklight") == 0) {
        KbdBacklightService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Energy") == 0) {
        EnergyService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ColorProfile") == 0) {
        ColorProfileService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DnsConfig") == 0) {
        DnsConfigService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Firewall") == 0) {
        FirewallService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DisplayArrange") == 0) {
        DisplayArrangeService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.PkgUpdates") == 0) {
        PkgUpdatesService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.AirplaneMode") == 0) {
        AirplaneModeService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DataSaver") == 0) {
        DataSaverService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DoNotDisturb") == 0) {
        DoNotDisturbService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.NightLight") == 0) {
        NightLightService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ReduceMotion") == 0) {
        ReduceMotionService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.RotationLock") == 0) {
        RotationLockService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.WakeLock") == 0) {
        WakeLockService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.GameMode") == 0) {
        GameModeService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.MicMute") == 0) {
        MicMuteService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ScrollDir") == 0) {
        ScrollDirService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.GPS") == 0) {
        GpsService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Timezone") == 0) {
        TimezoneService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DiskUsage") == 0) {
        DiskUsageService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Thermal") == 0) {
        ThermalService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.MemPressure") == 0) {
        MemPressureService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.CursorSize") == 0) {
        CursorSizeService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.TextSize") == 0) {
        TextSizeService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ScreenTimeout") == 0) {
        ScreenTimeoutService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Pointer") == 0) {
        PointerService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.ColorFilter") == 0) {
        ColorFilterService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Touchpad") == 0) {
        TouchpadService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DisplayScale") == 0) {
        DisplayScaleService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.SystemSounds") == 0) {
        SystemSoundsService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Privacy") == 0) {
        PrivacyService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.FocusMode") == 0) {
        FocusModeService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.AutoBrightness") == 0) {
        AutoBrightnessService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.RefreshRate") == 0) {
        RefreshRateService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Dock") == 0) {
        DockService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Gesture") == 0) {
        GestureService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.A11ySettings") == 0) {
        A11ySettingsService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.UpdateChannel") == 0) {
        UpdateChannelService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.SpatialAudio") == 0) {
        SpatialAudioService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Stylus") == 0) {
        StylusService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DNS") == 0) {
        DnsService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Proxy") == 0) {
        ProxyService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.SavedWifi") == 0) {
        SavedWifiService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.WakeOnLan") == 0) {
        WakeOnLanService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Contacts") == 0) {
        ContactsService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Keychain") == 0) {
        KeychainService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Notes") == 0) {
        NotesService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Archive") == 0) {
        ArchiveService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.MediaPlayer") == 0) {
        MediaPlayerService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Weather") == 0) {
        WeatherService::handleMessage(*msg, client);
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Disk") == 0) {
        // Format and Eject are destructive — denied to sandboxed apps
        const bool is_destructive =
            msg->opcode_ == static_cast<uint32_t>(shared::DiskOpcode::Format) ||
            msg->opcode_ == static_cast<uint32_t>(shared::DiskOpcode::Eject);
        if (client->isSandboxed() && is_destructive) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted Disk destructive op\n";
        } else {
            DiskService::handleMessage(*msg, client);
        }
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.FontManager") == 0) {
        // InstallFont and RemoveFont denied to sandboxed apps
        const bool is_write =
            msg->opcode_ == static_cast<uint32_t>(shared::FontManagerOpcode::InstallFont) ||
            msg->opcode_ == static_cast<uint32_t>(shared::FontManagerOpcode::RemoveFont);
        if (client->isSandboxed() && is_write) {
            std::cerr << "[XBusDispatcher] DENIED: sandboxed app attempted FontManager write\n";
        } else {
            FontManagerService::handleMessage(*msg, client);
        }
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Wallpaper") == 0) {
        if (msg->opcode_ == static_cast<uint32_t>(shared::WallpaperOpcode::SetWallpaper)) {
            shared::WallpaperSetRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            req.path_[sizeof(req.path_) - 1] = '\0';
            ConfigService::set("wallpaper", req.path_);
            ConfigService::set("wallpaper.fit", std::to_string(req.fit_mode_));
            // The compositor owns the desktop background (drawn below windows);
            // reload it live so the change is visible immediately.
            if (comp) comp->setWallpaper(req.path_, req.fit_mode_);
            if (redraw_cb_) redraw_cb_();  // mark scene dirty → recomposite
            // Broadcast to all clients so other listeners (e.g. lock screen) update.
            shared::XBusMessage bcast = *msg;
            BroadcastXBusMessage(bcast);
        } else if (msg->opcode_ == static_cast<uint32_t>(shared::WallpaperOpcode::GetWallpaper)) {
            shared::XBusMessage resp = *msg;
            shared::WallpaperSetRequest cur{};
            std::string path = ConfigService::get("wallpaper", "");
            std::strncpy(cur.path_, path.c_str(), sizeof(cur.path_) - 1);
            cur.fit_mode_ = static_cast<uint32_t>(
                safeConfigU32(ConfigService::get("wallpaper.fit", "0")));
            std::memcpy(resp.payload_, &cur, sizeof(cur));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        } else if (msg->opcode_ == static_cast<uint32_t>(shared::WallpaperOpcode::SetWorkspaceWallpaper)) {
            shared::WallpaperWorkspaceRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            req.path_[sizeof(req.path_) - 1] = '\0';
            std::string ws_key = "wallpaper.ws." + std::to_string(req.workspace_idx_);
            ConfigService::set(ws_key, req.path_);
            ConfigService::set(ws_key + ".fit", std::to_string(req.fit_mode_));
            // Single active workspace: reload the compositor background live.
            if (comp) comp->setWallpaper(req.path_, req.fit_mode_);
            if (redraw_cb_) redraw_cb_();  // mark scene dirty → recomposite
            // Broadcast so other listeners can react if this is the active workspace
            shared::XBusMessage bcast = *msg;
            BroadcastXBusMessage(bcast);
        } else if (msg->opcode_ == static_cast<uint32_t>(shared::WallpaperOpcode::GetWorkspaceWallpaper)) {
            shared::WallpaperWorkspaceRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            std::string ws_key = "wallpaper.ws." + std::to_string(req.workspace_idx_);
            // Fall back to global wallpaper if no per-workspace override is set
            std::string path = ConfigService::get(ws_key, "");
            if (path.empty()) path = ConfigService::get("wallpaper", "");
            std::string fit_key = path.empty() ? "wallpaper.fit" : ws_key + ".fit";
            shared::WallpaperWorkspaceRequest resp_data{};
            resp_data.workspace_idx_ = req.workspace_idx_;
            resp_data.fit_mode_ = static_cast<uint32_t>(
                safeConfigU32(ConfigService::get(fit_key, "0")));
            std::strncpy(resp_data.path_, path.c_str(), sizeof(resp_data.path_) - 1);
            shared::XBusMessage resp = *msg;
            std::memcpy(resp.payload_, &resp_data, sizeof(resp_data));
            (void)client->sendMessage({reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return {true, std::nullopt};
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.NightShift") == 0) {
        NightShiftService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.MonoAudio") == 0) {
        MonoAudioService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.TrueTone") == 0) {
        TrueToneService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.OptStorage") == 0) {
        OptStorageService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.StageManager") == 0) {
        StageManagerService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.TrustedDevices") == 0) {
        TrustedDevicesService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.LoginItems") == 0) {
        LoginItemsService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.DefaultApps") == 0) {
        DefaultAppsService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.KeyboardShortcuts") == 0) {
        KeyboardShortcutsService::ProcessMessage(client, *msg);
    }
    else if (std::strcmp(msg->service_name_, "com.freebsdx.Window") == 0) {
        if (msg->opcode_ == static_cast<uint32_t>(shared::WindowOpcode::SetTitle) && stack) {
            shared::WindowTitleRequest req{};
            std::memcpy(&req, msg->payload_, sizeof(req));
            req.title_[sizeof(req.title_) - 1] = '\0';
            stack->setWindowTitle(req.surface_id_, req.title_);
            std::cout << "[WM] Surface " << req.surface_id_ << " title: " << req.title_ << "\n";
            RebuildAndBroadcastWindowList(stack); // title changed — refresh taskbar
        }
        else if (msg->opcode_ == static_cast<uint32_t>(shared::WindowOpcode::Raise) && stack) {
            uint32_t sid = 0;
            std::memcpy(&sid, msg->payload_, sizeof(sid));
            stack->raiseToFocus(sid);
            if (raise_cb_) raise_cb_(sid);
            RebuildAndBroadcastWindowList(stack);
            std::cout << "[WM] Raise surface " << sid << "\n";
        }
    }

    return {true, std::nullopt};
}

} // namespace fbx::xserver
