#pragma once
#include <string>
namespace esphome {
class Application {
 public:
  const std::string &get_name() const { return name_; }
  void safe_reboot() { reboot_requested = true; }
  bool is_setup_complete() const { return setup_complete; }
  std::string name_{"dryer-link-3fa2c4"};
  bool reboot_requested{false};
  bool setup_complete{false};
};
extern Application App;  // NOLINT
}  // namespace esphome
