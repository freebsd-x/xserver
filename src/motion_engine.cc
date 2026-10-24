#include "xserver/motion_engine.h"
#include <cmath>
#include <algorithm>

namespace fbx::xserver {

void MotionEngine::step(float dt, WindowStack& window_stack) noexcept {
    // Premium Spring Constants (iOS-like damping)
    const float stiffness = 210.0f;
    const float damping = 28.0f;

    auto step_spring = [&](float& current, float target, float& velocity) {
        float force = -stiffness * (current - target) - damping * velocity;
        velocity += force * dt;
        current += velocity * dt;

        if (std::abs(current - target) < 0.01f && std::abs(velocity) < 0.01f) {
            current = target;
            velocity = 0.0f;
        }
    };

    // Animate Global Scene Offset (Workspaces)
    step_spring(window_stack.scene_offset_, window_stack.target_scene_offset_, window_stack.scene_velocity_);

    for (auto& w : window_stack.getWindowsMutable()) {
        step_spring(w.position_.x_, w.target_position_.x_, w.velocity_.x_);
        step_spring(w.position_.y_, w.target_position_.y_, w.velocity_.y_);
        
        step_spring(w.size_.width_, w.target_size_.width_, w.size_velocity_.width_);
        step_spring(w.size_.height_, w.target_size_.height_, w.size_velocity_.height_);
        
        step_spring(w.scale_, w.target_scale_, w.scale_velocity_);
        step_spring(w.opacity_, w.target_opacity_, w.opacity_velocity_);
    }
}

} // namespace fbx::xserver
