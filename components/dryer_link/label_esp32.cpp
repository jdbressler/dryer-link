// Reads this module's setup password from flash. See read_setup_password() in dryer_link.h.
#ifdef USE_ESP32
#include "dryer_link.h"

#include <esp_partition.h>

namespace esphome {
namespace dryer_link {

std::string read_setup_password() {
  // Type 0x40, subtype 0x00, named "dl_label": declared by this component's __init__.py, written by
  // tools/flash_module.py, never touched by a firmware update.
  const esp_partition_t *part = esp_partition_find_first((esp_partition_type_t) 0x40, (esp_partition_subtype_t) 0x00, "dl_label");
  if (part == nullptr)
    return "";
  uint8_t buf[80];
  if (esp_partition_read(part, 0, buf, sizeof(buf)) != ESP_OK)
    return "";
  return parse_label(buf, sizeof(buf));
}

}  // namespace dryer_link
}  // namespace esphome
#endif  // USE_ESP32
