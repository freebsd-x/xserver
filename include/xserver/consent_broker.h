#pragma once

#include "shared/protocol.h"
#include "xserver/ipc_socket.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// First-use permission consent. When a sandboxed app asks for a capability
// (camera, microphone, screen capture) the user has never decided on, the
// dispatcher parks the request here instead of refusing it, and the shell is
// asked (AppSandbox ConsentRequested). The answer arrives as SetPermission;
// resolve() then replays every request that was waiting on it, so the app
// simply sees its call succeed or fail as if it had waited.
namespace fbx::xserver::consent {

using Replay = std::function<void(std::shared_ptr<IpcSocket>, const shared::XBusMessage&)>;

// How a held request is re-dispatched (set once by the dispatcher).
void setReplay(Replay replay) noexcept;

// Park `msg` from `client` until the user decides `cap` for `app`. The shell
// is asked once per (app, cap) while a question is open; at most 16 requests
// are kept per question (more are dropped — the app is re-asking anyway).
void hold(const std::string& app, const std::string& display_name, uint32_t cap,
          std::shared_ptr<IpcSocket> client, const shared::XBusMessage& msg) noexcept;

// The user decided `cap` for `app`: replay its held requests (in order).
void resolve(const std::string& app, uint32_t cap) noexcept;

// Requests waiting on (app, cap) — for tests.
[[nodiscard]] size_t pending(const std::string& app, uint32_t cap) noexcept;

}  // namespace fbx::xserver::consent
