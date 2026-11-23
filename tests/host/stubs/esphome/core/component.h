#pragma once
#include <functional>
#include "esphome/core/hal.h"
namespace esphome {
namespace setup_priority {
constexpr float BUS = 1000.0f;
constexpr float WIFI = 250.0f;
}  // namespace setup_priority
void sim_defer(std::function<void()> &&f);
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
  void defer(std::function<void()> &&f) { sim_defer(std::move(f)); }
};
}  // namespace esphome
