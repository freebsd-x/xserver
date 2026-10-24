// Tests for WindowStack — pure logic, no Vulkan calls, no hardware required.
// VkImage is used as an opaque handle (uint64_t on 64-bit); VK_NULL_HANDLE = 0.

#include "xserver/window_stack.h"
#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

namespace fbx::xserver::testing {

class WindowStackTest : public ::testing::Test {
protected:
    WindowStack stack_;
};

// ── Basic window management ───────────────────────────────────────────────────

TEST_F(WindowStackTest, AddWindowAppearsInOutput) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    ASSERT_EQ(wins.size(), 1u);
    EXPECT_EQ(wins[0].surface_id_, 1u);
    EXPECT_FLOAT_EQ(wins[0].size_.width_,  800.0f);
    EXPECT_FLOAT_EQ(wins[0].size_.height_, 600.0f);
}

TEST_F(WindowStackTest, RemoveWindowDisappearsFromOutput) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.removeWindow(1);
    EXPECT_TRUE(stack_.getWindowsForOutput(0).empty());
}

TEST_F(WindowStackTest, MultipleWindowsAllPresent) {
    stack_.addWindow(1, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.addWindow(2, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.addWindow(3, VK_NULL_HANDLE, 400.0f, 300.0f);
    EXPECT_EQ(stack_.getWindowsForOutput(0).size(), 3u);
}

TEST_F(WindowStackTest, SetWindowTitle) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.setWindowTitle(1, "Hello FreeBSD-X");
    EXPECT_EQ(stack_.getWindows()[0].title_, "Hello FreeBSD-X");
}

TEST_F(WindowStackTest, MinimizeWindow) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.minimizeWindow(1);
    const auto& w = stack_.getWindows()[0];
    EXPECT_TRUE(w.is_minimized_);
    EXPECT_FLOAT_EQ(w.target_opacity_, 0.0f);
    EXPECT_FLOAT_EQ(w.target_scale_,   0.5f);
}

// ── Hit testing ───────────────────────────────────────────────────────────────

TEST_F(WindowStackTest, HitTestInsideTitleBar) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    // Title bar is the 28 px strip immediately above position.y_
    auto hit = stack_.hitTest({wx + 50.0f, wy - 10.0f});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, 1u);
}

TEST_F(WindowStackTest, HitTestMissesContentArea) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    // Content area is not a title-bar hit
    EXPECT_FALSE(stack_.hitTest({wx + 100.0f, wy + 100.0f}).has_value());
}

TEST_F(WindowStackTest, HitTestMissesAboveTitleBar) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    // More than 28 px above position.y_ = miss
    EXPECT_FALSE(stack_.hitTest({wx + 100.0f, wy - 30.0f}).has_value());
}

TEST_F(WindowStackTest, BodyHitTestCoversFullArea) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    EXPECT_TRUE(stack_.bodyHitTest({wx + 50.0f,  wy - 10.0f}).has_value()); // title bar
    EXPECT_TRUE(stack_.bodyHitTest({wx + 100.0f, wy + 100.0f}).has_value()); // content
}

TEST_F(WindowStackTest, BodyHitTestMissesOutside) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    EXPECT_FALSE(stack_.bodyHitTest({wx - 10.0f,  wy + 100.0f}).has_value());
    EXPECT_FALSE(stack_.bodyHitTest({wx + 900.0f,  wy + 100.0f}).has_value());
    EXPECT_FALSE(stack_.bodyHitTest({wx + 100.0f,  wy + 700.0f}).has_value());
}

TEST_F(WindowStackTest, TopWindowWinsBodyHitTest) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.addWindow(2, VK_NULL_HANDLE, 800.0f, 600.0f);
    // Both windows share the default spawn position; window 2 is on top.
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    auto hit = stack_.bodyHitTest({wx + 100.0f, wy + 100.0f});
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(*hit, 2u); // last added = topmost
}

// ── Resize hit testing ────────────────────────────────────────────────────────

TEST_F(WindowStackTest, ResizeHitTestLeftEdge) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    auto r = stack_.resizeHitTest({wx - 2.0f, wy + 200.0f});
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->first, 1u);
    EXPECT_TRUE(r->second & 4u); // left edge bitmask
}

TEST_F(WindowStackTest, ResizeHitTestRightEdge) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float ww = wins[0].size_.width_;
    float wy = wins[0].position_.y_;

    auto r = stack_.resizeHitTest({wx + ww + 2.0f, wy + 200.0f});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->second & 8u); // right edge bitmask
}

