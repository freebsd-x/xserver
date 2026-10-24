#pragma once

#include "xserver/seat_manager.h"
#include <gmock/gmock.h>

namespace fbx::xserver::testing {

class MockSeatManager : public ISeatManager {
public:
    MOCK_METHOD((std::expected<SeatDevice, std::error_code>), openDevice, (const std::string& path), (noexcept, override));
    MOCK_METHOD((std::expected<void, std::error_code>), closeDevice, (int device_id), (noexcept, override));
    MOCK_METHOD(int, getFd, (), (const, noexcept, override));
    MOCK_METHOD((std::expected<void, std::error_code>), dispatch, (int timeout_ms), (noexcept, override));
};

} // namespace fbx::xserver::testing