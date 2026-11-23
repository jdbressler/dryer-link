#pragma once
namespace esphome {
bool sim_setup_complete();
void sim_fault(const char *what);
class EntityBase {
 public:
  bool is_internal() const { return internal_; }
  void set_internal(bool internal) {
    if (sim_setup_complete())
      sim_fault("set_internal() called after setup");  // ESPHome calls this undefined behaviour
    internal_ = internal;
  }

 private:
  bool internal_{false};
};
}  // namespace esphome