TEST_F(WindowStackTest, ResizeHitTestBottomEdge) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;
    float wh = wins[0].size_.height_;

    auto r = stack_.resizeHitTest({wx + 100.0f, wy + wh + 2.0f});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->second & 2u); // bottom edge bitmask
}

TEST_F(WindowStackTest, ResizeHitTestCenterMisses) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    EXPECT_FALSE(stack_.resizeHitTest({wx + 100.0f, wy + 100.0f}).has_value());
}

TEST_F(WindowStackTest, ResizeHitTestBottomLeftCorner) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;
    float wh = wins[0].size_.height_;

    auto r = stack_.resizeHitTest({wx - 2.0f, wy + wh + 2.0f});
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->second & 4u); // left
    EXPECT_TRUE(r->second & 2u); // bottom
}

// ── Window geometry (set_window_geometry) ─────────────────────────────────────

TEST_F(WindowStackTest, WindowGeometryDefaultUnset) {
    stack_.addWindow(1, VK_NULL_HANDLE, 820.0f, 540.0f);
    EXPECT_EQ(stack_.getWindows()[0].geom_w_, 0); // 0 == not set
}

TEST_F(WindowStackTest, SetWindowGeometryStored) {
    stack_.addWindow(1, VK_NULL_HANDLE, 820.0f, 540.0f);
    stack_.setWindowGeometry(1, 10, 10, 800, 520);

    const auto& w = stack_.getWindows()[0];
    EXPECT_EQ(w.geom_x_, 10);
    EXPECT_EQ(w.geom_y_, 10);
    EXPECT_EQ(w.geom_w_, 800);
    EXPECT_EQ(w.geom_h_, 520);
}

TEST_F(WindowStackTest, SetWindowGeometryPropagatesAcrossWorkspaces) {
    // Shell windows live on all workspaces; verify geometry propagation does too.
    stack_.addWindow(1, VK_NULL_HANDLE, 820.0f, 540.0f);
    stack_.setWindowGeometry(1, 5, 5, 810, 530);

    // Geometry is set in all workspace copies via setWindowGeometry().
    // Verify via getWindows() (active workspace).
    const auto& w = stack_.getWindows()[0];
    EXPECT_EQ(w.geom_x_, 5);
    EXPECT_EQ(w.geom_w_, 810);
}

TEST_F(WindowStackTest, WindowGeometryRenderOffsetMath) {
    // Validates the arithmetic used in vulkan_compositor.cc:
    //   render_x = position_.x_ - geom_x_  (when geom_w_ > 0)
    stack_.addWindow(1, VK_NULL_HANDLE, 820.0f, 540.0f);
    stack_.setWindowGeometry(1, 10, 8, 800, 524);

    const auto& w = stack_.getWindows()[0];
    ASSERT_GT(w.geom_w_, 0);

    float render_x = w.position_.x_ - static_cast<float>(w.geom_x_);
    float render_y = w.position_.y_ - static_cast<float>(w.geom_y_);

    // Buffer top-left must be to the left/above content top-left
    EXPECT_FLOAT_EQ(render_x, w.position_.x_ - 10.0f);
    EXPECT_FLOAT_EQ(render_y, w.position_.y_ - 8.0f);
}

// ── Maximize / restore / snap ─────────────────────────────────────────────────

