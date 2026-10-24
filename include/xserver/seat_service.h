#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <memory>
#include <vector>
#include <string>

namespace fbx::xserver {

class SeatService {
public:
    struct Seat {
        uint32_t    id;
        std::string name;
        bool        has_keyboard{false};
        bool        has_pointer{false};
        bool        has_touch{false};
        uint32_t    output_id{0};
        uint32_t    active_surface{0};
        std::string user;
    };

    static void init() noexcept;  // populate seats from /dev/input enumeration
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called by compositor input dispatch to update active surface per seat
    static void setActiveSurface(uint32_t seat_id, uint32_t surface_id) noexcept;

    // Returns seat_id for a given input device path (0 = default seat)
    static uint32_t seatForDevice(const std::string& dev_path) noexcept;

    static const std::vector<Seat>& seats() noexcept { return seats_; }

private:
    static shared::SeatPage toPage(const Seat& s, uint32_t page, uint32_t total) noexcept;
    static std::vector<Seat> seats_;
};

} // namespace fbx::xserver
