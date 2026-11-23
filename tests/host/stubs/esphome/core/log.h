#pragma once
#include <cstdio>
namespace esphome {
extern int sim_log_level;  // 0 none, 1 error, 2 warn, 3 info, 4 config, 5 debug
void sim_log(int level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
}  // namespace esphome
#define ESP_LOGE(tag, ...) ::esphome::sim_log(1, tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::esphome::sim_log(2, tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ::esphome::sim_log(3, tag, __VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) ::esphome::sim_log(4, tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ::esphome::sim_log(5, tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) \
  do { \
  } while (0)
#define ESP_LOGVV(tag, ...) \
  do { \
  } while (0)
#define YESNO(b) ((b) ? "YES" : "NO")
