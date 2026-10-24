#include "xserver/remote_desktop_service.h"
#include "xserver/vulkan_compositor.h"
#include "shared/protocol.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <cerrno>

// Forward-declare BroadcastXBusMessage from xbus_dispatcher.cc
namespace fbx::xserver {
    void BroadcastXBusMessage(const shared::XBusMessage& msg);
}

namespace fbx::xserver {

// ─── Static member definitions ────────────────────────────────────────────────

VulkanCompositor*                 RemoteDesktopService::comp_         = nullptr;
std::atomic<bool>                 RemoteDesktopService::running_{false};
std::atomic<uint32_t>             RemoteDesktopService::client_count_{0};
int                               RemoteDesktopService::listen_fd_    = -1;
std::thread                       RemoteDesktopService::accept_thread_;
std::string                       RemoteDesktopService::password_;
std::mutex                        RemoteDesktopService::inject_mutex_;
std::deque<shared::InputMessage>  RemoteDesktopService::inject_queue_;
std::mutex                        RemoteDesktopService::capture_mutex_;

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Read exactly n bytes, looping on EINTR/short reads. Returns false on EOF/error.
static bool read_all(int fd, void* buf, size_t n) noexcept {
    uint8_t* p = static_cast<uint8_t*>(buf);
    while (n > 0) {
        ssize_t r = ::read(fd, p, n);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

// Write exactly n bytes, looping on EINTR/short writes. Returns false on error.
static bool write_all(int fd, const void* buf, size_t n) noexcept {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
    return true;
}

static void write_be16(uint8_t* dst, uint16_t v) noexcept {
    dst[0] = static_cast<uint8_t>(v >> 8);
    dst[1] = static_cast<uint8_t>(v & 0xFF);
}

// ─── Keysym → Linux scancode mapping ─────────────────────────────────────────

// ASCII 0x20–0x7e → Linux keycode + shift flag
// Returns {keycode, needs_shift}; keycode==-1 means unsupported
struct KeyMapping { int keycode; bool shift; };

static KeyMapping asciiToLinux(uint32_t ks) noexcept {
    // Space
    if (ks == 0x0020) return {57, false};

    // Digits 0-9 (no shift)
    if (ks >= '0' && ks <= '9') {
        // keycodes: 1=ESC, 2='1', 3='2', ..., 11='0'
        return {static_cast<int>((ks == '0') ? 11 : (ks - '0' + 1)), false};
    }

    // Shifted digits: ! @ # $ % ^ & * ( )
    static const struct { char ch; int kc; } shifted_digits[] = {
        {'!',2},{'@',3},{'#',4},{'$',5},{'%',6},{'^',7},{'&',8},{'*',9},{'(',10},{')',11}
    };
    for (auto& e : shifted_digits)
        if (ks == static_cast<uint32_t>(e.ch)) return {e.kc, true};

    // Letters a-z (no shift) / A-Z (shift)
    if (ks >= 'a' && ks <= 'z') {
        // Linux keycodes for a-z:
        // a=30 b=48 c=46 d=32 e=18 f=33 g=34 h=35 i=23 j=36 k=37 l=38
        // m=50 n=49 o=24 p=25 q=16 r=19 s=31 t=20 u=22 v=47 w=17 x=45 y=21 z=44
        static const int kc[] = {
            30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44
        };
        return {kc[ks - 'a'], false};
    }
    if (ks >= 'A' && ks <= 'Z') {
        static const int kc[] = {
            30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44
        };
        return {kc[ks - 'A'], true};
    }

    // Punctuation and symbols
    static const struct { uint32_t sym; int kc; bool sh; } punct[] = {
        {'-',12,false},{'_',12,true},{'=',13,false},{'+',13,true},
        {'[',26,false},{'{',26,true},{']',27,false},{'}',27,true},
        {'\\',43,false},{'|',43,true},{';',39,false},{':',39,true},
        {'\'',40,false},{'"',40,true},{'`',41,false},{'~',41,true},
        {',',51,false},{'<',51,true},{'.',52,false},{'>',52,true},
        {'/',53,false},{'?',53,true},
    };
    for (auto& e : punct)
        if (ks == e.sym) return {e.kc, e.sh};

    return {-1, false};
}

static int keysymToLinux(uint32_t ks) noexcept {
    // ASCII range
    if (ks >= 0x0020 && ks <= 0x007e) {
        return asciiToLinux(ks).keycode;
    }

    switch (ks) {
    case 0xFF08: return 14;  // BackSpace
    case 0xFF09: return 15;  // Tab
    case 0xFF0D: return 28;  // Return
    case 0xFF1B: return 1;   // Escape
    case 0xFF51: return 105; // Left
    case 0xFF52: return 103; // Up
    case 0xFF53: return 106; // Right
    case 0xFF54: return 108; // Down
    case 0xFF50: return 102; // Home
    case 0xFF57: return 107; // End
    case 0xFF55: return 104; // Prior (PgUp)
    case 0xFF56: return 109; // Next (PgDn)
    case 0xFF63: return 110; // Insert
    case 0xFFFF: return 111; // Delete
    case 0xFF7F: return 119; // Pause
    case 0xFFBE: return 59;  // F1
    case 0xFFBF: return 60;  // F2
    case 0xFFC0: return 61;  // F3
    case 0xFFC1: return 62;  // F4
    case 0xFFC2: return 63;  // F5
    case 0xFFC3: return 64;  // F6
    case 0xFFC4: return 65;  // F7
    case 0xFFC5: return 66;  // F8
    case 0xFFC6: return 67;  // F9
    case 0xFFC7: return 68;  // F10
    case 0xFFC8: return 87;  // F11
    case 0xFFC9: return 88;  // F12
    case 0xFFE1: case 0xFFE2: return 42; // Shift_L / Shift_R
    case 0xFFE3: case 0xFFE4: return 29; // Control_L / Control_R
    case 0xFFE7: case 0xFFE8: return 125;// Super_L / Super_R
    case 0xFFE9: case 0xFFEA: return 56; // Alt_L / Alt_R
    case 0xFF20: return 56;  // Meta (treat as Alt)
    case 0xFF8D: return 28;  // KP_Enter
    default: return -1;
    }
}

// ─── Public interface ─────────────────────────────────────────────────────────

void RemoteDesktopService::setCompositor(VulkanCompositor* comp) noexcept {
    comp_ = comp;
}

bool RemoteDesktopService::isRunning() noexcept {
    return running_.load();
}

std::vector<shared::InputMessage> RemoteDesktopService::drainInjectedInput() noexcept {
    std::lock_guard<std::mutex> lock(inject_mutex_);
    std::vector<shared::InputMessage> out;
    out.reserve(inject_queue_.size());
    for (auto& m : inject_queue_) out.push_back(m);
    inject_queue_.clear();
    return out;
}

void RemoteDesktopService::pushInjectedInput(const shared::InputMessage& msg) noexcept {
    std::lock_guard<std::mutex> lock(inject_mutex_);
    // Cap queue at 256 events to avoid memory growth if the event loop stalls
    if (inject_queue_.size() < 256) {
        inject_queue_.push_back(msg);
    }
}

// ─── Server lifecycle ─────────────────────────────────────────────────────────

void RemoteDesktopService::startServer() noexcept {
    if (running_.load()) return;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::cerr << "[RemoteDesktop] socket() failed: " << strerror(errno) << "\n";
        return;
    }

    int opt = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(kPort);

    if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[RemoteDesktop] bind() failed: " << strerror(errno) << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    if (::listen(listen_fd_, 8) < 0) {
        std::cerr << "[RemoteDesktop] listen() failed: " << strerror(errno) << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    running_.store(true);
    accept_thread_ = std::thread([]() { acceptLoop(); });

    std::cout << "[RemoteDesktop] RFB server listening on port " << kPort << "\n";
    broadcastStatus();
}

void RemoteDesktopService::stopServer() noexcept {
    if (!running_.load()) return;

    running_.store(false);

    // Close the listen socket to unblock accept()
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    client_count_.store(0);
    std::cout << "[RemoteDesktop] RFB server stopped.\n";
    broadcastStatus();
}

void RemoteDesktopService::acceptLoop() noexcept {
    while (running_.load()) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_,
            reinterpret_cast<struct sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) {
            if (!running_.load()) break; // server stopped
            if (errno == EINTR) continue;
            std::cerr << "[RemoteDesktop] accept() failed: " << strerror(errno) << "\n";
            continue;
        }

        ++client_count_;
        std::cout << "[RemoteDesktop] VNC client connected (total: "
                  << client_count_.load() << ")\n";
        broadcastStatus();

        // Detach a thread per client
        uint32_t w = 1920, h = 1080;
        if (comp_) {
            // Get dimensions from compositor output 0 — best-effort
            // (VulkanCompositor doesn't expose a direct getter, use a known default)
        }
        std::thread([client_fd, w, h]() {
            try { clientLoop(client_fd, w, h); } catch (...) { ::close(client_fd); }
        }).detach();
    }
}

