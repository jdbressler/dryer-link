#pragma once
#include <cstdint>
#include <string>
namespace esphome {
namespace wifi {
class WiFiAP {
 public:
  void set_password(const char *password) { password_ = password; }
  std::string password_;
};
class WiFiComponent {
 public:
  bool is_connected() const { return connected; }
  bool has_sta() const { return !ssid.empty(); }
  void save_wifi_sta(const char *new_ssid, const char *new_password);
  WiFiAP get_ap() { return ap; }
  void set_ap(const WiFiAP &new_ap) { ap = new_ap; }
  bool connected{false};
  std::string ssid;
  std::string password;
  uint32_t attempt_started{0};
  WiFiAP ap;  // the setup hotspot; an empty password means it is open
};
extern WiFiComponent *global_wifi_component;  // NOLINT
}  // namespace wifi
}  // namespace esphome