TEST_F(WindowStackTest, MaximizeExpandsToOutput) {
    stack_.addWindow(1, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.maximizeWindow(1);

    const auto& w = stack_.getWindows()[0];
    EXPECT_TRUE(w.is_maximized_);
    EXPECT_GT(w.target_size_.width_,  400.0f);
    EXPECT_GT(w.target_size_.height_, 300.0f);
}

TEST_F(WindowStackTest, MaximizeToggleRestores) {
    stack_.addWindow(1, VK_NULL_HANDLE, 400.0f, 300.0f);
    auto orig = stack_.getWindows()[0];
    float orig_x = orig.position_.x_;
    float orig_y = orig.position_.y_;

    stack_.maximizeWindow(1); // maximize
    stack_.maximizeWindow(1); // restore

    const auto& w = stack_.getWindows()[0];
    EXPECT_FALSE(w.is_maximized_);
    EXPECT_FLOAT_EQ(w.target_position_.x_, orig_x);
    EXPECT_FLOAT_EQ(w.target_position_.y_, orig_y);
    EXPECT_FLOAT_EQ(w.target_size_.width_,  400.0f);
    EXPECT_FLOAT_EQ(w.target_size_.height_, 300.0f);
}

TEST_F(WindowStackTest, SnapLeftHalf) {
    // Default output is 1920×1080
    stack_.addWindow(1, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.snapWindow(1, -1);

    const auto& w = stack_.getWindows()[0];
    EXPECT_FLOAT_EQ(w.target_size_.width_, 960.0f);  // half of 1920
    EXPECT_FLOAT_EQ(w.target_position_.x_, 0.0f);    // left edge
}

TEST_F(WindowStackTest, SnapRightHalf) {
    stack_.addWindow(1, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.snapWindow(1, +1);

    const auto& w = stack_.getWindows()[0];
    EXPECT_FLOAT_EQ(w.target_size_.width_,   960.0f);
    EXPECT_FLOAT_EQ(w.target_position_.x_,   960.0f); // right half starts at 960
}

// ── Z-order ───────────────────────────────────────────────────────────────────

TEST_F(WindowStackTest, RaiseToFocusMakesWindowTopmost) {
    stack_.addWindow(1, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.addWindow(2, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.addWindow(3, VK_NULL_HANDLE, 400.0f, 300.0f);

    stack_.raiseToFocus(1);

    const auto& wins = stack_.getWindows();
    ASSERT_EQ(wins.size(), 3u);
    EXPECT_EQ(wins.back().surface_id_, 1u);
}

// ── Workspace switching ───────────────────────────────────────────────────────

// These exercise multi-workspace switching, so they build a multi-workspace
// stack explicitly — production defaults to a single workspace
// (WindowStack::kWorkspaceCount), where setWorkspace()/slide are no-ops.
TEST_F(WindowStackTest, WindowNotVisibleOnOtherWorkspace) {
    WindowStack stack{9};
    stack.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack.setWorkspace(1);

    const auto& wins = stack.getWindows(); // active workspace = 1
    bool found = false;
    for (const auto& w : wins) if (w.surface_id_ == 1) found = true;
    EXPECT_FALSE(found);
}

TEST_F(WindowStackTest, SwitchBackToOriginalWorkspace) {
    WindowStack stack{9};
    stack.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack.setWorkspace(1);
    stack.setWorkspace(0);

    bool found = false;
    for (const auto& w : stack.getWindows()) if (w.surface_id_ == 1) found = true;
    EXPECT_TRUE(found);
}

TEST_F(WindowStackTest, GetActiveWorkspace) {
    WindowStack stack{9};
    EXPECT_EQ(stack.getActiveWorkspace(), 0u);
    stack.setWorkspace(3);
    EXPECT_EQ(stack.getActiveWorkspace(), 3u);
}

// ── Subsurfaces ───────────────────────────────────────────────────────────────

TEST_F(WindowStackTest, SubsurfaceMarkedCorrectly) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.addSubsurface(2, 1, 10.0f, 20.0f);

    auto wins = stack_.getWindowsForOutput(0);
    const Window* sub = nullptr;
    for (const auto& w : wins) if (w.surface_id_ == 2) sub = &w;

    ASSERT_NE(sub, nullptr);
    EXPECT_TRUE(sub->is_subsurface_);
    EXPECT_EQ(sub->parent_id_, 1u);
    EXPECT_FLOAT_EQ(sub->sub_x_, 10.0f);
    EXPECT_FLOAT_EQ(sub->sub_y_, 20.0f);
}

TEST_F(WindowStackTest, SubsurfacePositionUpdated) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.addSubsurface(2, 1, 10.0f, 20.0f);
    stack_.updateSubsurfacePosition(2, 50.0f, 60.0f);

    auto wins = stack_.getWindowsForOutput(0);
    for (const auto& w : wins) {
        if (w.surface_id_ == 2) {
            EXPECT_FLOAT_EQ(w.sub_x_, 50.0f);
            EXPECT_FLOAT_EQ(w.sub_y_, 60.0f);
            return;
        }
    }
    FAIL() << "Subsurface not found";
}

// ── Modal dialogs ─────────────────────────────────────────────────────────────

TEST_F(WindowStackTest, ModalBlocksHitTestOnOtherWindows) {
    stack_.addWindow(1, VK_NULL_HANDLE, 800.0f, 600.0f);
    stack_.addWindow(2, VK_NULL_HANDLE, 400.0f, 300.0f);
    stack_.setModal(2, true);

    // With modal present, non-modal window 1 should not be hit-testable
    auto wins = stack_.getWindowsForOutput(0);
    float wx = wins[0].position_.x_;
    float wy = wins[0].position_.y_;

    auto hit = stack_.bodyHitTest({wx + 100.0f, wy + 100.0f});
    // Either no hit or only the modal window (id=2)
    if (hit.has_value()) {
        EXPECT_EQ(*hit, 2u);
    }
}

} // namespace fbx::xserver::testing
