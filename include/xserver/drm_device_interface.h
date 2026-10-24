#pragma once

#include <expected>
#include <system_error>

namespace fbx::xserver {

class IDrmDevice {
public:
    virtual ~IDrmDevice() noexcept = default;

    // Abstracted hardware methods
    [[nodiscard]] virtual std::expected<void, std::error_code> modeset() noexcept = 0;
};

} // namespace fbx::xserver