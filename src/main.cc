#include <cstdio>
#include "xserver/crash_handler.h"
#include "xserver/drm_device.h"
#include "xserver/event_loop.h"
#include "xserver/seat_manager.h"
#include "xserver/xsessiond_device.h"
#include "xserver/input_manager.h"
#include <spawn.h>
#include "xserver/shortcuts.h"
#include "xserver/scrolldir_service.h"
#include "xserver/touchpad_service.h"
#include "xserver/pointer_service.h"
#include "xserver/cursor_size_service.h"
#include "xserver/display_scale_service.h"
#include "xserver/refresh_rate_service.h"
#include "xserver/display_arrange_service.h"
#include "xserver/color_filter_service.h"
#include "xserver/nightlight_service.h"
#include <array>
#include "xserver/micmute_service.h"
#include "xserver/wakelock_service.h"
#include "xserver/gps_service.h"
#include "xserver/window_stack.h"
#include "xserver/window_decorator.h"
#include "xserver/vulkan_compositor.h"
#include "xserver/ipc_socket.h"
#include "xserver/xbus_dispatcher.h"
#include "xserver/audio_service.h"
#include "xserver/screenshot_service.h"
#include "xserver/user_services.h"
#include "xserver/device_services.h"
#include "xserver/personal_services.h"
#include "xserver/desktop_services.h"
#include "xserver/keyboard_shortcuts_service.h"
#include "xserver/keymap_service.h"
#include "xserver/color_profile_service.h"
#include "xserver/opt_storage_service.h"
#include "xserver/notif_services.h"
#include "xserver/lock_service.h"
#include "xserver/screensaver_service.h"
#include "xserver/terminal_service.h"
#include "xserver/login_service.h"
#include "xserver/session_service.h"
#include "xserver/udev_monitor.h"
#include "xserver/display_service.h"
#include "xserver/ime_service.h"
#include "xserver/mime_service.h"
#include "xserver/app_manager_service.h"
#include <filesystem>
#include <libudev.h>
#include "xserver/config_service.h"
#include "xserver/motion_engine.h"
#include "xserver/system_service.h"
#include "xserver/notification_service.h"
#include "xserver/power_service.h"
#include "xserver/backlight_service.h"
#include "xserver/power_profile_service.h"
#include "xserver/seat_service.h"
#include "xserver/camera_service.h"
#include "xserver/tray_service.h"
#include "xserver/clipboard_history_service.h"
#include "xserver/security_events_service.h"
#include "xserver/app_sandbox_service.h"
#include "xserver/locale_service.h"
#include "xserver/contacts_service.h"
#include "xserver/keychain_service.h"
#include "xserver/notes_service.h"
#include "xserver/archive_service.h"
#include "xserver/media_player_service.h"
#include "xserver/weather_service.h"
#include "xserver/disk_service.h"
#include "xserver/font_manager_service.h"
#include "xserver/gamepad_manager.h"
#include "xserver/remote_desktop_service.h"
#include "xserver/calendar_service.h"
#include "xserver/virtual_keyboard_service.h"
#include "xserver/headless_service.h"
#include "shared/protocol.h"
#include "shared/logger.h"
#include <iostream>
#include <stdexcept>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/kbio.h>
#include <vector>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <drm_fourcc.h>
#include "xwayland-bridge/wayland_bridge.h"

extern "C" char** environ;  // POSIX doesn't guarantee a header declares it

namespace shared = fbx::shared; // allows shared::XYZ qualified access
using namespace fbx::shared;    // allows unqualified Logger, RenderMessage, etc.
using namespace fbx::xserver;
using namespace fbx::xwayland;
using namespace fbx;

static WindowStack window_stack;
static std::shared_ptr<IpcSocket> active_client = nullptr;
static std::shared_ptr<EventLoop> g_loop = nullptr;
static std::unordered_map<uint32_t, std::shared_ptr<IpcSocket>> g_surface_clients;
static std::unordered_map<uint32_t, VkImage> g_surface_images;
// Double-buffered dmabuf imports: one imported VkImage per client render buffer
// (index 0/1), so the compositor composites the buffer the client just finished
// while the client draws into the other — no tearing from reading a buffer
// mid-rewrite. These are the OWNING handles for dmabuf surfaces (released on
// disconnect/close); g_surface_images stays the owner for solid-color surfaces.
static std::unordered_map<uint32_t, std::array<VkImage, 2>> g_surface_buffers;
static std::unordered_map<uint32_t, uint32_t> g_fb_cache;
static std::unordered_set<uint32_t> g_subsurface_ids;   // surfaces created via wl_subcompositor
static std::unordered_set<uint32_t> g_layer_surface_ids; // surfaces created via zwlr_layer_shell

// Release every GPU image owned for a surface and drop its bookkeeping. A dmabuf
// surface owns its images through g_surface_buffers (one per double-buffer index);
// g_surface_images then holds only a NON-owning alias to the active one, so we
// release from g_surface_buffers and must not double-free that alias. A
// solid-color surface instead owns its single image directly in g_surface_images.
static void releaseSurfaceImages(VulkanCompositor* comp, uint32_t id) {
    auto bit = g_surface_buffers.find(id);
    if (bit != g_surface_buffers.end()) {
        for (VkImage img : bit->second) if (img != VK_NULL_HANDLE) comp->releaseImage(img);
        g_surface_buffers.erase(bit);
        g_surface_images.erase(id);   // alias into the buffers just freed — not owned
        return;
    }
    auto iit = g_surface_images.find(id);
    if (iit != g_surface_images.end()) {
        comp->releaseImage(iit->second);   // solid-color owner
        g_surface_images.erase(iit);
    }
}

static constexpr uint32_t kMaxSurfaces = 64; // hard cap — prevents Vulkan descriptor pool exhaustion
static pid_t g_shell_pid = -1;
static bool g_flip_pending = false; // single-threaded event loop — no race
// When the current page flip was issued. A page-flip completion event clears
// g_flip_pending; if that event is ever dropped (e.g. a modeset during the
// greeter→shell handoff), g_flip_pending would otherwise stay true forever and
// the render timer's `if (g_flip_pending) return;` would wedge the whole display
// on the last frame (a black screen). The watchdog below force-clears a flip that
// hasn't completed in well over a frame so one lost event self-heals.
static std::chrono::steady_clock::time_point g_flip_issued{};
// Set whenever something that affects the screen changes (a client frame, input,
// a window add/remove). The render timer composites+flips only when this is set
// (or an animation is in flight), so a *static* scene stops page-flipping and
// holds a steady frame — continuously re-flipping identical buffers made the
// whole screen blink on the login greeter. A periodic safety redraw bounds the
// cost of any missed signal so the display can never wedge.
static bool g_scene_dirty = true;

// Seat activation gate (phase 2b). True while this session owns the seat; set
// false on SeatDisable (VT switched away) and true again on SeatEnable. The
// render timer skips compositing+flipping while inactive, so we never touch the
// GPU after DRM master has been handed to another VT. Default true: on the
// libseat path (no activation relay) nothing ever clears it, preserving today's
// behavior exactly. Only the xsessiond broker backend drives it.
static bool g_seat_active = true;

// Global socket path for consistent access
static const char* kSocketPath = "/tmp/freebsdx.sock";

// Release every DRM framebuffer object in the cache and clear it.
// Must be called before DRM master is dropped (i.e. before DrmDevice is destroyed).
// Safe to call with a null drm pointer (no-op).
static void releaseFbCache(DrmDevice* drm) noexcept {
    if (!drm || g_fb_cache.empty()) return;
    int freed = 0, failed = 0;
    for (const auto& [key, fb_id] : g_fb_cache) {
        if (drmModeRmFB(drm->fd(), fb_id) == 0) {
            ++freed;
        } else {
            std::cerr << "[drm] drmModeRmFB(fb=" << fb_id << ") failed: "
                      << strerror(errno) << "\n";
            ++failed;
        }
    }
    g_fb_cache.clear();
    std::cout << "[drm] Framebuffer cache released: "
              << freed << " freed, " << failed << " failed.\n";
}

void spawn_shell() {
    std::cout << "[xserver] SPAWNING xshell...\n" << std::flush;
    g_shell_pid = fork();
    if (g_shell_pid == 0) {
        // Lead our own process group so xshell + every app it launches can be
        // signalled as a unit on shutdown (kill(-g_shell_pid, ...)).
        setpgid(0, 0);

        int fd = open("/tmp/xshell-boot.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
        
        setenv("LD_LIBRARY_PATH", "./out", 1);
        setenv("XBUS_SOCKET", kSocketPath, 1);
        
        if (access("./xshell", X_OK) == 0) {         // dev build (builddir/)
            execl("./xshell", "xshell", nullptr);
        } else if (access("./out/xshell", X_OK) == 0) {
            execl("./out/xshell", "xshell", nullptr);
        } else if (access("/usr/local/bin/xshell", X_OK) == 0) {
            execl("/usr/local/bin/xshell", "xshell", nullptr);
        }
        std::cerr << "[xserver] FATAL: Failed to exec xshell: " << strerror(errno) << std::endl;
        exit(1);
    }
}

// Signal the desktop session (xshell + the apps it spawned) to exit. xshell
// leads its own process group (see spawn_shell), so one signal to -g_shell_pid
// reaches every app it launched.
//
// This is strictly fire-and-forget: it must NEVER block. An earlier version
// waited (blocking waitpid) for the group to die, but a child wedged in the
// kernel — e.g. stuck in a GPU/DRM ioctl — cannot be reaped even with SIGKILL,
// so the wait hung shutdown forever and froze the seat before the CRTC was
// restored. Orphans are reparented to init, which reaps them; by the time we
// return we have already handed the seat back, so a straggler is harmless.
static void shutdown_children() noexcept {
    if (g_shell_pid <= 0) return;
    std::cout << "[xserver] Signalling desktop session (pgrp " << g_shell_pid << ")...\n";
    kill(-g_shell_pid, SIGTERM);
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}  // non-blocking reap of the already-dead
    g_shell_pid = -1;
}

static float cursor_x = 960.0f;
static float cursor_y = 540.0f;
static uint32_t focused_surface = 999;

// Last cursor shape pushed to the DRM cursor plane (wp_cursor_shape enum; 1 =
// default arrow). Tracked so hover feedback only re-rasterizes on an actual
// change instead of on every pointer-motion event.
static uint32_t g_cursor_shape = 1;

// User cursor-size multiplier (com.freebsdx.CursorSize): 0.75/1.0/1.5 for
// Small/Medium/Large. Combined with the output (HiDPI) scale when rasterising the
// DRM cursor glyph, so the effective scale is getOutputScale(0) * this.
static float g_cursor_size_factor = 1.0f;
static float cursorSizeFactor(uint32_t size) noexcept {
    return size == 0 ? 0.75f : (size == 2 ? 1.5f : 1.0f);
}

// Whether a mouse button is currently held. While it is, pointer motion stays
// with the surface that owns the implicit grab (the focused surface) instead of
// following the surface under the cursor — matching pointer-grab semantics so a
// drag that strays outside the window keeps delivering to it.
static bool g_pointer_button_down = false;

// The surface the pointer was last over (999 = shell/desktop). When it changes
// the compositor resets the cursor to the arrow so the newly-entered client
// re-asserts its own shape — clients only send on change, so without this the
// previous surface's cursor would linger over the new one.
static uint32_t g_hover_surface = 999;

// Modifier bitmask (Shift=1, Ctrl=2, Alt=4, Super=8) from the most recent
// pointer motion, so the drag-and-drop cursor can pick copy (Ctrl) vs move.
static uint32_t g_pointer_modifiers = 0;

void signal_handler(int sig) {
    if (g_loop) {
        LOG_INFO("Received signal {}, shutting down...", sig);
        g_loop->stop();
    }
}

// ── VT console keyboard grab (keystroke-leak fix) ─────────────────────────────
// The compositor reads input from evdev/libinput, but the kernel ALSO delivers
// translated keystrokes to the active VT's tty — so on a real seat every key goes
// to BOTH the compositor and the shell on that VT (greeter passwords leaking into a
// console). In the libseat/seatd model the DISPLAY SERVER is responsible for
// silencing the console keyboard; seatd brokers device fds but never touches
// KDSKBMODE. FreeBSD has no K_OFF, so we use K_CODE (the tty gets raw keycodes
// instead of ASCII → nothing readable leaks), and restore the saved mode on
// teardown. The controlling tty is the VT we were launched from (the active
// console), which is owned by the logged-in user, so no root is needed.
static int g_vt_fd = -1;
static int g_vt_saved_kbmode = -1;

