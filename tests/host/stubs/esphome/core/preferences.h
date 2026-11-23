#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
namespace esphome {
// Survives a simulated restart: the store is a process-wide map.
std::map<uint32_t, std::vector<uint8_t>> &sim_flash();
extern bool sim_flash_fails;
class ESPPreferenceObject {
 public:
  ESPPreferenceObject() = default;
  explicit ESPPreferenceObject(uint32_t key) : key_(key) {}
  template<typename T> bool save(const T *src) {
    if (sim_flash_fails)
      return false;
    auto &slot = sim_flash()[key_];
    slot.assign((const uint8_t *) src, (const uint8_t *) src + sizeof(T));
    return true;
  }
  template<typename T> bool load(T *dest) {
    auto it = sim_flash().find(key_);
    if (it == sim_flash().end() || it->second.size() != sizeof(T))
      return false;
    memcpy(dest, it->second.data(), sizeof(T));
    return true;
  }

 private:
  uint32_t key_{0};
};
class ESPPreferences {
 public:
  template<typename T> ESPPreferenceObject make_preference(uint32_t key, bool in_flash) { return ESPPreferenceObject(key); }
  bool sync() { return !sim_flash_fails; }
};
extern ESPPreferences *global_preferences;
}  // namespace esphome
