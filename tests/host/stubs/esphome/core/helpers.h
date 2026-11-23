#pragma once
#include <cstdint>
#include <memory>
#include <string>
namespace esphome {
uint32_t random_uint32();
uint32_t fnv1_hash(const std::string &str);
class Mutex {
 public:
  void lock() { depth_++; }
  void unlock() { depth_--; }
  int depth_{0};  // the simulator is single-threaded; this catches a lock taken twice
};
class LockGuard {
 public:
  explicit LockGuard(Mutex &m);
  ~LockGuard() { m_.unlock(); }

 private:
  Mutex &m_;
};
class HighFrequencyLoopRequester {
 public:
  void start() {}
  void stop() {}
};
}  // namespace esphome
