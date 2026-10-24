#include "mocks/mock_drm_device.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xserver {

// Dummy DisplayServer demonstrating Dependency Injection
class DisplayServer {
public:
    explicit DisplayServer(std::unique_ptr<IDrmDevice> drm_device) noexcept
        : drm_device_(std::move(drm_device)) {}

    [[nodiscard]] std::expected<void, std::error_code> start() noexcept {
        return drm_device_->modeset();
    }

private:
    std::unique_ptr<IDrmDevice> drm_device_;
};

} // namespace fbx::xserver

using namespace fbx::xserver;
using namespace fbx::xserver::testing;
using ::testing::Return;

class DisplayServerTest : public ::testing::Test {};

TEST_F(DisplayServerTest, StartFailsIfModesetFails) {
    auto mock_drm = std::make_unique<MockDrmDevice>();
    
    // Script the mock to simulate a KMS failure
    EXPECT_CALL(*mock_drm, modeset())
        .WillOnce(Return(std::unexpected(std::make_error_code(std::errc::device_or_resource_busy))));

    DisplayServer server(std::move(mock_drm));
    auto res = server.start();

    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::errc::device_or_resource_busy);
}

TEST_F(DisplayServerTest, StartSucceedsOnModeset) {
    auto mock_drm = std::make_unique<MockDrmDevice>();
    
    // Script the mock to succeed
    EXPECT_CALL(*mock_drm, modeset())
        .WillOnce(Return(std::expected<void, std::error_code>{}));

    DisplayServer server(std::move(mock_drm));
    auto res = server.start();

    EXPECT_TRUE(res.has_value());
}