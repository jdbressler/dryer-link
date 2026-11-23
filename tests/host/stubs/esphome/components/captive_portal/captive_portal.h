#pragma once
namespace esphome {
namespace captive_portal {
class CaptivePortal {
 public:
  bool is_active() const { return active; }
  bool active{false};
};
extern CaptivePortal *global_captive_portal;  // NOLINT
}  // namespace captive_portal
}  // namespace esphome
