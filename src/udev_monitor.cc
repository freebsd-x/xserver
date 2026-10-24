#include "xserver/udev_monitor.h"
#include <iostream>
#include <cstring>

namespace fbx::xserver {

UdevMonitor::UdevMonitor(EventCallback callback) noexcept 
    : callback_(std::move(callback)) {}

UdevMonitor::~UdevMonitor() noexcept {
    if (monitor_) {
        udev_monitor_unref(monitor_);
    }
}

std::expected<std::unique_ptr<UdevMonitor>, std::error_code> UdevMonitor::create(struct udev* udev, const char* subsystem, EventCallback callback) noexcept {
    auto udev_mon = std::unique_ptr<UdevMonitor>(new UdevMonitor(std::move(callback)));
    
    udev_mon->monitor_ = udev_monitor_new_from_netlink(udev, "udev");
    if (!udev_mon->monitor_) {
        return std::unexpected(std::make_error_code(std::errc::not_enough_memory));
    }

    if (udev_monitor_filter_add_match_subsystem_devtype(udev_mon->monitor_, subsystem, nullptr) < 0) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }

    if (udev_monitor_enable_receiving(udev_mon->monitor_) < 0) {
        return std::unexpected(std::make_error_code(std::errc::io_error));
    }

    return udev_mon;
}

int UdevMonitor::getFd() const noexcept {
    return udev_monitor_get_fd(monitor_);
}

void UdevMonitor::dispatch() noexcept {
    struct udev_device* dev = udev_monitor_receive_device(monitor_);
    if (dev) {
        const char* action = udev_device_get_action(dev);
        if (action && std::strcmp(action, "change") == 0) {
            if (callback_) {
                callback_();
            }
        }
        udev_device_unref(dev);
    }
}

} // namespace fbx::xserver
