#include "xserver/event_loop.h"
#include <iostream>
#include <unistd.h>
#include <cerrno>

namespace fbx::xserver {

std::expected<std::unique_ptr<EventLoop>, std::error_code> EventLoop::create() noexcept {
    auto loop = std::make_unique<EventLoop>();
    if (!loop->kq_) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    std::cout << "[xserver] EventLoop initialized with kqueue.\n";
    return loop;
}

EventLoop::EventLoop() noexcept {
    kq_.reset(kqueue());
}

EventLoop::~EventLoop() noexcept {
    std::cout << "[xserver] EventLoop shutting down.\n";
}

std::expected<void, std::error_code> EventLoop::addFd(int fd, Callback callback) noexcept {
    if (fd < 0) return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));

    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);

    if (kevent(kq_.get(), &change, 1, nullptr, 0, nullptr) == -1) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }

    callbacks_[fd] = std::move(callback);
    return {};
}

void EventLoop::removeFd(int fd) noexcept {
    if (fd < 0) return;

    struct kevent change;
    EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);

    kevent(kq_.get(), &change, 1, nullptr, 0, nullptr);
    callbacks_.erase(fd);
}

std::expected<void, std::error_code> EventLoop::addTimer(int id, int interval_ms, Callback callback) noexcept {
    struct kevent change;
    EV_SET(&change, id, EVFILT_TIMER, EV_ADD | EV_ENABLE, 0, interval_ms, nullptr);

    if (kevent(kq_.get(), &change, 1, nullptr, 0, nullptr) == -1) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }

    timer_callbacks_[id] = std::move(callback);
    return {};
}

void EventLoop::run() noexcept {
    running_ = true;
    std::cout << "[xserver] EventLoop started running.\n";

    constexpr int MAX_EVENTS = 32;
    struct kevent events[MAX_EVENTS];

    while (running_) {
        int num_events = kevent(kq_.get(), nullptr, 0, events, MAX_EVENTS, nullptr);
        if (num_events == -1) {
            if (errno == EINTR) continue;
            std::cerr << "[xserver] kevent wait failed\n";
            break;
        }

        for (int i = 0; i < num_events; ++i) {
            if (events[i].filter == EVFILT_READ) {
                int fd = events[i].ident;
                auto it = callbacks_.find(fd);
                if (it != callbacks_.end()) {
                    // Copy the callback before invoking it. A handler may call
                    // removeFd() on its own fd (e.g. a client disconnecting),
                    // which does callbacks_.erase(fd) — that would destroy the
                    // std::function currently executing, freeing its captured
                    // state (such as the client's shared_ptr<IpcSocket>) out from
                    // under the running handler. The local copy keeps the functor
                    // and its captures alive until the handler fully returns, so
                    // the socket's fd is closed deterministically afterwards and
                    // never while a reused fd number is in flight.
                    Callback cb = it->second;
                    cb();
                }
            } else if (events[i].filter == EVFILT_TIMER) {
                int id = events[i].ident;
                auto it = timer_callbacks_.find(id);
                if (it != timer_callbacks_.end()) {
                    Callback cb = it->second;
                    cb();
                }
            }
        }
    }
}

void EventLoop::stop() noexcept {
    running_ = false;
}

} // namespace fbx::xserver
