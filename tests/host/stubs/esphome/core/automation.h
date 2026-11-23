#pragma once
namespace esphome {
template<typename... Ts> class Trigger {
 public:
  void trigger(Ts... x) {}
};
}  // namespace esphome