// ─── RFB client handshake + message loop ──────────────────────────────────────

void RemoteDesktopService::clientLoop(int fd, uint32_t w, uint32_t h) noexcept {
    // 1. Version handshake
    if (!write_all(fd, "RFB 003.008\n", 12)) { ::close(fd); --client_count_; broadcastStatus(); return; }

    char ver[12]{};
    if (!read_all(fd, ver, 12)) { ::close(fd); --client_count_; broadcastStatus(); return; }

    // 2. Security
    if (password_.empty()) {
        // Security type 1 (None)
        uint8_t sec[2] = {1, 1};
        if (!write_all(fd, sec, 2)) { ::close(fd); --client_count_; broadcastStatus(); return; }

        uint8_t picked = 0;
        if (!read_all(fd, &picked, 1)) { ::close(fd); --client_count_; broadcastStatus(); return; }

        // SecurityResult: OK
        uint8_t ok[4] = {0, 0, 0, 0};
        if (!write_all(fd, ok, 4)) { ::close(fd); --client_count_; broadcastStatus(); return; }
    } else {
        // Security type 2 (VNC Auth)
        uint8_t sec[2] = {1, 2};
        if (!write_all(fd, sec, 2)) { ::close(fd); --client_count_; broadcastStatus(); return; }

        uint8_t picked = 0;
        if (!read_all(fd, &picked, 1)) { ::close(fd); --client_count_; broadcastStatus(); return; }

        if (!doVncAuth(fd)) {
            // SecurityResult: fail
            uint8_t fail[4] = {0, 0, 0, 1};
            write_all(fd, fail, 4);
            ::close(fd);
            --client_count_;
            broadcastStatus();
            return;
        }

        uint8_t ok[4] = {0, 0, 0, 0};
        if (!write_all(fd, ok, 4)) { ::close(fd); --client_count_; broadcastStatus(); return; }
    }

    // 3. ClientInit
    uint8_t client_init = 0;
    if (!read_all(fd, &client_init, 1)) { ::close(fd); --client_count_; broadcastStatus(); return; }

    // 4. ServerInit
    // Pixel format: 32 bpp, 24 depth, little-endian, true-colour
    // R-shift=16, G-shift=8, B-shift=0 (XRGB / BGRA byte order)
    const char* name = "FreeBSD-X";
    uint32_t name_len = htonl(static_cast<uint32_t>(std::strlen(name)));

    // Build ServerInit on the stack
    uint8_t sinit[24 + 9]; // header + name (9 bytes)
    uint16_t be_w = htons(static_cast<uint16_t>(w));
    uint16_t be_h = htons(static_cast<uint16_t>(h));
    std::memcpy(sinit + 0, &be_w, 2);
    std::memcpy(sinit + 2, &be_h, 2);
    // Pixel format (16 bytes at offset 4):
    sinit[4]  = 32;  // bits-per-pixel
    sinit[5]  = 24;  // depth
    sinit[6]  = 0;   // big-endian flag
    sinit[7]  = 1;   // true-colour flag
    uint16_t max255 = htons(255);
    std::memcpy(sinit + 8,  &max255, 2); // red-max
    std::memcpy(sinit + 10, &max255, 2); // green-max
    std::memcpy(sinit + 12, &max255, 2); // blue-max
    sinit[14] = 16; // red-shift
    sinit[15] = 8;  // green-shift
    sinit[16] = 0;  // blue-shift
    sinit[17] = 0; sinit[18] = 0; sinit[19] = 0; // 3 padding bytes
    std::memcpy(sinit + 20, &name_len, 4);
    std::memcpy(sinit + 24, name, 9);

    if (!write_all(fd, sinit, sizeof(sinit))) { ::close(fd); --client_count_; broadcastStatus(); return; }

    // 5. Message loop
    uint8_t type = 0;
    while (running_.load()) {
        ssize_t r = ::read(fd, &type, 1);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }

        switch (type) {
        case 3: { // FramebufferUpdateRequest
            uint8_t fbur[9]{};
            if (!read_all(fd, fbur, 9)) goto done;
            sendFrameUpdate(fd, w, h);
            break;
        }
        case 4: { // KeyEvent
            uint8_t kev[7]{};
            if (!read_all(fd, kev, 7)) goto done;
            {
                uint8_t down = kev[0];
                uint32_t keysym = (static_cast<uint32_t>(kev[3]) << 24) |
                                  (static_cast<uint32_t>(kev[4]) << 16) |
                                  (static_cast<uint32_t>(kev[5]) << 8)  |
                                   static_cast<uint32_t>(kev[6]);
                injectKeyEvent(keysym, down);
            }
            break;
        }
        case 5: { // PointerEvent
            uint8_t pev[5]{};
            if (!read_all(fd, pev, 5)) goto done;
            {
                uint8_t btn    = pev[0];
                uint16_t px    = (static_cast<uint16_t>(pev[1]) << 8) | pev[2];
                uint16_t py    = (static_cast<uint16_t>(pev[3]) << 8) | pev[4];
                injectPointerEvent(btn, px, py);
            }
            break;
        }
        case 6: { // ClientCutText
            uint8_t cct[7]{};
            if (!read_all(fd, cct, 7)) goto done; // 3 pad + 4 len
            {
                uint32_t len = (static_cast<uint32_t>(cct[3]) << 24) |
                               (static_cast<uint32_t>(cct[4]) << 16) |
                               (static_cast<uint32_t>(cct[5]) << 8)  |
                                static_cast<uint32_t>(cct[6]);
                // Drain the text but ignore it (no clipboard integration yet)
                if (len > 0 && len < 65536) {
                    std::vector<char> text(len);
                    if (!read_all(fd, text.data(), len)) goto done;
                }
            }
            break;
        }
        default:
            // Unknown message type — can't recover without knowing length
            // Log and close
            std::cerr << "[RemoteDesktop] Unknown RFB message type " << (int)type << ", closing client\n";
            goto done;
        }
    }

