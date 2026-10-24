#include "mocks/mock_seat_manager.h"
#include <gtest/gtest.h>
#include <memory>

namespace fbx::xserver::testing {

// A mock event handler that wraps the libseat integration
class SeatEventHandler {
public:
    explicit SeatEventHandler(std::shared_ptr<ISeatManager> seat_manager) noexcept
        : seat_manager_(std::move(seat_manager)) {}

    [[nodiscard]] std::expected<void, std::error_code> handleSeatEvent() noexcept {
        auto res = seat_manager_->dispatch();
        if (!res) {
            // Gracefully handle the failure (e.g., VT switched or seatd crashed)
            return std::unexpected(res.error());
        }
        return {};
    }

private:
    std::shared_ptr<ISeatManager> seat_manager_;
};

class SeatManagerTest : public ::testing::Test {};

TEST_F(SeatManagerTest, DispatchHandlesVtSwitchFailureGracefully) {
    auto mock_seat = std::make_shared<MockSeatManager>();
    
    // Simulate a libseat dispatch failure when the user switches VT away
    EXPECT_CALL(*mock_seat, dispatch(0))
        .WillOnce(::testing::Return(std::unexpected(std::make_error_code(std::errc::permission_denied))));

    SeatEventHandler handler(mock_seat);
    auto res = handler.handleSeatEvent();

    // Verify the system catches the error instead of crashing
    EXPECT_FALSE(res.has_value());
    EXPECT_EQ(res.error(), std::errc::permission_denied);
}

TEST_F(SeatManagerTest, DispatchSucceedsWhenActive) {
    auto mock_seat = std::make_shared<MockSeatManager>();
    
    // Simulate successful dispatch
    EXPECT_CALL(*mock_seat, dispatch(0))
        .WillOnce(::testing::Return(std::expected<void, std::error_code>{}));

    SeatEventHandler handler(mock_seat);
    auto res = handler.handleSeatEvent();

    EXPECT_TRUE(res.has_value());
}

} // namespace fbx::xserver::testing
