#include "xserver/seat_manager.h"
#include <iostream>
#include <cerrno>
#include <thread>
#include <chrono>
#include <cstring>
#include <unistd.h>
#include <cstdlib>

namespace fbx::xserver {

static libseat_seat_listener seat_listener = {
    .enable_seat = SeatManager::onEnableSeat,
    .disable_seat = SeatManager::onDisableSeat,
};

std::expected<std::unique_ptr<SeatManager>, std::error_code> SeatManager::create() noexcept {
    auto manager = std::unique_ptr<SeatManager>(new SeatManager());

    const char* backend = getenv("LIBSEAT_BACKEND");
    if (!backend) setenv("LIBSEAT_BACKEND", "seatd", 1);

    manager->seat_ = libseat_open_seat(&seat_listener, manager.get());
    if (!manager->seat_) return std::unexpected(std::error_code(errno, std::generic_category()));

    int tries = 0;
    while (!manager->is_active_ && tries < 50) {
        if (libseat_dispatch(manager->seat_, 10) == -1) return std::unexpected(std::error_code(errno, std::generic_category()));
        tries++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!manager->is_active_) return std::unexpected(std::make_error_code(std::errc::permission_denied));
    return manager;
}

SeatManager::~SeatManager() noexcept {
    if (seat_) {
        libseat_close_seat(seat_);
    }
}

std::expected<SeatDevice, std::error_code> SeatManager::openDevice(const std::string& path) noexcept {
    int fd = -1;
    int device_id = libseat_open_device(seat_, path.c_str(), &fd);
    if (device_id == -1) return std::unexpected(std::error_code(errno, std::generic_category()));

    return SeatDevice{ .fd_ = UniqueFd(fd), .device_id_ = device_id };
}

std::expected<void, std::error_code> SeatManager::closeDevice(int device_id) noexcept {
    // At shutdown the seatd dispatch pump is gone; a blocking close round-trip
    // would wedge teardown. Let the OS reclaim the fd on exit instead.
    if (shutting_down_) return {};
    if (libseat_close_device(seat_, device_id) == -1) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return {};
}

int SeatManager::getFd() const noexcept {
    return libseat_get_fd(seat_);
}

std::expected<void, std::error_code> SeatManager::dispatch(int timeout_ms) noexcept {
    if (libseat_dispatch(seat_, timeout_ms) == -1) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return {};
}

void SeatManager::onEnableSeat(libseat* seat, void* userdata) noexcept {
    (void)seat;
    auto* manager = static_cast<SeatManager*>(userdata);
    manager->is_active_ = true;
}

void SeatManager::onDisableSeat(libseat* seat, void* userdata) noexcept {
    auto* manager = static_cast<SeatManager*>(userdata);
    manager->is_active_ = false;
    libseat_disable_seat(seat);
}

} // namespace fbx::xserver
