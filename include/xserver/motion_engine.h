#pragma once

#include "xserver/window_stack.h"

namespace fbx::xserver {

class MotionEngine {
public:
    static void step(float dt, WindowStack& window_stack) noexcept;
};

} // namespace fbx::xserver
