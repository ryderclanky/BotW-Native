// BotW-Native shim: Cemu's input manager is not used by the GPU path.
#pragma once
#include <memory>
class VPADController {
   public:
    bool is_screen_active() const { return false; }
    bool is_screen_active_toggle() const { return false; }
};
class InputManager {
   public:
    static constexpr int kMaxController = 8;
    static InputManager& instance() {
        static InputManager m;
        return m;
    }
    std::shared_ptr<VPADController> get_vpad_controller(int) const { return nullptr; }
};
