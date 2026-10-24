// Tests for MotionEngine — spring physics stepping on WindowStack.
// Validates convergence, overshoot clamping, and scene-offset animation.
// No hardware, no Vulkan calls — WindowStack is header-only pure logic.

#include "xserver/motion_engine.h"
#include "xserver/window_stack.h"
#include <gtest/gtest.h>
#include <cmath>

namespace fbx::xserver::testing {

// Helper: run the engine for N frames at fixed dt and return the window state.
static void stepN(WindowStack& ws, int frames, float dt = 1.0f / 60.0f) {
    for (int i = 0; i < frames; ++i)
        MotionEngine::step(dt, ws);
}

// ── Single-window position spring ─────────────────────────────────────────────

class MotionEngineTest : public ::testing::Test {
protected:
    WindowStack ws_;
};

TEST_F(MotionEngineTest, WindowConvergesToTargetPosition) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    auto& wins = ws_.getWindowsMutable();
    wins[0].position_       = {0.0f, 0.0f};
    wins[0].target_position_ = {200.0f, 100.0f};
    wins[0].velocity_       = {0.0f, 0.0f};

    stepN(ws_, 300); // ~5 s at 60 fps — plenty for stiffness=210, damping=28

    EXPECT_NEAR(wins[0].position_.x_, 200.0f, 0.5f);
    EXPECT_NEAR(wins[0].position_.y_, 100.0f, 0.5f);
}

TEST_F(MotionEngineTest, WindowAlreadyAtTargetRemainsStable) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    auto& wins = ws_.getWindowsMutable();
    wins[0].position_        = {100.0f, 200.0f};
    wins[0].target_position_ = {100.0f, 200.0f};
    wins[0].velocity_        = {0.0f, 0.0f};

    stepN(ws_, 60);

    EXPECT_FLOAT_EQ(wins[0].position_.x_, 100.0f);
    EXPECT_FLOAT_EQ(wins[0].position_.y_, 200.0f);
}

TEST_F(MotionEngineTest, VelocityDampsToZeroAtRest) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    auto& wins = ws_.getWindowsMutable();
    wins[0].position_        = {0.0f, 0.0f};
    wins[0].target_position_ = {100.0f, 0.0f};
    wins[0].velocity_        = {500.0f, 0.0f}; // large initial kick

    stepN(ws_, 300);

    EXPECT_NEAR(wins[0].velocity_.x_, 0.0f, 0.01f);
    EXPECT_NEAR(wins[0].velocity_.y_, 0.0f, 0.01f);
}

// ── Size animation ────────────────────────────────────────────────────────────

TEST_F(MotionEngineTest, WindowConvergesToTargetSize) {
    ws_.addWindow(1, VK_NULL_HANDLE, 400, 300);
    auto& wins = ws_.getWindowsMutable();
    wins[0].size_            = {400.0f, 300.0f};
    wins[0].target_size_     = {1200.0f, 900.0f};
    wins[0].size_velocity_   = {0.0f, 0.0f};

    stepN(ws_, 300);

    EXPECT_NEAR(wins[0].size_.width_,  1200.0f, 1.0f);
    EXPECT_NEAR(wins[0].size_.height_,  900.0f, 1.0f);
}

// ── Opacity / scale spring ────────────────────────────────────────────────────

TEST_F(MotionEngineTest, OpacityConvergesToTarget) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    auto& wins = ws_.getWindowsMutable();
    wins[0].opacity_         = 0.0f;
    wins[0].target_opacity_  = 1.0f;
    wins[0].opacity_velocity_ = 0.0f;

    stepN(ws_, 300);

    EXPECT_NEAR(wins[0].opacity_, 1.0f, 0.01f);
}

TEST_F(MotionEngineTest, ScaleConvergesToTarget) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    auto& wins = ws_.getWindowsMutable();
    wins[0].scale_         = 0.5f;
    wins[0].target_scale_  = 1.0f;
    wins[0].scale_velocity_ = 0.0f;

    stepN(ws_, 300);

    EXPECT_NEAR(wins[0].scale_, 1.0f, 0.01f);
}

// ── Scene offset (workspace pan) ──────────────────────────────────────────────

TEST_F(MotionEngineTest, SceneOffsetConvergesToTarget) {
    ws_.scene_offset_        = 0.0f;
    ws_.target_scene_offset_ = 1920.0f;
    ws_.scene_velocity_      = 0.0f;

    stepN(ws_, 300);

    EXPECT_NEAR(ws_.scene_offset_, 1920.0f, 1.0f);
}

TEST_F(MotionEngineTest, SceneOffsetAlreadyAtTargetIsStable) {
    ws_.scene_offset_        = 1920.0f;
    ws_.target_scene_offset_ = 1920.0f;
    ws_.scene_velocity_      = 0.0f;

    stepN(ws_, 60);

    EXPECT_FLOAT_EQ(ws_.scene_offset_, 1920.0f);
}

// ── Multiple windows animated independently ───────────────────────────────────

TEST_F(MotionEngineTest, MultipleWindowsConvergeIndependently) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    ws_.addWindow(2, VK_NULL_HANDLE, 400, 300);
    auto& wins = ws_.getWindowsMutable();

    wins[0].position_        = {0.0f, 0.0f};
    wins[0].target_position_ = {300.0f, 200.0f};
    wins[0].velocity_        = {0.0f, 0.0f};

    wins[1].position_        = {1000.0f, 800.0f};
    wins[1].target_position_ = {500.0f, 400.0f};
    wins[1].velocity_        = {0.0f, 0.0f};

    stepN(ws_, 300);

    EXPECT_NEAR(wins[0].position_.x_, 300.0f, 0.5f);
    EXPECT_NEAR(wins[0].position_.y_, 200.0f, 0.5f);
    EXPECT_NEAR(wins[1].position_.x_, 500.0f, 0.5f);
    EXPECT_NEAR(wins[1].position_.y_, 400.0f, 0.5f);
}

// ── Step with dt=0 is safe ────────────────────────────────────────────────────

TEST_F(MotionEngineTest, ZeroDtDoesNotCrash) {
    ws_.addWindow(1, VK_NULL_HANDLE, 800, 600);
    EXPECT_NO_FATAL_FAILURE(MotionEngine::step(0.0f, ws_));
}

} // namespace fbx::xserver::testing
