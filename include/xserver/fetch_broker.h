#pragma once
#include "shared/protocol.h"
#include "xserver/unique_fd.h"
#include <chrono>
#include <memory>
#include <string>

namespace fbx::xserver {
class IpcSocket;

// com.freebsdx.Fetch: relays an app's allowed fetch to the xfetch daemon and
// its reply (with the body fd) back to the app. Policy: fetch_policy.h.
class FetchBroker {
public:
    // Every com.freebsdx.Fetch message, from apps and from xfetch itself.
    static void ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg, UniqueFd& fd) noexcept;

    // For the compositor's own services (weather, package catalog/downloads,
    // ad-block list): fetch `url` through xfetch under the compositor's
    // policy rules and wait for the body. Worker threads only — the reply is
    // handled on the main loop, so calling it there would wait on itself (it
    // refuses, returning Unavailable).
    struct Result {
        shared::FetchStatus status = shared::FetchStatus::Unavailable;
        uint32_t            http_status = 0;
        std::string         body;
        [[nodiscard]] bool ok() const noexcept { return status == shared::FetchStatus::Ok; }
    };
    static Result fetchBlocking(const std::string& url,
                                std::chrono::seconds timeout = std::chrono::seconds(60)) noexcept;
    // fetchBlocking, then write the body to `path` (atomically: a temp file
    // renamed into place). False if the fetch or the write failed.
    static bool fetchToFile(const std::string& url, const std::string& path,
                            std::chrono::seconds timeout = std::chrono::seconds(60)) noexcept;
};

}  // namespace fbx::xserver