static void grabConsoleKeyboard() noexcept {
    if (g_vt_fd >= 0) return;
    int fd = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        const char* n = ::ttyname(STDIN_FILENO);
        if (n) fd = ::open(n, O_RDWR | O_NOCTTY | O_CLOEXEC);
    }
    if (fd < 0) { LOG_WARN("VT keyboard grab: no controlling tty ({})", strerror(errno)); return; }
    int mode = -1;
    if (::ioctl(fd, KDGKBMODE, &mode) == 0) g_vt_saved_kbmode = mode;
    if (::ioctl(fd, KDSKBMODE, K_CODE) != 0) {
        LOG_WARN("VT keyboard grab failed (KDSKBMODE): {}", strerror(errno));
        ::close(fd);
        return;
    }
    g_vt_fd = fd;
    LOG_INFO("VT keyboard grabbed (KDSKBMODE K_CODE) — console keystroke leak suppressed");
}

static void restoreConsoleKeyboard() noexcept {
    if (g_vt_fd < 0) return;
    (void)::ioctl(g_vt_fd, KDSKBMODE, g_vt_saved_kbmode >= 0 ? g_vt_saved_kbmode : K_XLATE);
    ::close(g_vt_fd);
    g_vt_fd = -1;
    LOG_INFO("VT keyboard restored (KDSKBMODE)");
}