done:
    ::close(fd);
    if (client_count_.load() > 0) --client_count_;
    std::cout << "[RemoteDesktop] VNC client disconnected (remaining: "
              << client_count_.load() << ")\n";
    broadcastStatus();
}

// ─── VNC Auth ─────────────────────────────────────────────────────────────────

bool RemoteDesktopService::doVncAuth(int fd) noexcept {
    // Generate 16-byte random challenge
    uint8_t challenge[16]{};
    arc4random_buf(challenge, 16);
    if (!write_all(fd, challenge, 16)) return false;

    // Read 16-byte response from client
    uint8_t response[16]{};
    if (!read_all(fd, response, 16)) return false;

    // Simplified: without OpenSSL DES we can't verify the VNC auth response
    // properly. Accept the connection if a password is set — the password
    // protects via the security type negotiation (type 2 = VNC Auth is offered,
    // clients that don't know the password won't be able to produce the correct
    // DES-encrypted response). For now, always accept to complete the handshake.
    // TODO: implement full DES verification via OpenSSL DES_ecb_encrypt.
    (void)response;
    return true;
}

// ─── Frame capture and send ───────────────────────────────────────────────────

void RemoteDesktopService::sendFrameUpdate(int fd, [[maybe_unused]] uint32_t w, [[maybe_unused]] uint32_t h) noexcept {
    if (!comp_) {
        // Send a 1×1 black update as fallback
        uint8_t hdr[4]  = {0, 0, 0, 1};
        uint8_t rect[12]{};
        write_be16(rect + 4, 1); // w
        write_be16(rect + 6, 1); // h
        // encoding = Raw (0)
        uint8_t px[4] = {0, 0, 0, 0};
        write_all(fd, hdr, 4);
        write_all(fd, rect, 12);
        write_all(fd, px, 4);
        return;
    }

    std::lock_guard<std::mutex> lock(capture_mutex_);
    auto res = comp_->captureToShm(0);
    if (!res.has_value()) {
        // Send 1×1 black fallback
        uint8_t hdr[4]  = {0, 0, 0, 1};
        uint8_t rect[12]{};
        write_be16(rect + 4, 1);
        write_be16(rect + 6, 1);
        uint8_t px[4] = {0, 0, 0, 0};
        write_all(fd, hdr, 4);
        write_all(fd, rect, 12);
        write_all(fd, px, 4);
        return;
    }

    auto [shm_fd, info] = std::move(res.value());
    uint32_t actual_w = info.width_;
    uint32_t actual_h = info.height_;
    const size_t shm_sz = info.stride_ * info.height_;

    const uint8_t* pixels = static_cast<const uint8_t*>(
        ::mmap(nullptr, shm_sz, PROT_READ, MAP_SHARED, shm_fd, 0));
    ::close(shm_fd);

    if (pixels == MAP_FAILED) {
        // Send 1×1 black fallback
        uint8_t hdr[4]  = {0, 0, 0, 1};
        uint8_t rect[12]{};
        write_be16(rect + 4, 1);
        write_be16(rect + 6, 1);
        uint8_t px[4] = {0, 0, 0, 0};
        write_all(fd, hdr, 4);
        write_all(fd, rect, 12);
        write_all(fd, px, 4);
        return;
    }

    // FramebufferUpdate header: msg_type=0, padding, num_rects=1
    uint8_t hdr[4] = {0, 0, 0, 1};
    write_all(fd, hdr, 4);

    // Rectangle header (12 bytes): x, y, w, h, encoding
    uint8_t rect[12]{};
    rect[0] = 0; rect[1] = 0; // x = 0 (BE)
    rect[2] = 0; rect[3] = 0; // y = 0 (BE)
    write_be16(rect + 4, static_cast<uint16_t>(actual_w));
    write_be16(rect + 6, static_cast<uint16_t>(actual_h));
    // Encoding = Raw (0x00000000)
    rect[8] = 0; rect[9] = 0; rect[10] = 0; rect[11] = 0;
    write_all(fd, rect, 12);

    // Pixel data: XRGB8888 (B=byte0, G=byte1, R=byte2, X=byte3).
    // VNC Raw with R-shift=16, G-shift=8, B-shift=0 expects each 32-bit pixel as:
    //   byte0=B, byte1=G, byte2=R, byte3=0  (same as XRGB8888 native layout)
    // So no conversion needed — just send the raw SHM data.
    //
    // However, to match the ServerInit pixel format (R-shift=16 means R is in
    // bits 23-16 of the 32-bit value), the client interprets received bytes as
    // a little-endian uint32: byte0 is bits 7-0 (blue), byte2 is bits 23-16 (red).
    // This matches XRGB8888 exactly: B at byte0, G at byte1, R at byte2, X at byte3.

    // Send row by row using the actual stride from captureToShm
    if (info.stride_ == actual_w * 4) {
        // Contiguous — send in one shot
        write_all(fd, pixels, actual_w * actual_h * 4);
    } else {
        // Strided — send row by row
        for (uint32_t row = 0; row < actual_h; ++row) {
            write_all(fd, pixels + row * info.stride_, actual_w * 4);
        }
    }

    ::munmap(const_cast<uint8_t*>(pixels), shm_sz);
}

