#pragma once

#include <libudev.h>
#include <functional>
#include <memory>
#include <expected>
#include <system_error>

namespace fbx::xserver {

class UdevMonitor {
public:
    using EventCallback = std::function<void()>;

    [[nodiscard]] static std::expected<std::unique_ptr<UdevMonitor>, std::error_code> create(struct udev* udev, const char* subsystem, EventCallback callback) noexcept;
    
    ~UdevMonitor() noexcept;

    UdevMonitor(const UdevMonitor&) = delete;
    UdevMonitor& operator=(const UdevMonitor&) = delete;
    UdevMonitor(UdevMonitor&&) noexcept = default;
    UdevMonitor& operator=(UdevMonitor&&) noexcept = default;

    [[nodiscard]] int getFd() const noexcept;
    void dispatch() noexcept;

private:
    explicit UdevMonitor(EventCallback callback) noexcept;
    
    struct udev_monitor* monitor_{nullptr};
    EventCallback callback_;
};

} // namespace fbx::xserver
