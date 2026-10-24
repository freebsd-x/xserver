#pragma once

#include "xserver/unique_fd.h"
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <functional>
#include <unordered_map>
#include <memory>
#include <expected>
#include <system_error>

namespace fbx::xserver {

class EventLoop {
public:
    using Callback = std::function<void()>;

    static std::expected<std::unique_ptr<EventLoop>, std::error_code> create() noexcept;

    EventLoop() noexcept;
    ~EventLoop() noexcept;

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;
    EventLoop(EventLoop&&) = delete;
    EventLoop& operator=(EventLoop&&) = delete;

    std::expected<void, std::error_code> addFd(int fd, Callback callback) noexcept;
    void removeFd(int fd) noexcept;

    std::expected<void, std::error_code> addTimer(int id, int interval_ms, Callback callback) noexcept;

    void run() noexcept;
    void stop() noexcept;

private:
    UniqueFd kq_;
    std::unordered_map<int, Callback> callbacks_;
    std::unordered_map<int, Callback> timer_callbacks_;
    bool running_{false};
};

} // namespace fbx::xserver
