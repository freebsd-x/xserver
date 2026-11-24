#pragma once

#include "shared/protocol.h"
#include "xserver/idle_policy.h"
#include "xserver/ipc_socket.h"
#include <functional>
#include <memory>

namespace fbx::xserver {

// com.freebsdx.Idle — what happens when nobody touches the machine: dim, lock, screen
// off (idle_policy.h), and apps holding that off while a video plays. Event loop only.
class IdleService {
public:
    struct Hooks {
        std::function<void(idle::Phase)> set_phase;   // make the screen so
        std::function<bool()> held;                   // keep-awake, presentation mode
        std::function<bool()> at_greeter;
    };
    static void init(Hooks hooks) noexcept;

    static void handleMessage(const shared::XBusMessage& msg, std::shared_ptr<IpcSocket> client) noexcept;

    // Any input. True if it only woke a dark screen: it goes no further (a key pressed
    // to wake it is no part of a password, nor for whatever has the focus).
    [[nodiscard]] static bool noteInput() noexcept;

    static void tick() noexcept;   // once a second
    static void onClientDisconnect(const std::shared_ptr<IpcSocket>& client) noexcept;

    [[nodiscard]] static idle::Phase phase() noexcept;
};

}  // namespace fbx::xserver
