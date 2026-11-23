#pragma once
// The parts of ESPHome's update entity that dryer_link reads and calls.
#include <cstdint>
#include <string>
namespace esphome {
namespace update {
struct UpdateInfo {
  std::string latest_version;
  std::string current_version;
  std::string md5;
};
enum UpdateState : uint8_t {
  UPDATE_STATE_UNKNOWN,
  UPDATE_STATE_NO_UPDATE,
  UPDATE_STATE_AVAILABLE,
  UPDATE_STATE_INSTALLING,
};
class UpdateEntity {
 public:
  virtual ~UpdateEntity() = default;
  void perform() { this->perform(false); }
  virtual void perform(bool force) = 0;
  virtual void check() = 0;
  const UpdateInfo &update_info = update_info_;
  const UpdateState &state = state_;

 protected:
  UpdateState state_{UPDATE_STATE_UNKNOWN};
  UpdateInfo update_info_;
};
}  // namespace update
}  // namespace esphome