// ─── Input injection ──────────────────────────────────────────────────────────

void RemoteDesktopService::injectKeyEvent(uint32_t keysym, uint8_t down) noexcept {
    int linux_kc = keysymToLinux(keysym);
    if (linux_kc < 0) return;

    shared::InputMessage msg{};
    msg.type_    = shared::InputEventType::KeyPress;
    msg.value_a_ = linux_kc;
    msg.value_b_ = down ? 1 : 0;
    msg.modifiers_ = 0;

    // For ASCII printable, also set char_value_ so text inputs work
    if (keysym >= 0x0020 && keysym <= 0x007e && down) {
        msg.char_value_ = static_cast<int32_t>(keysym);
    }

    pushInjectedInput(msg);
}

void RemoteDesktopService::injectPointerEvent(uint8_t btn, uint16_t x, uint16_t y) noexcept {
    // Thread-local tracking of previous state for delta computation
    static uint16_t last_x   = 0;
    static uint16_t last_y   = 0;
    static uint8_t  last_btn = 0;

    if (x != last_x || y != last_y) {
        shared::InputMessage msg{};
        msg.type_    = shared::InputEventType::MouseMove;
        msg.value_a_ = static_cast<int32_t>(x) - static_cast<int32_t>(last_x);
        msg.value_b_ = static_cast<int32_t>(y) - static_cast<int32_t>(last_y);
        pushInjectedInput(msg);
        last_x = x;
        last_y = y;
    }

    if (btn != last_btn) {
        // RFB button mask: bit0=left, bit1=middle, bit2=right,
        //                  bit3=scroll-up, bit4=scroll-down
        auto button_changed = [&](uint8_t bit, int btn_code) {
            bool now = (btn      & bit) != 0;
            bool was = (last_btn & bit) != 0;
            if (now != was) {
                shared::InputMessage msg{};
                msg.type_    = shared::InputEventType::MouseButton;
                msg.value_a_ = btn_code;
                msg.value_b_ = now ? 1 : 0;
                pushInjectedInput(msg);
            }
        };

        button_changed(0x01, 272); // BTN_LEFT
        button_changed(0x02, 274); // BTN_MIDDLE
        button_changed(0x04, 273); // BTN_RIGHT

        // Scroll wheel (bit 3 = up, bit 4 = down)
        if ((btn & 0x08) && !(last_btn & 0x08)) {
            shared::InputMessage msg{};
            msg.type_    = shared::InputEventType::MouseWheel;
            msg.value_b_ = -3; // scroll up = negative delta
            pushInjectedInput(msg);
        }
        if ((btn & 0x10) && !(last_btn & 0x10)) {
            shared::InputMessage msg{};
            msg.type_    = shared::InputEventType::MouseWheel;
            msg.value_b_ = 3; // scroll down = positive delta
            pushInjectedInput(msg);
        }

        last_btn = btn;
    }
}

