#pragma once
#include <cstddef>
#include <cstdint>
namespace esphome {
namespace uart {
class UARTComponent {
 public:
  virtual ~UARTComponent() = default;
  virtual void write_array(const uint8_t *data, size_t len) = 0;
  virtual bool read_byte(uint8_t *data) = 0;
  virtual size_t available() = 0;
};
class UARTDevice {
 public:
  void set_uart_parent(UARTComponent *parent) { parent_ = parent; }
  void write_array(const uint8_t *data, size_t len) { parent_->write_array(data, len); }
  void write_byte(uint8_t data) { parent_->write_array(&data, 1); }
  bool read_byte(uint8_t *data) { return parent_->read_byte(data); }
  size_t available() { return parent_->available(); }

 protected:
  UARTComponent *parent_{nullptr};
};
}  // namespace uart
}  // namespace esphome
