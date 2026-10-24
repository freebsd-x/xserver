#pragma once

#include "xserver/drm_device_interface.h"
#include <gmock/gmock.h>

namespace fbx::xserver::testing {

class MockDrmDevice : public IDrmDevice {
public:
    // C++20 GMock macro syntax
    MOCK_METHOD((std::expected<void, std::error_code>), modeset, (), (noexcept, override));
};

} // namespace fbx::xserver::testing