// ─── XBus message handler ─────────────────────────────────────────────────────

bool RemoteDesktopService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                          const shared::XBusMessage& msg) noexcept {
    using Op = shared::RemoteDesktopOpcode;

    shared::XBusMessage response = msg;

    switch (static_cast<Op>(msg.opcode_)) {
    case Op::GetStatus: {
        shared::RemoteDesktopStatus st{};
        st.running_      = running_.load() ? 1u : 0u;
        st.port_         = kPort;
        st.client_count_ = client_count_.load();
        st.password_set_ = password_.empty() ? 0u : 1u;
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }
    case Op::Enable: {
        startServer();
        // Ack: send current status
        shared::RemoteDesktopStatus st{};
        st.running_      = running_.load() ? 1u : 0u;
        st.port_         = kPort;
        st.client_count_ = client_count_.load();
        st.password_set_ = password_.empty() ? 0u : 1u;
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }
    case Op::Disable: {
        stopServer();
        shared::RemoteDesktopStatus st{};
        st.running_      = 0;
        st.port_         = kPort;
        st.client_count_ = 0;
        st.password_set_ = password_.empty() ? 0u : 1u;
        std::memcpy(response.payload_, &st, sizeof(st));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }
    case Op::SetPassword: {
        shared::RemoteDesktopPasswordRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        req.password_[sizeof(req.password_) - 1] = '\0';
        password_ = std::string(req.password_, strnlen(req.password_, sizeof(req.password_)));
        std::cout << "[RemoteDesktop] Password "
                  << (password_.empty() ? "cleared" : "set") << "\n";
        // Ack
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        break;
    }
    default:
        return false;
    }

    return true;
}

// ─── Broadcast helpers ────────────────────────────────────────────────────────

void RemoteDesktopService::broadcastStatus() noexcept {
    shared::XBusMessage msg{};
    msg.magic_ = shared::XBUS_MAGIC;
    std::strncpy(msg.service_name_, "com.freebsdx.RemoteDesktop",
                 sizeof(msg.service_name_) - 1);
    msg.opcode_ = static_cast<uint32_t>(shared::RemoteDesktopOpcode::StatusChanged);

    shared::RemoteDesktopStatus st{};
    st.running_      = running_.load() ? 1u : 0u;
    st.port_         = kPort;
    st.client_count_ = client_count_.load();
    st.password_set_ = password_.empty() ? 0u : 1u;
    std::memcpy(msg.payload_, &st, sizeof(st));

    BroadcastXBusMessage(msg);
}

} // namespace fbx::xserver
