#include "xserver/seat_service.h"
#include "xserver/ipc_socket.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
// evdev ioctl defines
#ifdef __linux__
#  include <linux/input.h>
#else
#  include <dev/evdev/input.h>
#  include <dev/evdev/input-event-codes.h>
#endif

namespace fbx::xserver { void BroadcastXBusMessage(const shared::XBusMessage& msg); }

namespace fbx::xserver {

// ── Static state ─────────────────────────────────────────────────────────────

std::vector<SeatService::Seat> SeatService::seats_;

// ── Helpers ───────────────────────────────────────────────────────────────────

shared::SeatPage SeatService::toPage(const Seat& s, uint32_t page, uint32_t total) noexcept {
    shared::SeatPage sp{};
    sp.page_           = page;
    sp.total_          = total;
    sp.valid_          = 1;
    sp.seat_id_        = s.id;
    sp.has_keyboard_   = s.has_keyboard ? 1u : 0u;
    sp.has_pointer_    = s.has_pointer  ? 1u : 0u;
    sp.has_touch_      = s.has_touch    ? 1u : 0u;
    sp.output_id_      = s.output_id;
    sp.active_surface_ = s.active_surface;
    std::strncpy(sp.name_, s.name.c_str(), sizeof(sp.name_) - 1);
    std::strncpy(sp.user_, s.user.c_str(), sizeof(sp.user_) - 1);
    return sp;
}

// ── init ──────────────────────────────────────────────────────────────────────

void SeatService::init() noexcept {
    seats_.clear();

    Seat seat0{};
    seat0.id   = 0;
    seat0.name = "seat0";

    // Determine logged-in user
    const char* user = std::getenv("USER");
    if (!user || user[0] == '\0') user = getlogin();
    if (user) seat0.user = user;

    // Scan /dev/input for event devices
    DIR* dir = opendir("/dev/input");
    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (std::strncmp(ent->d_name, "event", 5) != 0) continue;

            std::string path = std::string("/dev/input/") + ent->d_name;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) continue;

            uint8_t key_bits[KEY_MAX / 8 + 1] = {};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
                auto test_bit = [&](int bit) -> bool {
                    return (key_bits[bit / 8] >> (bit % 8)) & 1;
                };
                if (test_bit(BTN_MISC) || test_bit(KEY_A))
                    seat0.has_keyboard = true;
                if (test_bit(BTN_MOUSE))
                    seat0.has_pointer = true;
                if (test_bit(BTN_TOUCH))
                    seat0.has_touch = true;
            }
            close(fd);
        }
        closedir(dir);
    } else {
        // /dev/input not available — assume basic seat
        seat0.has_keyboard = true;
        seat0.has_pointer  = true;
    }

    seats_.push_back(std::move(seat0));

    std::cout << "[SeatService] Initialized " << seats_.size()
              << " seat(s); seat0: kb=" << seats_[0].has_keyboard
              << " ptr=" << seats_[0].has_pointer
              << " touch=" << seats_[0].has_touch << "\n";
}

// ── setActiveSurface ─────────────────────────────────────────────────────────

void SeatService::setActiveSurface(uint32_t seat_id, uint32_t surface_id) noexcept {
    for (auto& s : seats_) {
        if (s.id == seat_id) {
            s.active_surface = surface_id;
            return;
        }
    }
}

// ── seatForDevice ────────────────────────────────────────────────────────────

uint32_t SeatService::seatForDevice(const std::string& /*dev_path*/) noexcept {
    return 0; // all devices belong to seat0 in this implementation
}

// ── ProcessMessage ────────────────────────────────────────────────────────────

bool SeatService::ProcessMessage(std::shared_ptr<IpcSocket> client,
                                 const shared::XBusMessage& msg) noexcept
{
    using namespace shared;
    XBusMessage resp = msg;

    switch (static_cast<SeatOpcode>(msg.opcode_)) {

    case SeatOpcode::ListSeats: {
        uint32_t page = 0;
        std::memcpy(&page, msg.payload_, sizeof(page));
        uint32_t total = static_cast<uint32_t>(seats_.size());
        if (page < total) {
            SeatPage sp = toPage(seats_[page], page, total);
            resp.opcode_ = static_cast<uint32_t>(SeatOpcode::SeatPage);
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &sp, sizeof(sp));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        } else {
            // Empty response — signal end of list
            SeatPage sp{};
            sp.page_  = page;
            sp.total_ = total;
            sp.valid_ = 0;
            resp.opcode_ = static_cast<uint32_t>(SeatOpcode::SeatPage);
            std::memset(resp.payload_, 0, sizeof(resp.payload_));
            std::memcpy(resp.payload_, &sp, sizeof(sp));
            (void)client->sendMessage(
                {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        }
        return true;
    }

    case SeatOpcode::GetSeatInfo: {
        SeatIdRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        uint32_t total = static_cast<uint32_t>(seats_.size());
        for (uint32_t i = 0; i < total; ++i) {
            if (seats_[i].id == req.seat_id_) {
                SeatPage sp = toPage(seats_[i], i, total);
                resp.opcode_ = static_cast<uint32_t>(SeatOpcode::SeatPage);
                std::memset(resp.payload_, 0, sizeof(resp.payload_));
                std::memcpy(resp.payload_, &sp, sizeof(sp));
                (void)client->sendMessage(
                    {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
                return true;
            }
        }
        // Not found — send empty
        SeatPage sp{};
        sp.valid_ = 0;
        resp.opcode_ = static_cast<uint32_t>(SeatOpcode::SeatPage);
        std::memset(resp.payload_, 0, sizeof(resp.payload_));
        std::memcpy(resp.payload_, &sp, sizeof(sp));
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    case SeatOpcode::AssignOutput: {
        if (client->isSandboxed()) {
            std::cerr << "[SeatService] DENIED: sandboxed app attempted AssignOutput\n";
            return true;
        }
        SeatAssignRequest req{};
        std::memcpy(&req, msg.payload_, sizeof(req));
        for (auto& s : seats_) {
            if (s.id == req.seat_id_) {
                s.output_id = req.output_id_;
                std::cout << "[SeatService] Assigned seat " << s.id
                          << " to output " << req.output_id_ << "\n";
                break;
            }
        }
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;
    }

    case SeatOpcode::SeatChanged:
        // Just ack
        (void)client->sendMessage(
            {reinterpret_cast<const uint8_t*>(&resp), sizeof(resp)});
        return true;

    default:
        std::cerr << "[SeatService] Unknown opcode: 0x"
                  << std::hex << msg.opcode_ << "\n";
        return false;
    }
}

} // namespace fbx::xserver