int main(int argc, char** argv) {
    // Unbuffer stdout so shutdown/teardown progress is written to a redirected
    // log immediately. Otherwise, when stdout is a file, buffered std::cout
    // output is lost if the process wedges or is killed during teardown — which
    // is exactly when we most need to see how far it got.
    setvbuf(stdout, nullptr, _IONBF, 0);

    bool     enable_input = true;
    bool     virtual_boot = false;
    bool     headless     = false;
    // Dev/testing: boot straight to the desktop, skipping the login greeter.
    // Enabled by --no-greeter or FREEBSDX_SKIP_GREETER=1 (=0 disables).
    const char* sg_env = std::getenv("FREEBSDX_SKIP_GREETER");
    bool     skip_greeter = sg_env && std::string_view(sg_env) != "0";
    uint32_t headless_w   = 1920;
    uint32_t headless_h   = 1080;
    for (int i = 1; i < argc; ++i) {
        std::string_view arg{argv[i]};
        if (arg == "--no-input")  enable_input = false;
        if (arg == "--virtual")   virtual_boot = true;
        if (arg == "--no-greeter") skip_greeter = true;
        if (arg == "--headless") { headless = true; virtual_boot = true; }
        if (arg.starts_with("--headless-size=")) {
            auto sz = arg.substr(16);
            auto xp = sz.find('x');
            if (xp != std::string_view::npos) {
                headless_w = static_cast<uint32_t>(
                    std::stoul(std::string(sz.substr(0, xp))));
                headless_h = static_cast<uint32_t>(
                    std::stoul(std::string(sz.substr(xp + 1))));
            }
        }
    }

    Logger::init("freebsdx");
    LOG_INFO("Starting freebsdx master server...");

    // Dump a backtrace to the log + ~/.freebsdx/crashes on a fatal fault, then
    // re-raise for a core. Installed before anything else can crash.
    installCrashHandler("freebsdx");

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    // Ignore SIGPIPE: the compositor writes to many client sockets (XBus, greeter,
    // app surfaces) that can close at any moment — notably the greeter socket during
    // the login→desktop handoff, where startSession() kills the greeter and a
    // subsequent write would raise SIGPIPE whose default action (terminate, no core)
    // took down the whole server. With it ignored, those writes fail with EPIPE,
    // which the socket code already handles.
    signal(SIGPIPE, SIG_IGN);

    auto loop_res = EventLoop::create();
    if (!loop_res) return 1;
    std::shared_ptr<EventLoop> loop = std::move(loop_res.value());
    g_loop = loop;

    XBusDispatcher::OnShutdown([&loop]() {
        LOG_INFO("Shutdown requested via XBus.");
        loop->stop();
    });

    XBusDispatcher::OnRaise([](uint32_t sid) {
        focused_surface = sid;
    });

    // In headless mode the virtual dimensions come from --headless-size (default 1920x1080).
    // In hardware mode they are overridden below by the DRM primary output's native mode.
    uint32_t screen_w = headless ? headless_w : 1920;
    uint32_t screen_h = headless ? headless_h : 1080;
    std::unique_ptr<ISeatManager> seat = nullptr;
    std::unique_ptr<DrmDevice> drm = nullptr;
    std::unique_ptr<InputManager> input = nullptr;

    if (enable_input && !virtual_boot) {
        // Seat backend selection. DEFAULT: the xsessiond OpenDevice broker — route
        // device opening through the xsessiond daemon and pump its connection for
        // SeatDisable/SeatEnable push events (gating rendering via g_seat_active).
        // Set XSESSIOND_USE_BROKER=0 to fall back to the libseat-backed SeatManager
        // (talks to real seatd). If the broker daemon is unavailable we also fall
        // back to libseat automatically.
        const char* ub = ::getenv("XSESSIOND_USE_BROKER");
        bool use_broker = !(ub && ub[0] == '0');
        if (use_broker) {
            auto br_res = XsessiondDeviceManager::create();
            if (br_res) {
                XsessiondDeviceManager* broker = br_res->get();
                broker->setActivationHandlers(
                    []() { g_seat_active = false;
                           LOG_INFO("[seat] SeatDisable — GPU quiesced (VT switched away)"); },
                    []() { g_seat_active = true; g_scene_dirty = true;
                           LOG_INFO("[seat] SeatEnable — reacquired seat, repainting"); });
                seat = std::move(br_res.value());
                // Drive dispatch() from the event loop so activation events are
                // handled; this is what makes the broker path safe on a VT switch.
                (void)loop->addFd(seat->getFd(), [s = seat.get()]() { (void)s->dispatch(); });
                LOG_INFO("[seat] device broker: xsessiond (default; XSESSIOND_USE_BROKER)");
            } else {
                LOG_ERROR("[seat] xsessiond broker unavailable — falling back to libseat");
            }
        }
        if (!seat) {
            auto seat_res = SeatManager::create();
            if (seat_res) seat = std::move(seat_res.value());
        }
        if (seat) {
            auto drm_res = DrmDevice::create(*seat);
            if (drm_res) {
                drm = std::move(drm_res.value());
                (void)drm->discoverResources();
                // Use primary output's native resolution for the primary screen dimensions
                if (!drm->outputs().empty() && drm->outputs()[0].mode.connected_) {
                    screen_w = drm->outputs()[0].mode.width_;
                    screen_h = drm->outputs()[0].mode.height_;
                }
            }
            auto input_res = InputManager::create(seat.get());
            if (input_res) {
                input = std::move(input_res.value());
                input->setScreenSize(screen_w, screen_h);
            }
        }
    } else {
        if (virtual_boot) LOG_INFO("VIRTUAL BOOT active. Bypassing hardware ignition.");
        else LOG_INFO("Headless mode active. Bypassing hardware ignition.");
    }

    window_stack.setScreenSize(static_cast<float>(screen_w), static_cast<float>(screen_h));
    LoadTheme();
    PowerProfileService::init();
    SeatService::init();
    CameraService::init();
    TrayService::init();
    ClipboardHistoryService::init();
    SecurityEventsService::init();
    AppSandboxService::init();
    LocaleService::init();
    // GPS: start the NMEA receiver reader if a device is present. With no device
    // the service still answers from a manually-configured gps.lat/gps.lon.
    GpsService::init();
    // Apply the persisted scroll direction to input devices, and re-apply live
    // when com.freebsdx.ScrollDir changes. (No-op in headless: input is null.)
    ScrollDirService::init([in = input.get()](bool natural) {
        if (in) in->setNaturalScroll(natural);
    });
    // Apply the persisted touchpad enable/disable to input devices, live on change.
    TouchpadService::init([in = input.get()](bool enabled) {
        if (in) in->setTouchpadEnabled(enabled);
    });
    // Apply persisted pointer prefs (speed/accel/tap) to libinput, live on change.
    PointerService::init([in = input.get()](uint32_t speed, bool accel, bool tap) {
        if (in) in->setPointerConfig(speed, accel, tap);
    });
    AppPermsService::init([] { return AppManagerService::appsDir(); });
    // Per-app output routing (com.freebsdx.AudioRouting) → the audio mixer.
    AudioRoutingService::init({
        [] {
            std::vector<audioroute::Stream> v;
            for (auto& s : AudioService::getInstance().streams()) v.push_back({s.id, s.app, s.route});
            return v;
        },
        [](const std::string& app, const std::string& path) { AudioService::getInstance().setAppRoute(app, path); },
        nullptr,
    });
    // Keyboard layout (com.freebsdx.Keymap) → XKB in the input path.
    KeymapService::init([in = input.get()](const std::string& layout, const std::string& variant) {
        return in ? in->setKeymap(layout, variant) : false;
    });
    // Saved shortcut on/off switches (com.freebsdx.KeyboardShortcuts).
    KeyboardShortcutsService::init();
    // Saved EQ → the audio mixer (com.freebsdx.AudioEq).
    AudioEqService::init([](const eq::Gains& g) { AudioService::getInstance().setEq(g); });
    // Restore the saved auto-lock policy (com.freebsdx.ScreenLock).
    ScreenLockService::init();
    // Per-device pointer speed (com.freebsdx.InputDevice) + hot-plug re-list.
    InputDeviceService::init(
        [in = input.get()]() { return in ? in->listDevices() : std::vector<inputdev::DeviceInfo>{}; },
        [in = input.get()](uint32_t id, uint32_t sens, bool adaptive) {
            if (in) in->setDeviceOverride(id, sens, adaptive);
        });
    if (input) input->setOnDevicesChanged([] { InputDeviceService::notifyDevicesChanged(); });
    // Mic mute gates the input at the OSS mixer.
    MicMuteService::init([](bool muted) { MicMuteService::applyToMixer(muted); });
    // WakeLock inhibits the idle screensaver + idle-lock while a keep-awake holds.
    // Presentation mode holds them off too.
    ScreenSaverService::setWakeInhibitor([]() { return WakeLockService::isActive() || PresentationService::isActive(); });
    LockService::setWakeInhibitor([]() { return WakeLockService::isActive() || PresentationService::isActive(); });
    NotifScheduleService::setExtraHold([] { return PresentationService::isActive(); });
    // Logout: tear down the desktop process group (xshell + its apps) but keep
    // the compositor + seat alive, end the login session, and return to the
    // greeter. Reuses the same pgroup-signal teardown as shutdown (fire-and-
    // forget — never blocks; see shutdown_children).
    SessionService::init([]() {
        if (g_shell_pid > 0) {
            std::cout << "[xserver] Logout: signalling desktop session (pgrp "
                      << g_shell_pid << ")...\n";
            kill(-g_shell_pid, SIGTERM);
            while (waitpid(-1, nullptr, WNOHANG) > 0) {}  // reap the already-dead
            g_shell_pid = -1;
        }
        LoginService::endSession();
        LoginService::spawnGreeter();
    });
    ContactsService::init();
    KeychainService::init();
    NotesService::init();
    ArchiveService::init();
    MediaPlayerService::init();
    WeatherService::init();
    DiskService::init();
    FontManagerService::init();

    std::cout << "[xserver] Initializing Vulkan Compositor...\n" << std::flush;
    auto comp_res = VulkanCompositor::create();
    if (!comp_res) {
        LOG_ERROR("Failed to init Vulkan Compositor.");
        return 1;
    }
    std::shared_ptr<VulkanCompositor> comp = std::move(comp_res.value());
    // Settings → Developer overlay drives the GPU frame-time overlay (Super+F3).
    DevOverlayService::init([c = comp.get()] { return c->isDebugOverlayVisible(); },
                            [c = comp.get()](bool on) { if (c->isDebugOverlayVisible() != on) c->toggleDebugOverlay(); });

    // Give ScreenshotService a back-pointer to the compositor for captureToShm().
    ScreenshotService::getInstance().setCompositor(comp.get());
    CapturesService::setScreenshotHook([] { ScreenshotService::getInstance().captureFullScreen(); });

    // Give RemoteDesktopService a back-pointer to the compositor for screen capture.
    RemoteDesktopService::setCompositor(comp.get());

    // Desktop wallpaper: the compositor draws the configured image as the
    // background below all windows. Falls back to the branding wallpaper when the
    // user hasn't set one. Live changes arrive via com.freebsdx.Wallpaper/SetWallpaper.
    {
        std::string wp = ConfigService::get(
            "wallpaper", "core/shell/Resources/Branding/wallpaper.svg");
        uint32_t fit = 0;  // Cover by default
        try { fit = static_cast<uint32_t>(std::stoul(ConfigService::get("wallpaper.fit", "0"))); }
        catch (...) {}
        comp->setWallpaper(wp, fit);
    }

    DrmDevice* drm_ptr = drm.get();

    // Pointer lock callback: called by XBusDispatcher when the Wayland bridge
    // sends com.freebsdx.Input/LockPointer or UnlockPointer.
    XBusDispatcher::OnCursorLock([drm_ptr](bool locked) {
        if (!drm_ptr) return;
        if (locked) drm_ptr->hideCursorAll();
        else        drm_ptr->showCursorAll();
    });
    // Server-side scene changes (e.g. wallpaper reload) request a recomposite.
    XBusDispatcher::OnRedraw([]() { g_scene_dirty = true; });
    XBusDispatcher::OnCursorShape([drm_ptr](uint32_t shape) {
        if (drm_ptr) drm_ptr->setCursorShape(shape, window_stack.getOutputScale(0) * g_cursor_size_factor);
        g_cursor_shape = shape;  // keep the hover tracker in sync with client requests
    });
    // Apply persisted cursor size to the DRM cursor glyph (re-rasterise at the new
    // effective scale = output scale × user factor), live on change.
    CursorSizeService::init([drm_ptr](uint32_t size) {
        g_cursor_size_factor = cursorSizeFactor(size);
        if (drm_ptr) drm_ptr->setCursorShape(g_cursor_shape,
                                             window_stack.getOutputScale(0) * g_cursor_size_factor);
    });
    // Refresh rate: advertise the primary output's real supported modes and apply
    // a requested rate via a DRM modeset.
    RefreshRateService::init(drm_ptr ? drm_ptr->supportedRefreshMask(0) : 0x1u,
        [drm_ptr](uint32_t hz) {
            return drm_ptr && drm_ptr->setRefreshRate(0, hz).has_value();
        });
    // Colour pipeline: ColorFilter (colour-blindness/monochrome) and NightLight
    // (warm tint) both drive the single CRTC CTM, so compose them instead of
    // letting the last writer clobber the other. Each service updates its own
    // matrix; the product (night-light applied last, over the corrected image) is
    // pushed to the hardware. No-op if the CRTC lacks a CTM property.
    static std::array<double, 9> g_colorfilter_mat = {1,0,0, 0,1,0, 0,0,1};
    static std::array<double, 9> g_nightlight_mat  = {1,0,0, 0,1,0, 0,0,1};
    // Display colour profile (sRGB content → the display's gamut): applied
    // last of all, after the filter and the warm tint.
    static std::array<double, 9> g_profile_mat     = {1,0,0, 0,1,0, 0,0,1};
    auto apply_color_pipeline = [drm_ptr]() {
        if (!drm_ptr) return;
        auto mul = [](const std::array<double, 9>& a, const std::array<double, 9>& b) {
            std::array<double, 9> p{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    p[r*3+c] = a[r*3+0]*b[0*3+c] + a[r*3+1]*b[1*3+c] + a[r*3+2]*b[2*3+c];
            return p;
        };
        // profile × night-light × filter: the filter acts first, the profile last.
        (void)drm_ptr->setColorMatrix(0, mul(g_profile_mat, mul(g_nightlight_mat, g_colorfilter_mat)));
    };
    ColorFilterService::init([apply_color_pipeline](const std::array<double, 9>& m) {
        g_colorfilter_mat = m;
        apply_color_pipeline();
    });
    NightLightService::init([apply_color_pipeline](const std::array<double, 9>& m) {
        g_nightlight_mat = m;
        apply_color_pipeline();
    });
    ColorProfileService::init([apply_color_pipeline](const std::array<double, 9>& m) {
        g_profile_mat = m;
        apply_color_pipeline();
    });

    if (drm) {
        (void)drm->forceEnableDisplay();
        (void)loop->addFd(drm->fd(), [&]() { if(drm_ptr) drm_ptr->handleEvent(); });

        // Create per-output Vulkan render targets for outputs beyond the primary.
        // Output 0 is already created at 1920x1080 by VulkanCompositor::init();
        // re-create it now at the actual native resolution.
        (void)comp->createTarget(0, screen_w, screen_h);
        for (size_t i = 1; i < drm->outputs().size(); ++i) {
            const auto& out = drm->outputs()[i];
            (void)comp->createTarget(i, out.mode.width_, out.mode.height_);
        }

        // Publish output geometry to the window manager and display broker.
        // Logical dimensions = physical / scale (scale defaults to 1.0 unless already set).
        {
            std::vector<OutputInfo> layout;
            float x_logical = 0.0f;
            for (const auto& out : drm->outputs()) {
                float pw = static_cast<float>(out.mode.width_);
                float ph = static_cast<float>(out.mode.height_);
                float sc = 1.0f; // default; Settings app can override via SetOutputScale
                OutputInfo oi{};
                oi.phys_width_  = pw;
                oi.phys_height_ = ph;
                oi.scale_       = sc;
                oi.width_       = pw / sc;
                oi.height_      = ph / sc;
                oi.x_offset_    = x_logical;
                layout.push_back(oi);
                x_logical += oi.width_;
            }
            // Probe VRR capability per output and store in the layout.
            for (size_t i = 0; i < layout.size(); ++i) {
                bool capable = drm->isVrrCapable(i);
                layout[i].vrr_capable_ = capable;
                if (capable) {
                    // Auto-enable VRR on every capable output at startup.
                    auto res = drm->setVrrEnabled(i, true);
                    layout[i].vrr_enabled_ = res.has_value();
                    if (res) {
                        std::cout << "[xserver] VRR auto-enabled on output " << i << "\n";
                    }
                }
                window_stack.setVrrCapable(i, layout[i].vrr_capable_);
                window_stack.setVrrEnabled(i, layout[i].vrr_enabled_);
            }

            // Probe HDR capability per output.
            for (size_t i = 0; i < layout.size(); ++i) {
                bool hdr_capable = drm->isHdrCapable(i);
                layout[i].hdr_capable_ = hdr_capable;
                window_stack.setHdrCapable(i, hdr_capable);
                // HDR is NOT auto-enabled at startup — user must enable via Settings.
                if (hdr_capable) {
                    std::cout << "[xserver] HDR capable on output " << i << "\n";
                }
            }

            window_stack.setOutputLayout(layout);
            DisplayService::setOutputs(layout);
            // Now that outputs exist, apply the persisted UI scale to the primary
            // output (recomputes logical dims + notifies clients), live on change.
            DisplayScaleService::init([](uint32_t pct) {
                DisplayService::applyOutputScale(&window_stack, 0, static_cast<float>(pct) / 100.0f);
            });
        }

        // com.freebsdx.DisplayArrange reads the live output list from window_stack.
        DisplayArrangeService::init([]() {
            std::vector<disparrange::ArrangeOutput> v;
            const auto& outs = window_stack.outputLayout();
            for (size_t i = 0; i < outs.size(); ++i) {
                disparrange::ArrangeOutput o;
                o.width       = static_cast<uint32_t>(outs[i].phys_width_);
                o.height      = static_cast<uint32_t>(outs[i].phys_height_);
                o.refresh_mhz = 0;              // not tracked in OutputInfo
                o.primary     = (i == 0);
                o.mirroring   = false;
                o.name        = "output-" + std::to_string(i);
                v.push_back(std::move(o));
            }
            return v;
        });

        // Upload the hardware cursor scaled to the primary output's DPI.
        (void)drm->initCursor(window_stack.getOutputScale(0) * g_cursor_size_factor);

        // Wire up DRM page-flip timestamps → wp_presentation feedback.
        drm->setFlipCallback([drm_ptr](uint32_t crtc_id, uint32_t seq,
                                        uint32_t tv_sec, uint32_t tv_usec) {
            uint32_t refresh_ns = 16666667u; // default: 60 Hz
            for (const auto& out : drm_ptr->outputs()) {
                if (out.crtc_id == crtc_id && out.mode.refresh_rate_ > 0) {
                    refresh_ns = 1000000000u / out.mode.refresh_rate_;
                    break;
                }
            }
            shared::PresentFeedback fb{};
            fb.tv_sec_lo_  = tv_sec;
            fb.tv_nsec_    = tv_usec * 1000u;
            fb.refresh_ns_ = refresh_ns;
            fb.seq_lo_     = seq;
            BroadcastPresentFeedback(fb);
        });
    }

    // Headless mode — set up a single synthetic output and init HeadlessService.
    // DRM was skipped above (virtual_boot=true), so we configure the window
    // manager and display broker here with the virtual resolution.
    if (headless) {
        // Recreate output-0 target at the requested headless resolution.
        // (VulkanCompositor::create() defaults to 1920×1080; this corrects it
        // when --headless-size specifies something different.)
        (void)comp->createTarget(0, screen_w, screen_h);

        OutputInfo oi{};
        oi.phys_width_  = oi.width_  = static_cast<float>(screen_w);
        oi.phys_height_ = oi.height_ = static_cast<float>(screen_h);
        oi.scale_       = 1.0f;
        oi.x_offset_    = 0.0f;
        window_stack.setScreenSize(static_cast<float>(screen_w),
                                   static_cast<float>(screen_h));
        window_stack.setOutputLayout({oi});
        DisplayService::setOutputs({oi});

        HeadlessService::init(comp.get(), screen_w, screen_h);
        // Let the test harness read the live cursor position (GetPointer) and the
        // decided cursor shape (GetCursorShape) — both observable without DRM.
        HeadlessService::setPointerQuery([]() {
            return std::pair{cursor_x, cursor_y};
        });
        HeadlessService::setCursorShapeQuery([]() { return g_cursor_shape; });
        LOG_INFO("Headless mode active: virtual output {}×{} (no DRM/KMS)",
                 screen_w, screen_h);
    }

    // DRM hot-plug monitor: listens for connector add/remove events via udev.
    struct udev* udev_ctx = udev_new();
    std::unique_ptr<UdevMonitor> drm_hotplug_monitor;

    if (udev_ctx && drm_ptr) {
        auto mon_res = UdevMonitor::create(udev_ctx, "drm",
            [comp, drm_ptr]() noexcept {
                std::cout << "[xserver] Hot-plug event: rescanning DRM outputs...\n" << std::flush;

                // Rebuild connector → CRTC → plane assignments
                (void)drm_ptr->rescan();
                const auto& new_outputs = drm_ptr->outputs();

                // Drop compositor targets for outputs that no longer exist
                comp->trimOutputs(new_outputs.size());

                // Enable all current outputs (modeset for new ones; refresh for existing)
                (void)drm_ptr->forceEnableDisplay();

                // (Re-)create Vulkan render targets at each output's native resolution
                for (size_t i = 0; i < new_outputs.size(); ++i) {
                    (void)comp->createTarget(i,
                        new_outputs[i].mode.width_, new_outputs[i].mode.height_);
                }

                // Release stale DRM framebuffer objects — they're detached from
                // CRTCs by the forceEnableDisplay() modeset above, so drmModeRmFB
                // is now safe. New fb_ids are created on the next render frame.
                releaseFbCache(drm_ptr);

                // Rebuild window manager and display broker output layout.
                // setOutputLayout preserves existing per-output scale factors.
                {
                    std::vector<OutputInfo> layout;
                    float x_logical = 0.0f;
                    for (const auto& out : new_outputs) {
                        float pw = static_cast<float>(out.mode.width_);
                        float ph = static_cast<float>(out.mode.height_);
                        OutputInfo oi{};
                        oi.phys_width_  = pw;
                        oi.phys_height_ = ph;
                        oi.scale_       = 1.0f; // preserved by setOutputLayout if already set
                        oi.width_       = pw;
                        oi.height_      = ph;
                        oi.x_offset_    = x_logical;
                        layout.push_back(oi);
                        x_logical += pw; // will be corrected by setOutputLayout after scale preservation
                    }
                    // Re-probe VRR capability after hot-plug (new display may differ).
                    for (size_t i = 0; i < layout.size(); ++i) {
                        bool capable = drm_ptr->isVrrCapable(i);
                        layout[i].vrr_capable_ = capable;
                        if (capable) {
                            auto res = drm_ptr->setVrrEnabled(i, true);
                            layout[i].vrr_enabled_ = res.has_value();
                        }
                        window_stack.setVrrCapable(i, layout[i].vrr_capable_);
                        window_stack.setVrrEnabled(i, layout[i].vrr_enabled_);
                    }

                    // Re-probe HDR capability after hot-plug.
                    for (size_t i = 0; i < layout.size(); ++i) {
                        bool hdr_capable = drm_ptr->isHdrCapable(i);
                        layout[i].hdr_capable_ = hdr_capable;
                        window_stack.setHdrCapable(i, hdr_capable);
                    }

                    window_stack.setOutputLayout(layout);
                    DisplayService::setOutputs(window_stack.outputLayout());
                    DisplayService::BroadcastOutputChanged();
                }

                // Re-upload cursor image — new CRTCs need it after modeset.
                if (!comp || !comp->isGpuCursorEnabled())
                    (void)drm_ptr->initCursor(window_stack.getOutputScale(0) * g_cursor_size_factor);

                std::cout << "[xserver] Hot-plug rescan complete: "
                          << new_outputs.size() << " output(s) active.\n" << std::flush;
            });

        if (mon_res) {
            drm_hotplug_monitor = std::move(mon_res.value());
            UdevMonitor* mon_ptr = drm_hotplug_monitor.get();
            (void)loop->addFd(drm_hotplug_monitor->getFd(),
                [mon_ptr]() { mon_ptr->dispatch(); });
            std::cout << "[xserver] DRM hot-plug monitor active.\n" << std::flush;
        } else {
            std::cerr << "[xserver] Warning: DRM hot-plug monitor unavailable.\n";
        }
    }

    // SOCK_SEQPACKET (not STREAM): preserve message boundaries so the compositor's
    // one-message-per-read receive path can't coalesce/desync under burst traffic
    // (see IpcSocket::connect). Accepted client fds inherit this type. All clients
    // must connect with SOCK_SEQPACKET to match.
    UniqueFd master_fd(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (!master_fd) {
        LOG_ERROR("Failed to create IPC socket: {}", strerror(errno));
        return 1;
    }

    struct sockaddr_un m_addr{}; m_addr.sun_family = AF_UNIX;
    strncpy(m_addr.sun_path, kSocketPath, sizeof(m_addr.sun_path)-1);
    unlink(kSocketPath);
    
    if (bind(master_fd.get(), (struct sockaddr*)&m_addr, sizeof(m_addr)) != 0) {
        LOG_ERROR("Failed to bind IPC socket to {}: {}", kSocketPath, strerror(errno));
        return 1;
    }

    if (listen(master_fd.get(), 5) != 0) {
        LOG_ERROR("Failed to listen on IPC socket: {}", strerror(errno));
        return 1;
    }

    chmod(kSocketPath, 0666);
    std::cout << "[xserver] IPC Socket LISTENING at " << kSocketPath << "\n" << std::flush;
    
    (void)loop->addFd(master_fd.get(), [&, m_fd = master_fd.get()]() {
        struct sockaddr_un c_addr; socklen_t c_len = sizeof(c_addr);
        // SOCK_NONBLOCK is essential: the compositor is single-threaded, so a
        // blocking sendmsg to a client whose receive buffer has filled (e.g. a
        // busy-rendering xshell that isn't draining broadcasts fast enough) would
        // stall the whole event loop — freezing input, XBus and presentation.
        // Non-blocking sends return EAGAIN instead and simply drop that message.
        int c_fd = accept4(m_fd, (struct sockaddr*)&c_addr, &c_len, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (c_fd >= 0) {
            std::cout << "[xserver] IPC Connection ACCEPTED (FD: " << c_fd << ")\n" << std::flush;
            auto c_sock = std::make_shared<IpcSocket>(UniqueFd(c_fd));
            RegisterClientForBroadcast(c_sock);
            (void)loop->addFd(c_fd, [c_sock, c_fd, comp, screen_w, screen_h, drm_ptr]() {
                // Zero-alloc receive: data lands in a pool slot, no heap allocation.
                UniqueFd f;
                auto slot_res = c_sock->receiveIntoSlot(XBusMessagePool::global(), f);
                if (!slot_res) {
                    if (slot_res.error().value() != EAGAIN && slot_res.error().value() != EWOULDBLOCK) {
                        std::cout << "[xserver] Client disconnected or error on FD " << c_fd << ": " << slot_res.error().message() << "\n" << std::flush;
                    }
                    g_loop->removeFd(c_fd);
                    // Do NOT close(c_fd) here: c_sock's IpcSocket owns this fd via
                    // UniqueFd and closes it exactly once when the socket is released.
                    // A raw close() frees the fd number while UniqueFd still owns it;
                    // the next accept4() (e.g. the shell reconnecting right after the
                    // greeter exits) reuses that number, then the lingering UniqueFd
                    // destructor closes it again — killing the new client's socket.
                    // That double-close was the intermittent greeter->shell handoff
                    // failure (EBADF / ECONNABORTED on the freshly-connected shell).

                    std::vector<uint32_t> to_remove;
                    for (const auto& [id, client] : g_surface_clients) {
                        if (client == c_sock) to_remove.push_back(id);
                    }
                    for (uint32_t id : to_remove) {
                        window_stack.removeWindow(id);
                        releaseSurfaceImages(comp.get(), id);
                        g_surface_clients.erase(id);
                        g_subsurface_ids.erase(id);
                        g_layer_surface_ids.erase(id);
                    }
                    // Detach this client from any terminal sessions it held.
                    TerminalService::OnClientDisconnect(c_sock);
                    return;
                }
                // `slot` is RAII — released back to the pool at end of scope.
                auto& slot = *slot_res;
                auto d = slot->span(); // std::span<const uint8_t>, no copy
                
                bool has_focus = false;
                if (g_surface_clients.contains(focused_surface)) {
                    has_focus = (g_surface_clients[focused_surface] == c_sock);
                }

                // Everything here runs on the compositor thread: a slow handler
                // freezes input and drawing. Name any that takes over 50 ms.
                const auto t_handler = std::chrono::steady_clock::now();
                XBusResult x_res = XBusDispatcher::ProcessMessage(c_sock, d, f, comp.get(), &window_stack, has_focus, drm_ptr);
                if (auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t_handler).count();
                    ms > 50 && d.size() >= sizeof(shared::XBusMessage)) {
                    const auto* m = reinterpret_cast<const shared::XBusMessage*>(d.data());
                    if (m->magic_ == shared::XBUS_MAGIC)
                        std::cerr << "[xserver] slow XBus handler: " << std::string(m->service_name_, strnlen(m->service_name_, sizeof(m->service_name_)))
                                  << " op " << m->opcode_ << " took " << ms << " ms (blocks the desktop)\n";
                }
                if (x_res.consumed) return;
                
                // Wayland bridge title update (64 bytes, distinct from RenderMessage 36)
                if (d.size() == sizeof(BridgeTitleMessage)) {
                    const auto* t = reinterpret_cast<const BridgeTitleMessage*>(d.data());
                    window_stack.setWindowTitle(t->surface_id_, t->title_);
                    RebuildAndBroadcastWindowList(&window_stack);
                    return;
                }

                if (d.size() >= sizeof(RenderMessage)) {
                    const auto* r = reinterpret_cast<const RenderMessage*>(d.data());
                    if (r->status_ == RenderMessageStatus::SubsurfaceCreated) {
                        // stride_ = parent_id, damage_x_ = sub_x, damage_y_ = sub_y
                        if (g_surface_clients.size() >= kMaxSurfaces) {
                            std::cerr << "[xserver] Surface cap (" << kMaxSurfaces << ") reached; ignoring subsurface ID " << r->surface_id_ << "\n";
                            return;
                        }
                        uint32_t parent_id = r->stride_;
                        window_stack.addSubsurface(r->surface_id_, parent_id,
                                                   r->damage_x_, r->damage_y_);
                        g_surface_clients[r->surface_id_] = c_sock;
                        g_subsurface_ids.insert(r->surface_id_);
                        std::cout << "[xserver] Subsurface CREATED (ID: " << r->surface_id_
                                  << " parent=" << parent_id << ")\n";
                        // No SurfaceReady reply needed — subsurfaces have no xdg_toplevel
                    } else if (r->status_ == RenderMessageStatus::LayerSurfaceCreated) {
                        // stride_ = layer (0-3), damage_x_/y_ = resolved position, width_/height_ = size
                        if (g_surface_clients.size() >= kMaxSurfaces) {
                            std::cerr << "[xserver] Surface cap (" << kMaxSurfaces << ") reached; ignoring layer surface ID " << r->surface_id_ << "\n";
                            return;
                        }
                        g_surface_clients[r->surface_id_] = c_sock;
                        g_layer_surface_ids.insert(r->surface_id_);
                        window_stack.addLayerSurface(r->surface_id_, r->stride_,
                                                      r->damage_x_, r->damage_y_,
                                                      static_cast<float>(r->width_),
                                                      static_cast<float>(r->height_));
                        std::cout << "[xserver] Layer Surface CREATED (ID: " << r->surface_id_
                                  << " layer=" << r->stride_ << ")\n";
                        // No SurfaceReady reply; bridge manages configure flow directly.
                    } else if (r->status_ == RenderMessageStatus::DialogModalSet) {
                        window_stack.setModal(r->surface_id_, true);
                        std::cout << "[xserver] Dialog modal SET (ID: " << r->surface_id_ << ")\n";
                    } else if (r->status_ == RenderMessageStatus::DialogModalUnset) {
                        window_stack.setModal(r->surface_id_, false);
                        std::cout << "[xserver] Dialog modal UNSET (ID: " << r->surface_id_ << ")\n";
                    } else if (r->status_ == RenderMessageStatus::ToplevelParentSet) {
                        uint32_t parent_id = r->stride_;
                        window_stack.setToplevelParent(r->surface_id_, parent_id);
                        std::cout << "[xserver] Toplevel parent set: " << r->surface_id_
                                  << " → parent " << parent_id << "\n";
                    } else if (r->status_ == RenderMessageStatus::ResizeBegin) {
                        uint32_t edges = r->stride_;
                        window_stack.startResize(r->surface_id_, {cursor_x, cursor_y}, edges);
                    } else if (r->status_ == RenderMessageStatus::SurfaceCreated || r->status_ == RenderMessageStatus::ShellSurfaceCreated) {
                        // Surface id 0 is never legitimate: the shell is 999 and apps
                        // use a nonzero (pid-derived) id. A zero id here means a stray
                        // or misframed message got parsed as a RenderMessage (the XBus
                        // stream is unframed). Honouring it would add an empty
                        // (VK_NULL_HANDLE) window over the desktop — hiding the icons —
                        // and steal input focus (focused_surface = 0), so drop it.
                        if (r->surface_id_ == 0) {
                            // Defensive: surface id 0 is never valid. With the
                            // SOCK_SEQPACKET transport this should no longer occur,
                            // but keep the guard so a stray/malformed message can't
                            // create a phantom window that hides the desktop.
                            std::cerr << "[xserver] Ignoring SurfaceCreated with invalid id 0\n";
                            return;
                        }
                        if (g_surface_clients.size() >= kMaxSurfaces) {
                            std::cerr << "[xserver] Surface cap (" << kMaxSurfaces << ") reached; rejecting surface ID " << r->surface_id_ << "\n";
                            // Send SurfaceReady with zero dims so the client unblocks and can exit cleanly.
                            RenderMessage rej{};
                            rej.surface_id_ = r->surface_id_; rej.status_ = RenderMessageStatus::SurfaceReady;
                            rej.width_ = 0; rej.height_ = 0; rej.stride_ = 0;
                            (void)c_sock->sendMessage({reinterpret_cast<const uint8_t*>(&rej), sizeof(rej)});
                            return;
                        }
                        bool is_s = (r->status_ == RenderMessageStatus::ShellSurfaceCreated);
                        // Logical screen dims (apps lay out in logical pixels; the
                        // compositor scales to physical).
                        float sc0 = window_stack.getOutputScale(0);
                        uint32_t full_w = static_cast<uint32_t>(screen_w / sc0);
                        uint32_t full_h = static_cast<uint32_t>(screen_h / sc0);
                        uint32_t win_w, win_h;
                        if (is_s) {
                            std::cout << "[xserver] Shell Surface CREATED (ID: " << r->surface_id_ << ")\n";
                            c_sock->setTrusted(true);
                            win_w = full_w; win_h = full_h;
                            window_stack.addShellWindow(r->surface_id_, VK_NULL_HANDLE, screen_w, screen_h);
                        } else {
                            std::cout << "[xserver] App Surface CREATED (ID: " << r->surface_id_ << ")\n";
                            // Apps open as a centered window, not fullscreen, so the
                            // desktop/panels stay visible around them and the window's
                            // close button isn't pinned to the top-left launcher area.
                            win_w = std::min<uint32_t>(1200u, static_cast<uint32_t>(full_w * 0.72f));
                            win_h = std::min<uint32_t>(820u,  static_cast<uint32_t>(full_h * 0.80f));
                            window_stack.addWindow(r->surface_id_, VK_NULL_HANDLE,
                                                   static_cast<float>(win_w), static_cast<float>(win_h));
                        }
                        g_surface_clients[r->surface_id_] = c_sock;
                        if (!is_s) {
                            focused_surface = r->surface_id_;
                            RebuildAndBroadcastWindowList(&window_stack);
                        }
                        RenderMessage a{};
                        a.surface_id_ = r->surface_id_; a.status_ = RenderMessageStatus::SurfaceReady; a.width_ = win_w; a.height_ = win_h; a.stride_ = comp->getStride();
                        (void)c_sock->sendMessage({reinterpret_cast<const uint8_t*>(&a), sizeof(a)});
                    } else if (r->status_ == RenderMessageStatus::FrameReady) {
                        static int frame_ready_count = 0;
                        if (frame_ready_count++ % 60 == 0) std::cout << "[xserver] FrameReady received from ID: " << r->surface_id_ << " (Valid FD: " << (f.isValid() ? "YES" : "NO") << ")\n";
                        if (!g_surface_clients.contains(r->surface_id_)) {
                            std::cerr << "[xserver] FrameReady for unknown surface " << r->surface_id_ << "; discarding\n";
                            return;
                        }
                        if (f.isValid()) {
                            // For subsurfaces, damage_x_/y_ carry the updated sub-position
                            if (g_subsurface_ids.contains(r->surface_id_))
                                window_stack.updateSubsurfacePosition(r->surface_id_,
                                                                      r->damage_x_, r->damage_y_);
                            // Double-buffering: the client alternates between two
                            // render buffers and names the one it just finished in
                            // buffer_index_. Import each buffer once (they alias
                            // distinct client memory), then composite the named one.
                            // The client is drawing into the OTHER buffer now, so
                            // the scanout never samples a buffer mid-rewrite.
                            uint32_t bi = r->buffer_index_ & 1;
                            auto& bufs = g_surface_buffers[r->surface_id_]; // value-inits to {NULL,NULL}
                            if (bufs[bi] == VK_NULL_HANDLE) {
                                auto i = comp->importExternalBuffer(f.release(), r->width_, r->height_);
                                if (i) bufs[bi] = i.value();
                            }
                            if (bufs[bi] != VK_NULL_HANDLE) {
                                g_surface_images[r->surface_id_] = bufs[bi]; // active alias (non-owning)
                                window_stack.updateSurface(r->surface_id_, bufs[bi], r->width_, r->height_, r->damage_x_, r->damage_y_, r->damage_width_, r->damage_height_);
                            }
                            // Apply committed window geometry (shadows/CSD border offset).
                            if (r->geom_w_ > 0)
                                window_stack.setWindowGeometry(r->surface_id_,
                                    r->geom_x_, r->geom_y_, r->geom_w_, r->geom_h_);
                        }
                        g_scene_dirty = true;  // a client presented a new frame
                    } else if (r->status_ == RenderMessageStatus::SolidColor) {
                        // stride_ = ARGB color (0xAARRGGBB); no fd — always recreate image
                        if (!g_surface_clients.contains(r->surface_id_)) {
                            std::cerr << "[xserver] SolidColor for unknown surface " << r->surface_id_ << "; discarding\n";
                            return;
                        }
                        releaseSurfaceImages(comp.get(), r->surface_id_);
                        auto i = comp->createSolidColorImage(r->stride_);
                        if (i) {
                            g_surface_images[r->surface_id_] = i.value();
                            window_stack.updateSurface(r->surface_id_, i.value(),
                                r->width_, r->height_,
                                r->damage_x_, r->damage_y_,
                                r->damage_width_, r->damage_height_);
                        }
                        g_scene_dirty = true;  // solid-color surface updated
                    } else if (r->status_ == RenderMessageStatus::SurfaceClosed) {
                        g_scene_dirty = true;  // a window went away
                        window_stack.removeWindow(r->surface_id_);
                        RebuildAndBroadcastWindowList(&window_stack);
                        releaseSurfaceImages(comp.get(), r->surface_id_);
                        if (g_surface_clients.contains(r->surface_id_)) g_surface_clients.erase(r->surface_id_);
                        g_subsurface_ids.erase(r->surface_id_);
                        g_layer_surface_ids.erase(r->surface_id_);
                    }
                }
            });
        }
    });

    static float last_cursor_x = cursor_x;
    static float last_cursor_y = cursor_y;

    // Shared pointer-motion processing — active-resize drive, server-side
    // hover-cursor arbitration, and the hardware cursor move — run for both real
    // (libinput) and injected input so injected motion drives the same cursor
    // logic. Assumes cursor_x/cursor_y are already updated. The cursor-shape
    // *decision* runs regardless of drm_ptr (so it is observable in headless);
    // only the DRM plane update and hardware cursor move are guarded.
    auto process_pointer_motion = [comp, drm_ptr]() noexcept {
        // DnD: update the target surface under the cursor (drives copy/move/no_drop
        // and the DragEnter/DragLeave delivered to clients).
        {
            auto& dnd = GetDnDState();
            if (dnd.active) {
                fbx::xkit::Offset dpos{cursor_x, cursor_y};
                uint32_t new_target = window_stack.bodyHitTest(dpos).value_or(UINT32_MAX);
                if (new_target == dnd.source_surface)
                    new_target = UINT32_MAX;  // ignore drops back on the source
                if (new_target != dnd.current_target) {
                    if (dnd.current_target != UINT32_MAX &&
                        g_surface_clients.contains(dnd.current_target)) {
                        DnDSendToClient(g_surface_clients[dnd.current_target],
                            shared::DnDOpcode::DragLeave, 0.0f, 0.0f, dnd.mime_type);
                    }
                    if (new_target != UINT32_MAX &&
                        g_surface_clients.contains(new_target)) {
                        const auto& wins = window_stack.getWindows();
                        auto wit = std::find_if(wins.begin(), wins.end(),
                            [new_target](const Window& w){ return w.surface_id_ == new_target; });
                        float rx = cursor_x - (wit != wins.end() ? wit->position_.x_ : 0);
                        float ry = cursor_y - (wit != wins.end() ? wit->position_.y_ : 0);
                        DnDSendToClient(g_surface_clients[new_target],
                            shared::DnDOpcode::DragEnter, rx, ry, dnd.mime_type);
                    }
                    dnd.current_target = new_target;
                }
            }
        }

        auto resize_res = window_stack.updateResize({cursor_x, cursor_y});
        if (resize_res) {
            auto [rsid, rsize] = *resize_res;
            if (g_surface_clients.contains(rsid)) {
                RenderMessage cfg{
                    .surface_id_ = rsid,
                    .status_     = RenderMessageStatus::ConfigureRequest,
                    .width_      = static_cast<uint32_t>(rsize.width_),
                    .height_     = static_cast<uint32_t>(rsize.height_)
                };
                (void)g_surface_clients[rsid]->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&cfg), sizeof(cfg)});
            }
        }

        uint32_t want = g_cursor_shape;
        if (GetDnDState().active) {
            // Reflect the drag-and-drop operation: no valid drop target under the
            // pointer → not-allowed; over a target → copy (Ctrl held) or move.
            if (GetDnDState().current_target == UINT32_MAX)
                want = 14;  // no_drop
            else
                want = (g_pointer_modifiers & 2u) ? 12u : 13u;  // Ctrl=copy, else move
        } else if (resize_res) {
            // active resize — keep the cursor chosen on hover
        } else if (window_stack.isDragging()) {
            want = 1;   // default arrow while moving a window
        } else if (auto rh = window_stack.resizeHitTest({cursor_x, cursor_y})) {
            uint32_t e = rh->second;  // edge bitmask: L=4 R=8 B=2
            if      (e == (2u | 8u)) want = 23;  // bottom-right → se_resize
            else if (e == (2u | 4u)) want = 24;  // bottom-left  → sw_resize
            else if (e & 4u)         want = 25;  // left         → w_resize
            else if (e & 8u)         want = 18;  // right        → e_resize
            else if (e & 2u)         want = 22;  // bottom       → s_resize
        } else {
            auto hit = window_stack.bodyHitTest({cursor_x, cursor_y});
            uint32_t hov = hit.value_or(UINT32_MAX);
            bool crossed = (hov != g_hover_surface);
            g_hover_surface = hov;
            // Over the bare desktop no app owns the cursor, so it must revert to
            // the default arrow — otherwise a cursor a client last set (e.g. an
            // I-beam/hand/wait in shapes 2..11, or a lingering server-owned DnD
            // 12..14 or grab/resize 17..31 cursor) stays stuck there. Also reset
            // when crossing onto a new surface.
            if (!hit || crossed ||
                (g_cursor_shape >= 12 && g_cursor_shape <= 14) ||
                (g_cursor_shape >= 17 && g_cursor_shape <= 31))
                want = 1;
        }
        if (want != g_cursor_shape) {
            g_cursor_shape = want;
            if (drm_ptr) drm_ptr->setCursorShape(want, window_stack.getOutputScale(0) * g_cursor_size_factor);
        }

        if (comp) comp->updateCursorPosition(cursor_x, cursor_y);
        if (drm_ptr) {
            const auto& layout = window_stack.outputLayout();
            for (size_t i = 0; i < layout.size(); ++i) {
                float sc = layout[i].scale_;
                int32_t lx = static_cast<int32_t>((cursor_x - layout[i].x_offset_) * sc);
                int32_t ly = static_cast<int32_t>(cursor_y * sc);
                drm_ptr->moveCursor(i, lx, ly);
            }
        }
    };

    // Prune expired notifications every 5 seconds (timer id=2)
    (void)loop->addTimer(2, 5000, []() {
        NotificationService::cleanup();
    });

    // Poll AC/battery state every 30 seconds (timer id=3) and broadcast
    // PowerChanged to all clients if the AC↔battery state changed.
    (void)loop->addTimer(3, 30000, []() {
        PowerService::pollAndBroadcast();
    });

    // Check idle-lock and idle-screensaver timeouts every 10 seconds (timer id=4).
    (void)loop->addTimer(4, 10000, []() {
        ScreenSaverService::pollIdle();
        LockService::pollIdle();
        OptStorageService::poll();   // ≤ hourly: old-cache measurement / cleanup
    });

    // Poll calendar reminders and delayed screenshots every second (timer id=5).
    (void)loop->addTimer(5, 1000, []() {
        CalendarService::pollReminders();
        ScreenCaptureService::poll();   // delayed screenshots
    });


    (void)loop->addTimer(1, 16, [comp, screen_w, screen_h, drm_ptr, &process_pointer_motion]() {
        if (!g_seat_active) return;   // seat handed to another VT — don't touch the GPU
        if (g_flip_pending) {
            // Watchdog: a healthy flip completes in ~1 frame (16ms @60Hz). If one
            // hasn't in 200ms, its completion event was dropped (a lost flip would
            // otherwise wedge the display black forever) — force-clear and re-render.
            auto waited = std::chrono::steady_clock::now() - g_flip_issued;
            if (waited < std::chrono::milliseconds(200)) return;
            LOG_WARN("[flip] page-flip completion not seen in 200ms — recovering (dropped event?)");
            g_flip_pending = false;
        }

        float dx = 0, dy = 0, dw = 0, dh = 0;
        if (cursor_x != last_cursor_x || cursor_y != last_cursor_y) {
            dx = std::min(cursor_x, last_cursor_x) - 10;
            dy = std::min(cursor_y, last_cursor_y) - 10;
            dw = std::abs(cursor_x - last_cursor_x) + 40;
            dh = std::abs(cursor_y - last_cursor_y) + 40;
            last_cursor_x = cursor_x; last_cursor_y = cursor_y;
        }

        MotionEngine::step(0.016f, window_stack);
        for (const auto& w : window_stack.getWindows()) {
            if (std::abs(w.position_.x_ - w.target_position_.x_) > 0.1f ||
                std::abs(w.position_.y_ - w.target_position_.y_) > 0.1f) {
                dx = 0; dy = 0; dw = screen_w; dh = screen_h;
                break;
            }
        }

        // Use DRM's authoritative output count when hardware is present;
        // fall back to compositor's own count in headless/virtual mode.
        size_t num_outputs = (drm_ptr && !drm_ptr->outputs().empty())
            ? drm_ptr->outputs().size()
            : std::max<size_t>(1, comp->outputCount());
        // Composite + flip only when the scene changed or an animation is in
        // flight; otherwise hold the last scanned-out frame. Continuously
        // re-flipping an unchanging scene blinked the whole display on the login
        // greeter (the composited content is provably identical frame-to-frame).
        // idle_skip forces a redraw at least every ~0.5s so a missed dirty
        // signal can never wedge the screen.
        bool animating = std::abs(window_stack.scene_offset_ -
                                  window_stack.target_scene_offset_) > 0.1f;
        if (!animating) {
            for (const auto& w : window_stack.getWindows()) {
                if (std::abs(w.position_.x_ - w.target_position_.x_) > 0.1f ||
                    std::abs(w.position_.y_ - w.target_position_.y_) > 0.1f ||
                    std::abs(w.size_.width_  - w.target_size_.width_)  > 0.1f ||
                    std::abs(w.size_.height_ - w.target_size_.height_) > 0.1f ||
                    std::abs(w.scale_        - w.target_scale_)        > 0.001f ||
                    std::abs(w.opacity_      - w.target_opacity_)      > 0.001f) {
                    animating = true;
                    break;
                }
            }
        }
        static int idle_skip = 0;
        bool must_render = g_scene_dirty || animating || (++idle_skip >= 30);
        // Wrapped (not early-return) so the input drains further below always run.
        if (must_render) {
        idle_skip = 0;
        g_scene_dirty = false;

        comp->setFocusedSurface(focused_surface);
        for (size_t i = 0; i < num_outputs; ++i) {
            (void)comp->composite(i, window_stack, cursor_x, cursor_y, dx, dy, dw, dh);
        }
        window_stack.clearDamage();

        // In headless mode the frame is already composited into the VkImage;
        // there is no DRM device to scan out to, so just count the frame.
        if (HeadlessService::isHeadless()) {
            HeadlessService::incrementFrameCount();
        } else {
            // Export and flip each output to the DRM kernel
            for (size_t i = 0; i < num_outputs; ++i) {
                auto export_res = comp->exportTarget(i);
                if (!export_res) continue;

                uint32_t fb_id = 0;
                uint32_t buf_idx = (comp->getCurrentFrame(i) - 1) % 2;
                uint32_t cache_key = static_cast<uint32_t>(i) * 1000000u + buf_idx;

                if (g_fb_cache.contains(cache_key)) {
                    fb_id = g_fb_cache[cache_key];
                } else if (drm_ptr && i < drm_ptr->outputs().size()) {
                    const auto& out = drm_ptr->outputs()[i];
                    auto fb_res = drm_ptr->importFdAndAddFb(
                        export_res.value(), out.mode.width_, out.mode.height_,
                        comp->getDrmFormat(i), comp->getStride(i));
                    if (fb_res) {
                        fb_id = fb_res.value();
                        g_fb_cache[cache_key] = fb_id;
                    } else {
                        LOG_ERROR("Output {}: failed to import compositor buffer: {}", i, fb_res.error().message());
                    }
                }

                if (fb_id != 0 && drm_ptr) {
                    // Only gate the frame-rate on the primary output's page flip.
                    // Secondary outputs fire-and-forget (nullptr user_data).
                    void* flip_user_data = (i == 0) ? &g_flip_pending : nullptr;
                    auto flip_res = drm_ptr->flipOutput(i, fb_id, flip_user_data);
                    if (flip_res) {
                        if (i == 0) { g_flip_pending = true; g_flip_issued = std::chrono::steady_clock::now(); }
                        if (i == 0) SystemService::incrementFrameCount();
                    } else {
                        LOG_ERROR("Output {}: DRM page flip failed: {}", i, flip_res.error().message());
                    }
                }
                close(export_res.value());
            }
        }
        }  // end if (must_render)

        // Drain remotely injected input from VNC clients and forward to focused app
        {
            auto injected = RemoteDesktopService::drainInjectedInput();
            if (!injected.empty()) g_scene_dirty = true;  // remote input moves things
            for (const auto& imsg : injected) {
                LockService::resetIdleTimer();
                ScreenSaverService::resetIdleTimer();
                if (imsg.type_ == InputEventType::MouseMove) {
                    cursor_x += static_cast<float>(imsg.value_a_);
                    cursor_y += static_cast<float>(imsg.value_b_);
                    cursor_x = std::clamp(cursor_x, 0.0f, static_cast<float>(screen_w));
                    cursor_y = std::clamp(cursor_y, 0.0f, static_cast<float>(screen_h));
                    // Same processing as real input: hover arbitration + cursor move.
                    g_pointer_modifiers = imsg.modifiers_;
                    process_pointer_motion();
                }
                if (g_surface_clients.contains(focused_surface)) {
                    shared::InputMessage out = imsg;
                    if (out.type_ == InputEventType::MouseMove) {
                        // Deliver surface-local absolute coordinates (see primary
                        // input path); clients no longer accumulate relative deltas.
                        auto o = window_stack.windowOrigin(focused_surface);
                        out.value_a_ = static_cast<int32_t>(cursor_x - o.x_);
                        out.value_b_ = static_cast<int32_t>(cursor_y - o.y_);
                    }
                    (void)g_surface_clients[focused_surface]->sendMessage(
                        {reinterpret_cast<const uint8_t*>(&out), sizeof(out)});
                }
            }
        }

        // Drain virtual keyboard injected keys → focused surface
        {
            auto vk_keys = VirtualKeyboardService::drainInjectedKeys();
            if (!vk_keys.empty()) g_scene_dirty = true;
            for (const auto& imsg : vk_keys) {
                if (g_surface_clients.contains(focused_surface)) {
                    (void)g_surface_clients[focused_surface]->sendMessage(
                        {reinterpret_cast<const uint8_t*>(&imsg), sizeof(imsg)});
                }
            }
        }
    });

    if (input) {
        (void)loop->addFd(input->getFd(), [&]() {
            auto msgs_res = input->dispatch();
            if (msgs_res) {
                if (!msgs_res.value().empty())
                    g_scene_dirty = true;  // input may move the cursor / focus ring
                for (auto& msg : msgs_res.value()) {
                    // --- Reset idle timers on any user input ---
                    LockService::resetIdleTimer();
                    ScreenSaverService::resetIdleTimer();

                    // Always track the hardware cursor on pointer motion, even
                    // when input is redirected to the greeter or lock screen —
                    // otherwise the cursor freezes on those screens (it's a DRM
                    // plane the compositor owns, not the client's to move).
                    if (msg.type_ == InputEventType::MouseMove &&
                        (LoginService::isGreeterActive() || LockService::isLocked())) {
                        cursor_x = std::clamp(cursor_x + static_cast<float>(msg.value_a_),
                                              0.0f, static_cast<float>(screen_w));
                        cursor_y = std::clamp(cursor_y + static_cast<float>(msg.value_b_),
                                              0.0f, static_cast<float>(screen_h));
                        g_pointer_modifiers = msg.modifiers_;
                        process_pointer_motion();
                    }

                    // ── While login greeter is active, redirect all input to it ────────────
                    if (LoginService::isGreeterActive()) {
                        auto greeter = LoginService::greeterClient();
                        if (greeter) {
                            // The greeter is a fullscreen surface at the origin,
                            // and its client treats incoming mouse coordinates as
                            // absolute (surface-local). libinput motion is a
                            // *relative* delta, though, so forward the tracked
                            // absolute cursor position instead — otherwise the
                            // greeter's cursor stays pinned near (0,0) and clicks
                            // can't land on fields (e.g. the password box).
                            shared::InputMessage gm = msg;
                            if (gm.type_ == InputEventType::MouseMove) {
                                gm.value_a_ = static_cast<int32_t>(cursor_x);
                                gm.value_b_ = static_cast<int32_t>(cursor_y);
                            }
                            (void)greeter->sendMessage(
                                {reinterpret_cast<const uint8_t*>(&gm), sizeof(gm)});
                            continue;
                        }
                    }

                    // --- While locked, redirect all input to the lock screen ---
                    if (LockService::isLocked()) {
                        auto ls_client = LockService::lockscreenClient();
                        if (ls_client) {
                            // Allow media keys through even while locked
                            bool is_media_key =
                                msg.type_ == InputEventType::AudioVolumeUp   ||
                                msg.type_ == InputEventType::AudioVolumeDown ||
                                msg.type_ == InputEventType::AudioMute       ||
                                msg.type_ == InputEventType::BrightnessUp    ||
                                msg.type_ == InputEventType::BrightnessDown;
                            if (!is_media_key) {
                                (void)ls_client->sendMessage(
                                    {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
                                continue;
                            }
                        }
                    }

                    // --- Mouse Move ---
                    if (msg.type_ == InputEventType::MouseMove) {
                        cursor_x += static_cast<float>(msg.value_a_);
                        cursor_y += static_cast<float>(msg.value_b_);
                        cursor_x = std::clamp(cursor_x, 0.0f, static_cast<float>(screen_w));
                        cursor_y = std::clamp(cursor_y, 0.0f, static_cast<float>(screen_h));
                        window_stack.updateDrag({cursor_x, cursor_y});

                        // Active-resize drive + hover-cursor arbitration + hardware
                        // cursor move (shared with the injected-input path).
                        g_pointer_modifiers = msg.modifiers_;
                        process_pointer_motion();
                    }

                    // --- Mouse Button ---
                    if (msg.type_ == InputEventType::MouseButton) {
                        fbx::xkit::Offset mpos{cursor_x, cursor_y};
                        if (msg.value_b_ == 1) { // press
                            // Window-management chrome — resize edges, title bar and
                            // its close/min/max buttons — responds to the LEFT button
                            // only. Other buttons fall through to focus and are still
                            // forwarded to the client (context menu, middle-click paste).
                            const bool is_left = (msg.value_a_ == 272 /* BTN_LEFT */);
                            // Check for resize zone first (takes priority over drag/focus)
                            auto resize_hit = window_stack.resizeHitTest(mpos);
                            if (is_left && resize_hit) {
                                auto [rsid, redges] = *resize_hit;
                                focused_surface = rsid;
                                window_stack.raiseToFocus(rsid);
                                window_stack.startResize(rsid, mpos, redges);
                            } else {
                            auto title_hit = window_stack.hitTest(mpos);
                            if (is_left && title_hit) {
                                uint32_t win_id = *title_hit;
                                focused_surface = win_id;
                                window_stack.raiseToFocus(win_id);

                                // Identify which zone within the title bar was clicked
                                const auto& wins = window_stack.getWindows();
                                auto wit = std::find_if(wins.begin(), wins.end(),
                                    [win_id](const Window& w){ return w.surface_id_ == win_id; });
                                if (wit != wins.end()) {
                                    float wx = wit->position_.x_;
                                    float wy = wit->position_.y_;
                                    // Accept a click anywhere in the title-bar band
                                    // (not just the 12px dot) so the close/min/max
                                    // buttons are easy to hit with the mouse.
                                    float btn_top = wy - kTitleBarHeight;
                                    float btn_bot = wy;

                                    if (cursor_y >= btn_top && cursor_y <= btn_bot) {
                                        // Within the button row — check which button
                                        if (cursor_x >= wx + kCloseX && cursor_x <= wx + kCloseX + kButtonSize) {
                                            // Close button — notify client then remove
                                            if (g_surface_clients.contains(win_id)) {
                                                RenderMessage close_msg{
                                                    .surface_id_ = win_id,
                                                    .status_ = RenderMessageStatus::SurfaceClosed
                                                };
                                                (void)g_surface_clients[win_id]->sendMessage(
                                                    {reinterpret_cast<const uint8_t*>(&close_msg), sizeof(close_msg)});
                                            }
                                            window_stack.removeWindow(win_id);
                                            focused_surface = 999; // fall back to shell
                                        } else if (cursor_x >= wx + kMinimizeX && cursor_x <= wx + kMinimizeX + kButtonSize) {
                                            window_stack.minimizeWindow(win_id);
                                            focused_surface = 999;
                                        } else if (cursor_x >= wx + kMaximizeX && cursor_x <= wx + kMaximizeX + kButtonSize) {
                                            window_stack.maximizeWindow(win_id);
                                        } else {
                                            window_stack.startDrag(win_id, mpos);
                                        }
                                    } else {
                                        // Click in title bar but outside button row — drag
                                        window_stack.startDrag(win_id, mpos);
                                    }
                                }
                            } else {
                                // Click in window body — focus without drag
                                auto body_hit = window_stack.bodyHitTest(mpos);
                                if (body_hit) {
                                    focused_surface = *body_hit;
                                    window_stack.raiseToFocus(*body_hit);
                                }
                            }
                            } // end else (not resize_hit)
                        } else if (msg.value_a_ == 272 /* BTN_LEFT */) { // left release
                            // End resize/drag and deliver a drop only on the left
                            // release — the button these interactions start on — so a
                            // stray right/middle click mid-drag doesn't cancel or drop.
                            window_stack.endResize();
                            window_stack.endDrag();

                            // ── DnD: deliver drop ──────────────────────────────────────
                            auto& dnd = GetDnDState();
                            if (dnd.active) {
                                if (dnd.current_target != UINT32_MAX &&
                                    g_surface_clients.contains(dnd.current_target)) {
                                    const auto& wins = window_stack.getWindows();
                                    auto wit = std::find_if(wins.begin(), wins.end(),
                                        [&dnd](const Window& w){
                                            return w.surface_id_ == dnd.current_target; });
                                    float rx = cursor_x - (wit != wins.end() ? wit->position_.x_ : 0);
                                    float ry = cursor_y - (wit != wins.end() ? wit->position_.y_ : 0);
                                    DnDSendToClient(g_surface_clients[dnd.current_target],
                                        shared::DnDOpcode::Drop,
                                        rx, ry, dnd.mime_type, dnd.data);
                                    std::cout << "[DnD] Drop on surface " << dnd.current_target
                                              << " data=" << dnd.data << "\n";
                                }
                                // Notify source that drag has ended
                                DnDSendToClient(dnd.source_client,
                                    shared::DnDOpcode::DragEnd,
                                    0.0f, 0.0f, dnd.mime_type);
                                ClearDnDState();
                            }
                        }
                    }

                    // --- TouchDown: focus the surface under the finger (slot 0 only) ---
                    if (msg.type_ == InputEventType::TouchDown && msg.modifiers_ == 0) {
                        fbx::xkit::Offset tpos{
                            static_cast<float>(msg.value_a_),
                            static_cast<float>(msg.value_b_)
                        };
                        auto title_hit = window_stack.hitTest(tpos);
                        if (title_hit) {
                            focused_surface = *title_hit;
                            window_stack.raiseToFocus(*title_hit);
                        } else {
                            auto body_hit = window_stack.bodyHitTest(tpos);
                            if (body_hit) {
                                focused_surface = *body_hit;
                                window_stack.raiseToFocus(*body_hit);
                            }
                        }
                    }

                    // --- Hardware brightness keys ---
                    if (msg.type_ == InputEventType::BrightnessUp ||
                        msg.type_ == InputEventType::BrightnessDown) {
                        int delta = (msg.type_ == InputEventType::BrightnessUp)
                                    ? msg.value_a_ : -msg.value_a_;
                        BacklightService::step(delta);
                        continue;
                    }

                    // --- Audio media keys ---
                    if (msg.type_ == InputEventType::AudioVolumeUp  ||
                        msg.type_ == InputEventType::AudioVolumeDown ||
                        msg.type_ == InputEventType::AudioMute) {
                        auto& audio = AudioService::getInstance();
                        if (msg.type_ == InputEventType::AudioMute)
                            audio.toggleMute();
                        else {
                            int delta = (msg.type_ == InputEventType::AudioVolumeUp)
                                        ? msg.value_a_ : -msg.value_a_;
                            audio.adjustVolume(delta);
                        }
                        continue;
                    }

                    // --- Screenshot / screen-recording shortcuts ---
                    if (msg.type_ == InputEventType::ScreenshotCapture ||
                        msg.type_ == InputEventType::ScreenshotCaptureRegion) {
                        auto& ss = ScreenshotService::getInstance();
                        if (msg.type_ == InputEventType::ScreenshotCapture)
                            ss.captureFullScreen(static_cast<uint32_t>(msg.value_a_));
                        else
                            ss.triggerRegionSelector();
                        continue;
                    }

                    // --- Workspace switching ---
                    if (msg.type_ == InputEventType::WorkspaceSwitch) {
                        if (msg.value_a_ == -1) {
                            // Super+Left/Right: snap focused window if one is active,
                            // otherwise slide the workspace.
                            if (focused_surface != 999u) {
                                window_stack.snapWindow(focused_surface, msg.value_b_);
                            } else {
                                window_stack.slideWorkspace(msg.value_b_);
                                BroadcastWorkspaceChanged(
                                    static_cast<uint32_t>(window_stack.getActiveWorkspace()));
                            }
                        } else if (msg.value_a_ == -2) {
                            window_stack.cycleLayoutMode();
                        } else if (msg.value_a_ == -3) {
                            // Super+H/L: grow/shrink the tiling master column.
                            window_stack.adjustMasterRatio(msg.value_b_ > 0 ? 0.05f : -0.05f);
                        } else if (msg.value_a_ == -4) {
                            // Super+Shift+H/J/K/L: move focus to the nearest window
                            // in that direction (no reorder — tiles stay put).
                            using D = fbx::xserver::layout::Dir;
                            D d = msg.value_b_ == 0 ? D::Left : msg.value_b_ == 1 ? D::Right
                                : msg.value_b_ == 2 ? D::Up   : D::Down;
                            uint32_t nf = window_stack.focusDirection(focused_surface, d);
                            if (nf != focused_surface) focused_surface = nf;
                        } else if (msg.value_a_ == -5) {
                            // Super+M: promote the focused window to master.
                            window_stack.swapWithMaster(focused_surface);
                        } else {
                            window_stack.setWorkspace(static_cast<size_t>(msg.value_a_));
                        }
                        if (msg.value_a_ >= 0) {
                            BroadcastWorkspaceChanged(
                                static_cast<uint32_t>(window_stack.getActiveWorkspace()));
                        }
                    }

                    // --- Move focused window to adjacent output: Super+Shift+Left/Right ---
                    if (msg.type_ == InputEventType::MoveWindowToOutput
                            && focused_surface != 999u) {
                        const auto& layout = window_stack.outputLayout();
                        uint32_t cur_oi = window_stack.getWindowOutput(focused_surface);
                        uint32_t new_oi = cur_oi;
                        if (msg.value_b_ == -1 && cur_oi > 0)
                            new_oi = cur_oi - 1;
                        else if (msg.value_b_ == 1 && cur_oi + 1 < layout.size())
                            new_oi = cur_oi + 1;

                        if (new_oi != cur_oi) {
                            window_stack.moveWindowToOutput(focused_surface, new_oi);
                            // Notify the moved app about its new display environment
                            if (g_surface_clients.contains(focused_surface)) {
                                shared::XBusMessage disp_msg{};
                                std::strncpy(disp_msg.service_name_, "com.freebsdx.Display",
                                             sizeof(disp_msg.service_name_) - 1);
                                disp_msg.opcode_ = static_cast<uint32_t>(
                                    shared::DisplayOpcode::OutputChanged);
                                shared::DisplayWindowInfo wi{};
                                wi.output_idx_    = new_oi;
                                wi.output_width_  = static_cast<uint32_t>(layout[new_oi].width_);
                                wi.output_height_ = static_cast<uint32_t>(layout[new_oi].height_);
                                wi.x_offset_      = layout[new_oi].x_offset_;
                                std::memcpy(disp_msg.payload_, &wi, sizeof(wi));
                                (void)g_surface_clients[focused_surface]->sendMessage(
                                    {reinterpret_cast<const uint8_t*>(&disp_msg),
                                     sizeof(disp_msg)});
                            }
                            std::cout << "[wm] Window " << focused_surface
                                      << " moved to output " << new_oi
                                      << " via Super+Shift+"
                                      << (msg.value_b_ < 0 ? "Left" : "Right") << "\n";
                        }
                    }

                    // --- Alt+Tab: cycle window focus ---
                    if (msg.type_ == InputEventType::CycleFocus) {
                        const auto& wins = window_stack.getWindows();
                        std::vector<uint32_t> switchable;
                        for (const auto& w : wins) {
                            if (!w.is_shell_ && w.layer_ < 0 && !w.is_subsurface_)
                                switchable.push_back(w.surface_id_);
                        }
                        if (!switchable.empty()) {
                            int cur = -1;
                            for (int i = 0; i < (int)switchable.size(); ++i)
                                if (switchable[i] == focused_surface) { cur = i; break; }
                            int dir = msg.value_b_;
                            int next = (cur < 0)
                                ? (dir > 0 ? 0 : (int)switchable.size() - 1)
                                : (int)(((cur + dir) % (int)switchable.size()
                                         + (int)switchable.size()) % (int)switchable.size());
                            focused_surface = switchable[next];
                            window_stack.raiseToFocus(focused_surface);

                            // Build and broadcast switcher payload
                            shared::SwitcherPayload sp{};
                            sp.focused_id_ = focused_surface;
                            sp.count_ = static_cast<uint32_t>(
                                std::min(switchable.size(), size_t{7}));
                            for (uint32_t i = 0; i < sp.count_; ++i) {
                                sp.entries_[i].surface_id_ = switchable[i];
                                // Find title from window list
                                auto wit = std::find_if(wins.begin(), wins.end(),
                                    [sid = switchable[i]](const Window& w){
                                        return w.surface_id_ == sid; });
                                if (wit != wins.end()) {
                                    std::strncpy(sp.entries_[i].title_,
                                        wit->title_.empty()
                                            ? ("App " + std::to_string(switchable[i])).c_str()
                                            : wit->title_.c_str(),
                                        sizeof(sp.entries_[i].title_) - 1);
                                }
                            }
                            BroadcastSwitcherUpdate(sp);
                        }
                    }

                    // --- Alt release: dismiss switcher ---
                    if (msg.type_ == InputEventType::SwitcherDismiss) {
                        BroadcastSwitcherDismiss();
                    }

                    // --- Snap: maximize / half / quarter / restore ---
                    if (msg.type_ == InputEventType::SnapWindow && focused_surface != 999u) {
                        switch (msg.value_a_) {
                        case 0: window_stack.maximizeWindow(focused_surface);          break;
                        case 1: window_stack.snapWindow(focused_surface, -1);          break; // left half
                        case 2: window_stack.restoreWindow(focused_surface);           break;
                        case 3: window_stack.snapWindow(focused_surface, +1);          break; // right half
                        case 4: window_stack.snapWindowQuarter(focused_surface,-1,-1); break; // top-left
                        case 5: window_stack.snapWindowQuarter(focused_surface,+1,-1); break; // top-right
                        case 6: window_stack.snapWindowQuarter(focused_surface,-1,+1); break; // bottom-left
                        case 7: window_stack.snapWindowQuarter(focused_surface,+1,+1); break; // bottom-right
                        default: break;
                        }
                    }

                    // --- Dashboard / Search: forward to shell ---
                    if (msg.type_ == InputEventType::DashboardToggle ||
                        msg.type_ == InputEventType::SearchToggle) {
                        if (g_surface_clients.contains(999u)) {
                            (void)g_surface_clients[999u]->sendMessage(
                                {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
                        }
                    }

                    // --- Super+F3: toggle compositor GPU debug overlay ---
                    if (msg.type_ == InputEventType::DebugOverlayToggle) {
                        comp->toggleDebugOverlay();
                        std::cout << "[xserver] Debug overlay: "
                                  << (comp->isDebugOverlayVisible() ? "ON" : "OFF") << "\n";
                        DevOverlayService::notifyChanged();
                    }

                    // --- XF86 playback media keys → MediaPlayerService broadcast ---
                    if (msg.type_ == InputEventType::MediaPlay ||
                        msg.type_ == InputEventType::MediaStop ||
                        msg.type_ == InputEventType::MediaNext ||
                        msg.type_ == InputEventType::MediaPrev) {
                        uint32_t key = (msg.type_ == InputEventType::MediaPlay) ? 0u :
                                       (msg.type_ == InputEventType::MediaStop) ? 1u :
                                       (msg.type_ == InputEventType::MediaNext) ? 2u : 3u;
                        MediaPlayerService::broadcastMediaKey(key);
                    }

                    // --- Super+Enter: launch terminal ---
                    if (shortcuts::enabled(shortcuts::kTerminal)
                        && msg.modifiers_ & 8 && msg.type_ == InputEventType::KeyPress
                        && msg.value_b_ != 0 && msg.value_a_ == 28
                        && access("./out/terminal", X_OK) == 0) {
                        // posix_spawn, not fork+setenv: the compositor is
                        // multithreaded, and the child may only run
                        // async-signal-safe code before exec.
                        std::vector<std::string> env;
                        for (char** e = environ; e && *e; ++e) {
                            std::string_view kv(*e);
                            if (kv.starts_with("LD_LIBRARY_PATH=") || kv.starts_with("XBUS_SOCKET=")) continue;
                            env.emplace_back(kv);
                        }
                        env.push_back("LD_LIBRARY_PATH=./out");
                        env.push_back(std::string("XBUS_SOCKET=") + kSocketPath);
                        std::vector<char*> envp;
                        for (auto& kv : env) envp.push_back(kv.data());
                        envp.push_back(nullptr);
                        char arg0[] = "terminal";
                        char* argv[] = {arg0, nullptr};
                        pid_t pid = -1;
                        if (posix_spawn(&pid, "./out/terminal", nullptr, nullptr, argv, envp.data()) != 0)
                            std::cerr << "[xserver] Super+Enter: can't start the terminal\n";
                    }

                    // --- Super+Q: close the focused app window (reliable keyboard
                    // fallback for the small title-bar close button). Never closes
                    // the shell (999). Mirrors the title-bar close path. ---
                    if (shortcuts::enabled(shortcuts::kClose)
                        && msg.modifiers_ & 8 && msg.type_ == InputEventType::KeyPress
                        && msg.value_b_ != 0 && msg.value_a_ == 16 /* KEY_Q */) {
                        uint32_t fid = focused_surface;
                        if (fid != 0 && fid != 999 && g_surface_clients.contains(fid)) {
                            RenderMessage close_msg{};
                            close_msg.surface_id_ = fid;
                            close_msg.status_ = RenderMessageStatus::SurfaceClosed;
                            (void)g_surface_clients[fid]->sendMessage(
                                {reinterpret_cast<const uint8_t*>(&close_msg), sizeof(close_msg)});
                            window_stack.removeWindow(fid);
                            focused_surface = 999;
                            RebuildAndBroadcastWindowList(&window_stack);
                            g_scene_dirty = true;
                        }
                    }

                    // --- Super+L: lock the screen (spawns the lock screen via
                    // LockService, same path as the Quick Settings Lock button). ---
                    if (shortcuts::enabled(shortcuts::kLock) && msg.modifiers_ & 8 && msg.type_ == InputEventType::KeyPress
                        && msg.value_b_ != 0 && msg.value_a_ == 38 /* KEY_L */) {
                        LockService::lock();
                    }

                    // --- Forward keyboard, mouse, touch, stylus, and pinch to apps ---
                    // Suppress mouse move/button while a DnD drag is active.
                    const bool dnd_active = GetDnDState().active;

                    // Track the implicit pointer grab: a held button keeps motion
                    // on the surface that received the press.
                    if (msg.type_ == InputEventType::MouseButton)
                        g_pointer_button_down = (msg.value_b_ != 0);

                    if (!dnd_active && msg.type_ == InputEventType::MouseMove) {
                        // Pointer focus: motion follows the surface under the cursor,
                        // unless a button is held (implicit grab → keep it on the
                        // focused surface). Falls back to the shell (999) over the
                        // desktop. Coordinates are rewritten to surface-local
                        // absolute so any surface can hit-test correctly regardless
                        // of when it started receiving motion.
                        uint32_t target = g_pointer_button_down
                            ? focused_surface
                            : window_stack.bodyHitTest({cursor_x, cursor_y}).value_or(999u);
                        if (g_surface_clients.contains(target)) {
                            shared::InputMessage local = msg;
                            auto o = window_stack.windowOrigin(target);
                            local.value_a_ = static_cast<int32_t>(cursor_x - o.x_);
                            local.value_b_ = static_cast<int32_t>(cursor_y - o.y_);
                            (void)g_surface_clients[target]->sendMessage(
                                {reinterpret_cast<const uint8_t*>(&local), sizeof(local)});
                        }
                    } else if (msg.type_ == InputEventType::MouseWheel) {
                        // Scroll targets the surface under the pointer, not the
                        // keyboard-focused one (scroll never updates focus, so there
                        // is nothing to correct it otherwise).
                        uint32_t target =
                            window_stack.bodyHitTest({cursor_x, cursor_y}).value_or(999u);
                        if (g_surface_clients.contains(target)) {
                            (void)g_surface_clients[target]->sendMessage(
                                {reinterpret_cast<const uint8_t*>(&msg), sizeof(msg)});
                        }
                    } else if (msg.type_ == InputEventType::KeyPress          ||
                        msg.type_ == InputEventType::TouchDown         ||
                        msg.type_ == InputEventType::TouchMove         ||
                        msg.type_ == InputEventType::TouchUp           ||
                        msg.type_ == InputEventType::StylusDown        ||
                        msg.type_ == InputEventType::StylusMove        ||
                        msg.type_ == InputEventType::StylusUp          ||
                        msg.type_ == InputEventType::StylusProximity   ||
                        msg.type_ == InputEventType::GesturePinchBegin ||
                        msg.type_ == InputEventType::GesturePinchUpdate||
                        msg.type_ == InputEventType::GesturePinchEnd   ||
                        (!dnd_active && msg.type_ == InputEventType::MouseButton)) {
                        if (g_surface_clients.contains(focused_surface)) {
                            auto& focused_client = g_surface_clients[focused_surface];
                            // IME intercepts KeyPress events — if consumed, do not forward.
                            if (msg.type_ == InputEventType::KeyPress &&
                                ImeService::FilterKeyPress(focused_client, msg)) {
                                // consumed by IME
                            } else {
                                shared::InputMessage out = msg;
                                // Touch/stylus carry screen-absolute coordinates; rewrite
                                // them to surface-local (as MouseMove already is) so the
                                // client hit-tests correctly for off-origin windows.
                                switch (msg.type_) {
                                    case InputEventType::TouchDown:
                                    case InputEventType::TouchMove:
                                    case InputEventType::TouchUp:
                                    case InputEventType::StylusDown:
                                    case InputEventType::StylusMove:
                                    case InputEventType::StylusUp:
                                    case InputEventType::StylusProximity: {
                                        auto o = window_stack.windowOrigin(focused_surface);
                                        out.value_a_ = msg.value_a_ - static_cast<int32_t>(o.x_);
                                        out.value_b_ = msg.value_b_ - static_cast<int32_t>(o.y_);
                                        break;
                                    }
                                    default: break;
                                }
                                (void)focused_client->sendMessage(
                                    {reinterpret_cast<const uint8_t*>(&out), sizeof(out)});
                            }
                        }
                    }
                }
            }
        });
    }

    // Load MIME handler registry from app bundles (try both in-tree and build-dir paths).
    MimeService::loadFromAppsDir("./apps");
    MimeService::loadFromAppsDir("../apps");
    AppManagerService::setAppsDir(
        std::filesystem::exists("./apps") ? "./apps" : "../apps");

    // In headless mode clients (tests, CI tools) connect directly;
    // no login greeter or shell should be spawned automatically.
    if ((enable_input || virtual_boot) && !headless) {
        LoginService::setShellSpawner(spawn_shell);
        if (skip_greeter) {
            // Dev/testing: bypass login and boot straight to the desktop.
            std::cout << "[xserver] Skipping login greeter (boot straight to xshell).\n" << std::flush;
            spawn_shell();
        } else {
            LoginService::spawnGreeter();
        }
    }

    // ── Gamepad / controller support ──────────────────────────────────────
    // Scan /dev/input/event* for gamepad devices and add each fd to the
    // event loop.  Events are forwarded to the focused client as InputMessages.
    auto gamepad_mgr = std::make_unique<GamepadManager>();
    {
        int n = gamepad_mgr->scan();
        if (n > 0) std::cout << "[xserver] " << n << " gamepad(s) detected\n";
        for (int gfd : gamepad_mgr->fds()) {
            (void)loop->addFd(gfd, [&gamepad_mgr, gfd]() {
                auto gmsgs = gamepad_mgr->read(gfd);
                for (const auto& gmsg : gmsgs) {
                    // Forward to focused client (same routing as KeyPress)
                    if (focused_surface != 0 && g_surface_clients.contains(focused_surface)) {
                        (void)g_surface_clients[focused_surface]->sendMessage(
                            {reinterpret_cast<const uint8_t*>(&gmsg), sizeof(gmsg)});
                    }
                }
            });
        }
    }

    // Silence the console tty so physical keystrokes go only to the compositor
    // (via evdev), not also to the shell on this VT. Real seat only. Restored in
    // teardown. See grabConsoleKeyboard().
    if (!headless) grabConsoleKeyboard();

    std::cout << "FreeBSD-X Master is up and running.\n" << std::flush;
    loop->run();
    unlink(kSocketPath);

    // Bring the console keyboard back FIRST (before the GPU/seat teardown) so the
    // text console is usable the instant we hand it back — even if later steps are
    // slow. No-op if we never grabbed (headless / no controlling tty).
    restoreConsoleKeyboard();

    // ── Emergency-safe teardown ───────────────────────────────────────────────
    // Recover the seat FIRST and above everything else — restore the console
    // CRTC and drop DRM master — then hard-exit. We deliberately do NOT run the
    // full destructor chain: those destructors do blocking GPU (vkDeviceWaitIdle)
    // and seatd (libseat_close_device) work, plus we'd wait on child processes,
    // any of which can wedge in the kernel and hang shutdown forever, freezing
    // the seat before the CRTC is handed back. The kernel reclaims our fds, GPU
    // allocations and DRM master on _exit; seatd sees the disconnect. The only
    // thing the kernel will NOT do for us is re-point the CRTC at the console
    // framebuffer, so that one step we must do ourselves, up front.

    // Make any remaining closeDevice() calls no-ops (no blocking seatd trips).
    if (seat) seat->beginShutdown();

    // Drain a possibly-pending page flip so the restore modeset isn't -EBUSY.
    if (g_flip_pending && drm_ptr) {
        struct pollfd pfd{drm_ptr->fd(), POLLIN, 0};
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & POLLIN))
            drm_ptr->handleEvent();
    }

    releaseFbCache(drm_ptr);

    // Restore the saved console CRTC and drop DRM master. ~DrmDevice's per-device
    // closeDevice is now a no-op (beginShutdown), so this does not block.
    std::cout << "[teardown] restoring console (CRTC + drop master)...\n" << std::flush;
    drm.reset();

    // Gracefully close the seat: libseat_close_seat tells seatd we're done, and
    // seatd restores the VT to text mode. Skipping this (a bare crash or _exit)
    // leaves the VT in graphics mode, so the console never repaints after we go
    // dark — which is exactly the "console didn't come back" symptom. This is a
    // connection-level close, distinct from the per-device closeDevice round
    // trips that were blocking, so it is safe to run here.
    std::cout << "[teardown] closing seat (seatd restores the VT)...\n" << std::flush;
    seat.reset();

    // Nudge the desktop session to exit (fire-and-forget), then hard-exit so the
    // GPU (vkDeviceWaitIdle) and libinput destructors can't wedge the seat we
    // just handed back.
    std::cout << "[teardown] signalling desktop session...\n" << std::flush;
    shutdown_children();

    if (udev_ctx) udev_unref(udev_ctx);
    std::cout << "[teardown] complete — exiting.\n" << std::flush;
    _exit(0);
}
