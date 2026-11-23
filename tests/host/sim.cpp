// Runs the real bus component and the real dryer_link component on a computer, against pretend dryers.
//
//   sim --test                      run the scripted scenarios and check the results
//   sim --serve PORT [--speed N] [--dryer NAME] [--test-build] [--label PASSWORD]
//                                   serve the real setup page and the module's API on 127.0.0.1:PORT, so the page can be
//                                   clicked through (by hand or by tests/host/walk.js) against the firmware's own logic.
//                                   Time runs N times faster than real time (default 1). Pretend dryers: gtd65 (default),
//                                   new, washer, none. --test-build behaves like dryer-link-test.yaml (walkthrough also
//                                   on the home network). --label gives the module a label password. Extra addresses
//                                   for the test driver: /sim/door?open=0|1, /sim/state?v=N, /sim/cable?in=0|1, /sim/info
//   sim --parse-label HEX           print the setup password in a label partition image (hex), or nothing if it is not
//                                   valid. Used to check tools/flash_module.py against the firmware's own parser.
//
// What this does not prove: anything about a real dryer's timing or a real phone. It checks the firmware's decisions.

#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/captive_portal/captive_portal.h"
#include "esphome/components/dryer_link/dryer_link.h"
#include "esphome/components/gea/gea.h"
#include "esphome/components/wifi/wifi_component.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace esphome;

// =====================================================================================================================
// The pieces of ESPHome the stubs leave to us
// =====================================================================================================================

static uint32_t g_ms = 0;
static std::vector<std::function<void()>> g_deferred;
static std::vector<std::string> g_faults;
static std::vector<std::string> g_log;  // warnings and errors, for the tests

namespace esphome {
int sim_log_level = 2;
uint32_t millis() { return g_ms; }
void sim_log(int level, const char *tag, const char *fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (level <= 2)
    g_log.push_back(std::string(tag) + ": " + buf);
  if (level <= sim_log_level)
    fprintf(stderr, "[%7.3f][%c][%s] %s\n", g_ms / 1000.0, " EWICD"[level], tag, buf);
}
uint32_t random_uint32() { return (uint32_t) rand(); }
uint32_t fnv1_hash(const std::string &str) {
  uint32_t h = 2166136261UL;
  for (char c : str) {
    h *= 16777619UL;
    h ^= (uint8_t) c;
  }
  return h;
}
LockGuard::LockGuard(Mutex &m) : m_(m) {
  if (m.depth_ != 0)
    sim_fault("lock taken twice (would deadlock on the module)");
  m.lock();
}
std::map<uint32_t, std::vector<uint8_t>> &sim_flash() {
  static std::map<uint32_t, std::vector<uint8_t>> flash;
  return flash;
}
bool sim_flash_fails = false;
static ESPPreferences g_prefs;
ESPPreferences *global_preferences = &g_prefs;
void sim_defer(std::function<void()> &&f) { g_deferred.push_back(std::move(f)); }
bool sim_setup_complete() { return App.setup_complete; }
void sim_fault(const char *what) {
  fprintf(stderr, "FAULT at %.3f s: %s\n", g_ms / 1000.0, what);
  g_faults.push_back(what);
}
Application App;
namespace captive_portal {
static CaptivePortal g_portal;
CaptivePortal *global_captive_portal = &g_portal;
}  // namespace captive_portal
namespace wifi {
static WiFiComponent g_wifi;
WiFiComponent *global_wifi_component = &g_wifi;
void WiFiComponent::save_wifi_sta(const char *new_ssid, const char *new_password) {
  ssid = new_ssid;
  password = new_password;
  connected = false;
  attempt_started = g_ms;
}
}  // namespace wifi
}  // namespace esphome

extern const uint8_t DRYER_LINK_PAGE_GZ[];
extern const size_t DRYER_LINK_PAGE_GZ_SIZE;

// The module's label partition, as flash holds it: erased flash reads 0xFF.
static std::vector<uint8_t> g_label(80, 0xFF);
namespace esphome {
namespace dryer_link {
std::string read_setup_password() { return parse_label(g_label.data(), g_label.size()); }
}  // namespace dryer_link
}  // namespace esphome

// Writes a label the way tools/flash_module.py does (written here independently of the firmware's parser).
static void set_label(const char *password) {
  g_label.assign(80, 0xFF);
  if (password == nullptr)
    return;
  std::vector<uint8_t> blob = {'D', 'L', 'K', '1', (uint8_t) strlen(password)};
  blob.insert(blob.end(), password, password + strlen(password));
  uint16_t crc = 0xFFFF;  // CRC-16/CCITT-FALSE, as Python's binascii.crc_hqx(data, 0xFFFF)
  for (uint8_t b : blob) {
    for (int bit = 7; bit >= 0; bit--) {
      const bool top = (crc & 0x8000) != 0, in = ((b >> bit) & 1) != 0;
      crc = (uint16_t)(crc << 1);
      if (top != in)
        crc ^= 0x1021;
    }
  }
  blob.push_back(crc >> 8);
  blob.push_back(crc & 0xFF);
  std::copy(blob.begin(), blob.end(), g_label.begin());
}

// =====================================================================================================================
// Wire formats (written independently of the component, from docs/protocol.md)
// =====================================================================================================================

static uint16_t crc16(const std::vector<uint8_t> &d) {
  uint16_t crc = 0x1021;
  for (uint8_t b : d) {
    crc ^= (uint16_t) b << 8;
    for (int j = 0; j < 8; j++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

static std::vector<uint8_t> frame(uint8_t dest, uint8_t src, const std::vector<uint8_t> &payload) {
  std::vector<uint8_t> inner = {dest, (uint8_t)(7 + payload.size()), src};
  inner.insert(inner.end(), payload.begin(), payload.end());
  uint16_t crc = crc16(inner);
  inner.push_back(crc >> 8);
  inner.push_back(crc & 0xFF);
  std::vector<uint8_t> out = {0xE2};
  for (uint8_t b : inner) {
    if (b >= 0xE0 && b <= 0xE3)
      out.push_back(0xE0);
    out.push_back(b);
  }
  out.push_back(0xE3);
  return out;
}

// Collects what the module transmits and hands over whole, CRC-checked frames: {dest, len, src, payload...}.
struct FrameParser {
  std::vector<uint8_t> buf;
  bool in_frame{false}, esc{false};
  bool push(uint8_t b, std::vector<uint8_t> &out) {
    if (!in_frame) {
      if (b == 0xE2) {
        in_frame = true;
        esc = false;
        buf.clear();
      }
      return false;
    }
    if (esc) {
      buf.push_back(b);
      esc = false;
      return false;
    }
    if (b == 0xE0) {
      esc = true;
      return false;
    }
    if (b == 0xE2) {
      buf.clear();
      return false;
    }
    if (b != 0xE3) {
      buf.push_back(b);
      return false;
    }
    in_frame = false;
    if (buf.size() < 6)
      return false;
    std::vector<uint8_t> body(buf.begin(), buf.end() - 2);
    if (crc16(body) != (((uint16_t) buf[buf.size() - 2] << 8) | buf[buf.size() - 1]))
      return false;
    out = body;
    return true;
  }
};

struct FakeUart : uart::UARTComponent {
  std::deque<std::pair<uint32_t, uint8_t>> rx;  // (time it arrives, byte)
  std::function<void(const uint8_t *, size_t)> on_write;
  uint32_t last_due{0};
  void write_array(const uint8_t *data, size_t len) override {
    if (on_write)
      on_write(data, len);
  }
  bool read_byte(uint8_t *data) override {
    if (rx.empty() || rx.front().first > g_ms)
      return false;
    *data = rx.front().second;
    rx.pop_front();
    return true;
  }
  size_t available() override {
    size_t n = 0;
    for (auto &e : rx) {
      if (e.first > g_ms)
        break;
      n++;
    }
    return n;
  }
  // Bytes arrive one after another at centi_ms_per_byte hundredths of a millisecond each, not before `at`.
  uint32_t feed(uint32_t at, const std::vector<uint8_t> &bytes, uint32_t centi_ms_per_byte) {
    if (at < last_due)
      at = last_due;
    for (size_t i = 0; i < bytes.size(); i++) {
      last_due = at + (uint32_t)((i + 1) * centi_ms_per_byte / 100);
      rx.emplace_back(last_due, bytes[i]);
    }
    return last_due;
  }
};

// =====================================================================================================================
// A pretend classic (GEA2) bus: one wire, everything the module sends comes back to it, boards answer reads
// =====================================================================================================================

struct Board {
  uint8_t addr;
  std::map<uint16_t, std::vector<uint8_t>> erds;
  bool polls_wifi_module{false};  // the GTD65 asks address 0xBF for 0x6003 every 5 s
  bool answers_broadcast{true};   // false: only answers when asked at its own address
  bool accepts_writes{true};      // false: says nothing to a write (as documented for a GFD48GSSK0WW)
};

struct ClassicBus {
  FakeUart uart;
  std::vector<Board> boards;
  bool cable_in{true};
  FrameParser parser;
  uint32_t wire_free_at{0};
  int corrupt_reads_of{-1};  // ERD whose next read request is garbled on the wire (a collision), once
  std::map<uint16_t, int> reads;  // how often each ERD was asked for
  int writes{0};
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> written;  // every write request seen: (ERD, value)
  uint32_t last_poll{0};

  ClassicBus() {
    uart.on_write = [this](const uint8_t *d, size_t n) { this->from_module(d, n); };
  }

  void from_module(const uint8_t *d, size_t n) {
    std::vector<uint8_t> bytes(d, d + n), echo = bytes;
    bool garble = false;
    // Peek: is this a read request for the ERD we were told to collide with?
    if (corrupt_reads_of >= 0 && n >= 9) {
      FrameParser peek;
      std::vector<uint8_t> f;
      for (uint8_t b : bytes) {
        if (peek.push(b, f) && f.size() >= 7 && f[3] == 0xF0 && (((uint16_t) f[5] << 8) | f[6]) == (uint16_t) corrupt_reads_of) {
          garble = true;
          corrupt_reads_of = -1;
        }
      }
    }
    if (garble)
      echo[echo.size() / 2] ^= 0x5A;
    uint32_t start = std::max(g_ms, wire_free_at);
    uint32_t end = uart.feed(start, echo, 52);  // 19200 baud: 0.52 ms per byte
    wire_free_at = end;
    if (garble)
      return;  // nobody understood it
    std::vector<uint8_t> f;
    for (uint8_t b : bytes) {
      if (parser.push(b, f))
        this->frame_seen(f, end);
    }
  }

  void frame_seen(const std::vector<uint8_t> &f, uint32_t at) {
    if (!cable_in || f.size() < 4)
      return;
    const uint8_t dest = f[0], src = f[2], cmd = f[3];
    uint32_t t = at;
    if (cmd == 0xF1 && f.size() >= 8 && f[4] == 1) {
      writes++;
      written.emplace_back((uint16_t)(((uint16_t) f[5] << 8) | f[6]),
                           std::vector<uint8_t>(f.begin() + 8, f.begin() + std::min(f.size(), (size_t) 8 + f[7])));
    }
    for (auto &b : boards) {
      if (dest != b.addr && !(dest == 0xFF && b.answers_broadcast))
        continue;
      if (cmd == 0xF0 && f.size() >= 7 && f[4] == 1) {
        const uint16_t erd = ((uint16_t) f[5] << 8) | f[6];
        if (dest != 0xFF)
          reads[erd]++;
        auto it = b.erds.find(erd);
        if (it == b.erds.end())
          continue;  // a board that does not have the ERD says nothing
        std::vector<uint8_t> p = {0xF0, 0x01, f[5], f[6], (uint8_t) it->second.size()};
        p.insert(p.end(), it->second.begin(), it->second.end());
        t = uart.feed(t + 3, {0xE1}, 52);
        t = uart.feed(t + 9, frame(src, b.addr, p), 52);
        wire_free_at = t;
      } else if (cmd == 0xF1 && f.size() >= 8 && f[4] == 1) {
        const uint16_t erd = ((uint16_t) f[5] << 8) | f[6];
        auto it = b.erds.find(erd);
        if (it == b.erds.end() || !b.accepts_writes)
          continue;
        const uint8_t size = f[7];
        if (erd == 0x201B && size == 1 && f.size() >= 9)
          it->second = {f[8]};
        if (erd == 0x2040 && size == 1 && f.size() >= 9 && f[8] == 0)
          b.erds[0x2000] = {0x00};
        t = uart.feed(t + 3, {0xE1}, 52);
        t = uart.feed(t + 9, frame(src, b.addr, {0xF1, 0x01, f[5], f[6]}), 52);
        wire_free_at = t;
      }
    }
  }

  void tick() {
    // The dryer's own chatter, when the wire is free.
    if (cable_in && g_ms - last_poll >= 5000 && g_ms > wire_free_at + 20) {
      last_poll = g_ms;
      for (auto &b : boards) {
        if (b.polls_wifi_module)
          wire_free_at = uart.feed(g_ms, frame(0xBF, b.addr, {0xF0, 0x01, 0x60, 0x03}), 52);
      }
    }
  }
};

// =====================================================================================================================
// A pretend new (GEA3) appliance: two wires, no echo, answers a subscription by publishing everything it has
// =====================================================================================================================

struct NewBus {
  FakeUart uart;
  bool present{false};
  uint8_t addr{0xC0};
  std::map<uint16_t, std::vector<uint8_t>> erds;
  FrameParser parser;
  uint8_t pub_id{1};
  int reads{0};
  bool answers_broadcast{true};   // false: only answers at its own address
  bool refuse_subscription{false};  // answers "no available subscriptions" (another client holds the only one)
  int busy_reads{0};              // this many read requests are answered with "busy" first
  std::vector<uint16_t> writable;  // data points that accept a write
  std::vector<std::pair<uint16_t, std::vector<uint8_t>>> written;
  std::vector<uint8_t> asked_from;  // the source address of every request seen
  int subscriptions{0};

  NewBus() {
    uart.on_write = [this](const uint8_t *d, size_t n) {
      std::vector<uint8_t> f;
      for (size_t i = 0; i < n; i++) {
        if (parser.push(d[i], f))
          this->frame_seen(f);
      }
    };
  }

  void frame_seen(const std::vector<uint8_t> &f) {
    if (!present || f.size() < 4 || !(f[0] == addr || (f[0] == 0xFF && answers_broadcast)))
      return;
    const uint8_t src = f[2], cmd = f[3];
    asked_from.push_back(src);
    uint32_t t = uart.feed(g_ms + 1, {0xE1}, 5);  // 230400 baud
    if (cmd == 0xA4 && f.size() >= 6) {
      subscriptions++;
      if (refuse_subscription) {
        uart.feed(t + 1, frame(src, addr, {0xA5, f[4], 0x01}), 5);
        return;
      }
      t = uart.feed(t + 1, frame(src, addr, {0xA5, f[4], 0x00}), 5);
      if (f[5] != 0x00)
        return;  // keep-alive: nothing new to publish
      auto it = erds.begin();
      while (it != erds.end()) {
        std::vector<uint8_t> p = {0xA6, 0x00, pub_id++, 0x00};
        for (int n = 0; n < 4 && it != erds.end(); n++, ++it) {
          p.push_back(it->first >> 8);
          p.push_back(it->first & 0xFF);
          p.push_back((uint8_t) it->second.size());
          p.insert(p.end(), it->second.begin(), it->second.end());
          p[3]++;
        }
        t = uart.feed(t + 6, frame(src, addr, p), 5);
      }
    } else if (cmd == 0xA0 && f.size() >= 7) {
      reads++;
      const uint16_t erd = ((uint16_t) f[5] << 8) | f[6];
      auto it = erds.find(erd);
      // GE's own code: result 0 = here it is, 1 = no such data point, 2 = busy. Only a success carries a value.
      uint8_t result = it == erds.end() ? 0x01 : 0x00;
      if (busy_reads > 0) {
        busy_reads--;
        result = 0x02;
      }
      std::vector<uint8_t> p = {0xA1, f[4], result, f[5], f[6]};
      if (result == 0x00) {
        p.push_back((uint8_t) it->second.size());
        p.insert(p.end(), it->second.begin(), it->second.end());
      }
      uart.feed(t + 2, frame(src, addr, p), 5);
    } else if (cmd == 0xA2 && f.size() >= 8) {
      const uint16_t erd = ((uint16_t) f[5] << 8) | f[6];
      written.emplace_back(erd, std::vector<uint8_t>(f.begin() + 8, f.begin() + std::min(f.size(), (size_t) 8 + f[7])));
      const bool ok = std::find(writable.begin(), writable.end(), erd) != writable.end();
      uart.feed(t + 2, frame(src, addr, {0xA3, f[4], (uint8_t)(ok ? 0x00 : 0x01), f[5], f[6]}), 5);
    }
  }
};

// =====================================================================================================================
// The module
// =====================================================================================================================

static std::vector<uint8_t> text32(const char *s) {
  std::vector<uint8_t> v(32, 0);
  for (size_t i = 0; s[i] != '\0' && i < 32; i++)
    v[i] = (uint8_t) s[i];
  return v;
}

// The 18 data points a GTD65GBSJ3WS answers (scan of 2026-11-22). Values are plausible idle values.
static Board gtd65() {
  Board b{0x2B, {}, true};
  b.erds[0x0001] = text32("GTD65GBSJ3WS");
  b.erds[0x0002] = text32("AB123456C");
  b.erds[0x0008] = {0x01};
  b.erds[0x2000] = {0x00};
  b.erds[0x2001] = {0x00};
  b.erds[0x2002] = {0x00};
  b.erds[0x2007] = {0x02, 0x58};
  b.erds[0x200A] = {0x83};
  b.erds[0x200E] = {0x00};
  b.erds[0x2010] = {0x00, 0x00};
  b.erds[0x2012] = {0x01};
  b.erds[0x201B] = {0x00};
  b.erds[0x201C] = {0x01};
  b.erds[0x201D] = {0x00};
  b.erds[0x2038] = {0x00, 0x00};
  b.erds[0x2039] = {0xFF};
  b.erds[0x2040] = {0xFF};
  b.erds[0x2041] = {0xFF};
  return b;
}

struct FakeEntity : EntityBase, gea::GEAEntity {
  std::vector<uint8_t> last;
  int updates{0};
  void on_erd_data(const std::vector<uint8_t> &data) override {
    last = data;
    updates++;
  }
};

struct FeatureDef {
  uint16_t erd;
  const char *label;
  bool control;
  bool polled;         // false for the Stop button, which is written, never read
  uint16_t write_erd;  // write_check: in dryer-link.yaml
};
// Must match the features: list in dryer-link.yaml (tools/check_features.py compares the two).
static const FeatureDef FEATURES[] = {
    {0x2000, "What the dryer is doing", false, true, 0},
    {0x2001, "Stage of the cycle", false, true, 0},
    {0x2007, "Time remaining", false, true, 0},
    {0x200A, "Selected cycle", false, true, 0},
    {0x201C, "Sensor or timed dry", false, true, 0},
    {0x204D, "Dryness level", false, true, 0},
    {0x2050, "Heat level", false, true, 0},
    {0x2012, "Door open or closed", false, true, 0},
    {0x2002, "Cycle finished", false, true, 0},
    {0x2010, "Delay start", false, true, 0},
    {0x2003, "Number of cycles run", false, true, 0},
    {0x2047, "Damp alert", false, true, 0},
    {0x206F, "Blocked vent warning", false, true, 0},
    {0x0001, "Model and serial number", false, true, 0},
    {0x0002, "Model and serial number", false, true, 0},
    {0x2040, "Stop", true, false, 0},
    {0x201B, "Extended Tumble", true, true, 0x201B},
    {0x2053, "Extended Tumble", true, true, 0x2051},
};

// ESPHome's update entity, as far as dryer_link is concerned: a state, what is offered, and "install it".
struct FakeUpdate : update::UpdateEntity {
  int performs{0};
  bool takes{true};  // false: the install never becomes the running firmware (download fails, or it rolls back)
  void perform(bool force) override { performs++; }
  void check() override {}
  void offer(const char *version, const char *md5) {
    state_ = update::UPDATE_STATE_AVAILABLE;
    update_info_.latest_version = version;
    update_info_.md5 = md5;
  }
  void nothing_offered() { state_ = update::UPDATE_STATE_NO_UPDATE; }
  void unknown() { state_ = update::UPDATE_STATE_UNKNOWN; }
};

struct World {
  ClassicBus classic;
  NewBus newbus;
};

struct Module {
  std::unique_ptr<gea::GEAComponent> hub;
  std::unique_ptr<dryer_link::DryerLink> dl;
  std::unique_ptr<web_server_base::WebServerBase> base;
  std::vector<std::unique_ptr<FakeEntity>> entities;
  World *world{nullptr};
  int boots{0};
  FakeUpdate update;              // survives restarts, like the release address does
  // How the firmware was built. The defaults are the release build (dryer-link.yaml).
  bool setup_on_network{false};
  bool status_page_login{true};
  bool auto_update{true};
  bool with_update{true};

  // Power-up. The flash, the Wi-Fi details and the pretend dryers survive; everything else starts again.
  void boot(World &w) {
    world = &w;
    boots++;
    g_ms = 0;
    g_deferred.clear();
    w.classic.uart.rx.clear();
    w.classic.uart.last_due = 0;
    w.classic.wire_free_at = 0;
    w.classic.last_poll = 0;
    w.newbus.uart.rx.clear();
    w.newbus.uart.last_due = 0;
    App.reboot_requested = false;
    App.setup_complete = false;
    auto *wifi = wifi::global_wifi_component;
    wifi->connected = false;
    wifi->attempt_started = 0;
    wifi->ap = wifi::WiFiAP();  // the configuration's hotspot has no password
    captive_portal::global_captive_portal->active = !wifi->has_sta();

    entities.clear();
    hub = std::make_unique<gea::GEAComponent>();
    dl = std::make_unique<dryer_link::DryerLink>();
    base = std::make_unique<web_server_base::WebServerBase>();
    // What ESPHome's generated code does, in the same order.
    hub->set_uart_parent(&w.classic.uart);
    hub->set_protocol(gea::Protocol::GEA2);
    hub->set_src_address(0xBB);
    hub->set_poll_interval(5000);
    hub->add_poll_override(0x2000, 2000);
    hub->add_poll_override(0x2012, 2000);
    hub->add_poll_override(0x0001, 600000);
    hub->add_poll_override(0x0002, 600000);
    hub->set_autostart(false);
    dl->set_hub(hub.get());
    dl->set_web_server_base(base.get());
    dl->set_classic_uart(&w.classic.uart);
    dl->set_new_uart(&w.newbus.uart);
    dl->set_version("0.1.0-sim");
    dl->set_setup_on_network(setup_on_network);
    dl->set_status_page_login(status_page_login);
    dl->set_auto_update(auto_update);
    if (with_update)
      dl->set_update(&update);
    for (const auto &f : FEATURES) {
      entities.push_back(std::make_unique<FakeEntity>());
      entities.back()->set_erd(f.erd);
      if (f.polled)
        hub->register_entity(entities.back().get());
      dl->add_feature(f.erd, f.label, f.control, f.write_erd);
      dl->add_feature_entity(entities.back().get());
    }
    // Setup order on the module: the hub (bus priority), then dryer_link, then Wi-Fi, which starts the hotspot
    // with whatever password dryer_link has put in place by then.
    hub->setup();
    dl->setup();
    App.setup_complete = true;
  }

  FakeEntity *entity(uint16_t erd) {
    for (size_t i = 0; i < entities.size(); i++) {
      if (FEATURES[i].erd == erd)
        return entities[i].get();
    }
    return nullptr;
  }

  // One pass of the main loop, `step` milliseconds after the previous one.
  void tick(uint32_t step = 1) {
    g_ms += step;
    auto *wifi = wifi::global_wifi_component;
    auto *portal = captive_portal::global_captive_portal;
    if (wifi->has_sta() && !wifi->connected && g_ms - wifi->attempt_started > 2500 && wifi->password != "wrongpass") {
      wifi->connected = true;
      portal->active = false;  // ESPHome drops the hotspot as soon as the module is on Wi-Fi
    }
    if (wifi->has_sta() && !wifi->connected && g_ms - wifi->attempt_started > 60000)
      portal->active = true;  // fallback hotspot
    world->classic.tick();
    hub->loop();
    dl->loop();
    auto deferred = std::move(g_deferred);
    g_deferred.clear();
    for (auto &f : deferred)
      f();
    if (App.reboot_requested)
      this->boot(*world);
  }

  void run(uint32_t ms, uint32_t step = 1) {
    for (uint32_t t = 0; t < ms; t += step)
      this->tick(step);
  }

  // A request as the web server would hand it over. Returns false when the module would not handle it.
  bool http(const std::string &path_query, AsyncWebServerRequest &req) {
    size_t q = path_query.find('?');
    req.url_ = path_query.substr(0, q);
    if (q != std::string::npos) {
      std::string rest = path_query.substr(q + 1);
      size_t pos = 0;
      while (pos < rest.size()) {
        size_t amp = rest.find('&', pos);
        std::string pair = rest.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = pair.find('=');
        std::string value = eq == std::string::npos ? "" : pair.substr(eq + 1), decoded;
        for (size_t i = 0; i < value.size(); i++) {
          if (value[i] == '%' && i + 2 < value.size() + 0 && isxdigit(value[i + 1]) && isxdigit(value[i + 2])) {
            decoded += (char) strtol(value.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
          } else {
            decoded += value[i] == '+' ? ' ' : value[i];
          }
        }
        req.args_[pair.substr(0, eq)] = decoded;
        pos = amp == std::string::npos ? rest.size() : amp + 1;
      }
    }
    for (auto *h : base->handlers) {
      if (h->canHandle(&req)) {
        h->handleRequest(&req);
        return true;
      }
    }
    return false;
  }
  std::string get(const std::string &path_query) {
    AsyncWebServerRequest req;
    return this->http(path_query, req) ? req.response.body : std::string("<not handled>");
  }
  // What the status says, whether or not the module would serve it to anyone right now. For the tests' own use.
  std::string status() {
    AsyncWebServerRequest req;
    req.url_ = "/dl/status";
    dl->handleRequest(&req);
    return req.response.body;
  }
  std::string report() {
    AsyncWebServerRequest req;
    req.url_ = "/dl/report";
    dl->handleRequest(&req);
    return req.response.body;
  }
};

// =====================================================================================================================
// Scripted scenarios
// =====================================================================================================================

static int g_checks = 0, g_failed = 0;
static void expect(bool ok, const char *what, const std::string &detail = "") {
  g_checks++;
  if (!ok) {
    g_failed++;
    fprintf(stderr, "  FAILED: %s%s%s\n", what, detail.empty() ? "" : "\n     got: ", detail.c_str());
  }
}
static bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }
static void expect_has(const std::string &hay, const char *needle) { expect(has(hay, needle), needle, hay); }
static void section(const char *name) { fprintf(stderr, "%s\n", name); }

// Run until the status contains `needle`, at most `limit_ms`. Returns the time it took, or -1.
static long until(Module &m, const char *needle, uint32_t limit_ms, uint32_t step = 1) {
  const uint32_t boots = m.boots;
  long waited = 0;
  while (waited < (long) limit_ms) {
    if (has(m.status(), needle))
      return waited;
    m.run(100, step);
    waited += 100;
    if (m.boots != (int) boots)
      return -2;
  }
  return has(m.status(), needle) ? waited : -1;
}

static void fresh() {
  sim_flash().clear();
  sim_flash_fails = false;
  set_label(nullptr);
  wifi::global_wifi_component->ssid.clear();
  wifi::global_wifi_component->password.clear();
  g_log.clear();
}

static int run_tests() {
  // ------------------------------------------------------------------------------------------------------------------
  for (uint32_t step : {1u, 7u, 16u}) {
    fprintf(stderr, "A. a dryer like the GTD65, main loop every %u ms\n", step);
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    expect_has(m.status(), "\"hotspot\":true");
    long t = until(m, "\"state\":\"found\"", 5000, step);
    expect(t >= 0 && t <= 2000, "found within 2 s", std::to_string(t));
    std::string s = m.status();
    expect_has(s, "\"bus\":{\"state\":\"found\",\"lang\":\"classic\",\"address\":\"0x2B\"}");
    expect_has(s, "\"dryer\":{\"type\":\"Clothes Dryer\",\"model\":\"GTD65GBSJ3WS\",\"serial\":\"AB123456C\"}");
    expect_has(s, "\"learn\":{\"state\":\"idle\"");
    expect_has(s, "\"caps\":{\"door\":true,\"power\":true}");
    expect_has(m.get("/dl/report"), "0x2000 ?   What the dryer is doing");  // nothing is claimed before learning
    // Not learned and somebody may be at the page: nothing is polled and nothing starts by itself.
    w.classic.reads.clear();
    m.run(20000, step);
    expect(w.classic.reads[0x2000] == 0 && w.classic.reads[0x204D] == 0, "no polling before learning");
    expect_has(m.status(), "\"learn\":{\"state\":\"idle\"");
    expect_has(m.status(), "\"state\":\"found\"");

    // Quick check.
    expect(m.get("/dl/learn?mode=quick") == "{}", "learn accepted");
    expect_has(m.status(), "\"learn\":{\"state\":\"running\",\"mode\":\"quick\"");
    m.run(300, step);
    s = m.status();
    expect_has(s, "\"total\":517");
    t = until(m, "\"learn\":{\"state\":\"done\"", 400000, step);
    fprintf(stderr, "   quick check took %.0f s of dryer time\n", t / 1000.0);
    expect(t > 60000 && t < 200000, "quick check takes between 1 and 3.3 minutes", std::to_string(t));
    s = m.status();
    expect_has(s, "\"can\":[\"What the dryer is doing\",\"Stage of the cycle\",\"Time remaining\",\"Selected cycle\",\"Sensor or timed dry\","
                  "\"Door open or closed\",\"Cycle finished\",\"Delay start\",\"Model and serial number\"]");
    expect_has(s, "\"controls\":[\"Stop\",\"Extended Tumble\"]");
    expect_has(s, "\"skipped\":6,\"unknown\":5}");
    std::string r = m.get("/dl/report");
    expect_has(r, "codes that answered: 18");
    expect_has(r, "0x2039=FF");
    expect_has(r, "0x204D no  Dryness level");
    expect_has(r, "model: GTD65GBSJ3WS");

    // After learning, what the dryer has is polled and what it lacks is left alone.
    w.classic.reads.clear();
    int before = m.entity(0x2000)->updates;
    m.run(30000, step);
    expect(w.classic.reads[0x2000] >= 12, "machine state polled about every 2 s", std::to_string(w.classic.reads[0x2000]));
    expect(w.classic.reads[0x2007] >= 5, "remaining time polled about every 5 s", std::to_string(w.classic.reads[0x2007]));
    expect(w.classic.reads[0x204D] == 0 && w.classic.reads[0x2050] == 0, "absent data points are not polled");
    expect(m.entity(0x2000)->updates > before, "entities receive values");
    // The one write learning makes: Extended Tumble's own value, back to where it came from.
    expect(w.classic.written.size() == 1 && w.classic.written[0].first == 0x201B && w.classic.written[0].second == std::vector<uint8_t>{0x00},
           "the only write is Extended Tumble's unchanged value", std::to_string(w.classic.written.size()));
    expect(w.classic.boards[0].erds[0x201B] == std::vector<uint8_t>{0x00}, "and it changed nothing");

    // Door check.
    m.get("/dl/check?what=door");
    expect_has(m.status(), "\"check\":{\"what\":\"door\",\"phase\":\"wait1\"}");
    m.run(3000, step);
    expect_has(m.status(), "\"phase\":\"wait1\"");
    w.classic.boards[0].erds[0x2012] = {0x00};
    t = until(m, "\"phase\":\"wait2\"", 3000, step);
    expect(t >= 0 && t <= 1500, "door seen open within 1.5 s", std::to_string(t));
    expect_has(m.status(), "\"door\":\"open\"");
    w.classic.boards[0].erds[0x2012] = {0x01};
    t = until(m, "\"check\":{\"what\":\"door\",\"phase\":\"ok\"}", 3000, step);
    expect(t >= 0 && t <= 1500, "door seen closed within 1.5 s", std::to_string(t));

    // Power check.
    m.get("/dl/check?what=power");
    expect_has(m.status(), "\"check\":{\"what\":\"power\",\"phase\":\"wait1\"}");
    m.run(2000, step);
    w.classic.boards[0].erds[0x2000] = {0x01};
    t = until(m, "\"check\":{\"what\":\"power\",\"phase\":\"ok\"}", 3000, step);
    expect(t >= 0 && t <= 1500, "power press seen within 1.5 s", std::to_string(t));
    m.get("/dl/check?what=none");
    expect_has(m.status(), "\"check\":{\"what\":\"none\",\"phase\":\"idle\"}");

    // Wi-Fi, wrong password first.
    expect(has(m.get("/"), "\x1f\x8b"), "the hotspot serves the page (gzip) at /");
    expect(has(m.get("/hotspot-detect.html"), "\x1f\x8b"), "and at the address phones probe");
    expect(m.get("/config.json") == "<not handled>", "the network list is left to ESPHome's captive portal");
    expect(m.get("/dl/nonsense") == "Not found", "unknown API address");
    m.get("/wifisave?ssid=Maple%20House&psk=wrongpass");
    m.run(50, step);
    expect_has(m.status(), "\"wifi\":{\"state\":\"connecting\",\"ssid\":\"Maple House\"}");
    t = until(m, "\"wifi\":{\"state\":\"fail\"", 40000, step);
    expect(t > 25000 && t <= 32000, "wrong password reported after about 30 s", std::to_string(t));
    expect(m.boots == 1, "no restart while still on the hotspot");
    m.get("/wifisave?ssid=Maple+House&psk=correct%26horse");
    m.run(50, step);
    expect(wifi::global_wifi_component->password == "correct&horse", "password passed on unchanged", wifi::global_wifi_component->password);
    t = until(m, "\"wifi\":{\"state\":\"ok\"", 5000, step);
    expect(t >= 0, "joined");
    // On Wi-Fi the hidden set (two absent features) differs from what start-up applied: one restart, then none.
    expect(m.get("/") == "<not handled>", "off the hotspot, / belongs to the status page");
    expect(m.get("/wifisave?ssid=x&psk=12345678") == "<not handled>", "off the hotspot, Wi-Fi details cannot be changed");
    expect(m.get("/setup") == "<not handled>" && m.get("/dl/status") == "<not handled>", "off the hotspot, the walkthrough and its API are gone");
    m.run(8000, step);
    expect(m.boots == 2, "restarted once to apply what was learned", std::to_string(m.boots));
    expect(m.entity(0x204D)->is_internal() && m.entity(0x2050)->is_internal(), "absent features hidden after the restart");
    int hidden = 0;
    for (auto &e : m.entities)
      hidden += e->is_internal() ? 1 : 0;
    expect(hidden == 6, "nothing else hidden", std::to_string(hidden));
    t = until(m, "\"state\":\"found\"", 5000, step);
    expect(t >= 0, "found again after the restart");
    s = m.status();
    expect_has(s, "\"learn\":{\"state\":\"done\",\"mode\":\"quick\"");
    expect_has(s, "\"skipped\":6,\"unknown\":5}");
    expect_has(s, "\"hotspot\":false");
    w.classic.reads.clear();
    m.run(40000, step);
    expect(m.boots == 2, "no second restart", std::to_string(m.boots));
    expect(w.classic.reads[0x2000] >= 15 && w.classic.reads[0x204D] == 0, "polling resumes from what was saved");
    expect(w.classic.reads.size() <= 12, "only known-present data points are asked for", std::to_string(w.classic.reads.size()));
    expect(w.classic.writes == 1, "nothing more written", std::to_string(w.classic.writes));
    expect(g_faults.empty(), "no faults");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("B. nothing connected, then the cable goes in");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    w.classic.cable_in = false;
    Module m;
    m.boot(w);
    m.run(5000);
    expect_has(m.status(), "\"bus\":{\"state\":\"searching\",\"lang\":\"\",\"address\":\"\"}");
    long t = until(m, "\"state\":\"none\"", 30000);
    expect(t >= 0 && t <= 21000, "says it can't hear the dryer within about 25 s", std::to_string(t));
    expect(m.get("/dl/learn?mode=quick") == "{}" && has(m.status(), "\"learn\":{\"state\":\"idle\""), "learning refused without a dryer");
    m.get("/dl/check?what=door");
    expect_has(m.status(), "\"check\":{\"what\":\"door\",\"phase\":\"timeout\"}");
    m.get("/dl/check?what=none");
    w.classic.cable_in = true;
    t = until(m, "\"state\":\"found\"", 20000);
    expect(t >= 0 && t <= 10000, "found within 10 s of the cable going in, without asking", std::to_string(t));
    m.get("/dl/find");
    expect_has(m.status(), "\"state\":\"searching\"");
    t = until(m, "\"state\":\"found\"", 5000);
    expect(t >= 0, "found again when asked to look again");
    expect(g_faults.empty(), "no faults");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("C. a washer");
  {
    fresh();
    World w;
    Board b = gtd65();
    b.addr = 0x23;
    b.erds[0x0008] = {0x02};
    b.erds[0x0001] = text32("GTW685BSL2WS");
    w.classic.boards.push_back(b);
    Module m;
    m.boot(w);
    long t = until(m, "\"state\":\"unsupported\"", 5000);
    expect(t >= 0, "recognised as not a dryer");
    expect_has(m.status(), "\"dryer\":{\"type\":\"Clothes Washer\",\"model\":\"GTW685BSL2WS\"");
    m.get("/dl/learn?mode=quick");
    expect_has(m.status(), "\"learn\":{\"state\":\"idle\"");
    w.classic.reads.clear();
    m.run(30000);
    expect(w.classic.reads.empty(), "a washer is left alone", std::to_string(w.classic.reads.size()));
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("D. a dryer that does not say what type it is");
  {
    fresh();
    World w;
    Board b = gtd65();
    b.addr = 0x24;
    b.erds.erase(0x0008);
    b.erds.erase(0x2012);  // and has no door switch on the bus
    w.classic.boards.push_back(b);
    Module m;
    m.boot(w);
    long t = until(m, "\"state\":\"found\"", 12000);
    expect(t >= 0, "found through its model number");
    expect_has(m.status(), "\"address\":\"0x24\"");
    expect_has(m.status(), "\"dryer\":{\"type\":\"Dryer\",\"model\":\"GTD65GBSJ3WS\"");
    m.run(70000);
    expect_has(m.status(), "\"state\":\"found\"");  // the keep-alive works without the type
    m.get("/dl/learn?mode=quick");
    t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
    expect(t >= 0, "learned");
    expect_has(m.status(), "\"caps\":{\"door\":false,\"power\":true}");
    expect(!has(m.status(), "Door open or closed"), "door not listed");
    expect_has(m.status(), "\"skipped\":7,");
    m.get("/dl/check?what=door");
    expect_has(m.status(), "\"check\":{\"what\":\"door\",\"phase\":\"timeout\"}");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("E. two boards on the bus");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Board second{0x24, {}, false};
    second.erds[0x0001] = text32("UIBOARD");
    w.classic.boards.push_back(second);
    Module m;
    m.boot(w);
    long t = until(m, "\"state\":\"found\"", 12000);
    expect(t >= 0, "the board that hosts the appliance type is chosen");
    expect_has(m.status(), "\"address\":\"0x2B\"");
    // Neither says its type: nothing tells them apart.
    fresh();
    w.classic.boards[0].erds.erase(0x0008);
    m.boot(w);
    t = until(m, "\"state\":\"unsupported\"", 12000);
    expect(t >= 0, "two indistinguishable boards are reported, not guessed");
    expect_has(m.status(), "more than one control board");
    expect_has(m.get("/dl/report"), "boards that answered: 0x2B 0x24");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("F. the cable comes out during the quick check");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=quick");
    m.run(20000);
    expect_has(m.status(), "\"learn\":{\"state\":\"running\"");
    w.classic.cable_in = false;
    long t = until(m, "\"learn\":{\"state\":\"idle\"", 60000);
    expect(t >= 0 && t <= 40000, "the check stops instead of recording a dryer with nothing", std::to_string(t));
    t = until(m, "\"state\":\"none\"", 40000);
    expect(t >= 0, "and the page is told the dryer is gone");
    expect(sim_flash().empty(), "nothing was saved");
    w.classic.cable_in = true;
    t = until(m, "\"state\":\"found\"", 20000);
    expect(t >= 0, "found again");
    m.get("/dl/learn?mode=quick");
    t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
    expect(t >= 0, "learned on the second try");
    expect_has(m.status(), "\"skipped\":6,\"unknown\":5}");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("G. a door that never reports, and the owner carries on");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=quick");
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    m.get("/dl/check?what=door");
    long t = until(m, "\"check\":{\"what\":\"door\",\"phase\":\"timeout\"}", 60000);
    expect(t > 40000 && t <= 47000, "gives up waiting for the door after about 45 s", std::to_string(t));
    m.get("/dl/check?what=door");  // "Try once more"
    expect_has(m.status(), "\"phase\":\"wait1\"");
    until(m, "\"phase\":\"timeout\"", 60000);
    m.get("/dl/check?what=power");  // "Carry on without it"
    m.run(200);
    expect(!has(m.status(), "Door open or closed"), "door no longer listed");
    expect_has(m.status(), "\"skipped\":7,");
    expect_has(m.get("/dl/report"), "door check: no movement seen, door hidden");
    m.get("/dl/check?what=none");
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(12000);
    expect(m.boots == 2, "restart after joining Wi-Fi", std::to_string(m.boots));
    expect(m.entity(0x2012)->is_internal(), "door entity hidden");
    // Skipping the check (no timeout) hides nothing.
    fresh();
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=quick");
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    m.get("/dl/check?what=door");
    m.run(5000);
    m.get("/dl/check?what=power");  // "Skip this check"
    m.run(200);
    expect_has(m.status(), "Door open or closed");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("H. a dryer that speaks the new language");
  {
    fresh();
    World w;
    w.newbus.present = true;
    w.newbus.erds = gtd65().erds;
    w.newbus.erds[0x0001] = text32("PTD60GBSRWS");
    w.newbus.erds[0x204D] = {0x03};
    w.newbus.erds.erase(0x201B);
    Module m;
    m.boot(w);
    long t = until(m, "\"state\":\"found\"", 12000);
    expect(t >= 0 && t <= 8000, "found on the new bus", std::to_string(t));
    expect_has(m.status(), "\"bus\":{\"state\":\"found\",\"lang\":\"new\",\"address\":\"0xC0\"}");
    expect_has(m.status(), "\"model\":\"PTD60GBSRWS\"");
    m.run(70000);
    expect_has(m.status(), "\"state\":\"found\"");
    m.get("/dl/learn?mode=quick");
    t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
    fprintf(stderr, "   quick check took %.0f s (the pretend appliance refuses unknown codes at once)\n", t / 1000.0);
    expect(t >= 0, "learned");
    std::string s = m.status();
    expect_has(s, "\"Dryness level\"");
    expect_has(s, "\"controls\":[\"Stop\"]");
    expect_has(s, "\"skipped\":6,");
    expect(!w.newbus.asked_from.empty() && std::count(w.newbus.asked_from.begin(), w.newbus.asked_from.end(), 0xE4) == (long) w.newbus.asked_from.size(),
           "on the new bus the module speaks from GE's adapter address, 0xE4");
    expect(w.newbus.written.empty(), "nothing written");
    m.get("/dl/check?what=door");
    w.newbus.erds[0x2012] = {0x00};
    t = until(m, "\"phase\":\"wait2\"", 3000);
    expect(t >= 0, "door check works by asking");
    // After a restart the new bus is tried first.
    m.get("/dl/check?what=none");
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(12000);
    expect(m.boots == 2, "restart after joining Wi-Fi", std::to_string(m.boots));
    t = until(m, "\"state\":\"found\"", 12000);
    expect(t >= 0 && t <= 2000, "found at once on the bus it used before", std::to_string(t));
    expect(m.entity(0x201B)->is_internal() && m.entity(0x2050)->is_internal() && !m.entity(0x204D)->is_internal(), "entities fit this dryer");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("I. a collision on the wire during the scan");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    w.classic.corrupt_reads_of = 0x2039;  // asked once only; a lost attempt must not read as "absent"
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=quick");
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    expect(w.classic.corrupt_reads_of == -1, "the collision happened");
    expect_has(m.get("/dl/report"), "0x2039=FF");
    expect_has(m.status(), "\"unknown\":5}");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("J. a learned module moved to a different dryer");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=quick");
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(12000);
    expect(m.boots == 2, "set up", std::to_string(m.boots));
    // Different model: has a dryness level, has no extended tumble.
    w.classic.boards[0].erds[0x0001] = text32("GFD55ESSN0WW");
    w.classic.boards[0].erds[0x204D] = {0x03};
    w.classic.boards[0].erds.erase(0x201B);
    m.boot(w);
    long t = until(m, "\"learn\":{\"state\":\"running\"", 25000);
    expect(t >= 14000 && t <= 17000, "learns the new dryer without being asked, about 15 s after power-on", std::to_string(t));
    m.run(400000);
    expect(m.boots == 4, "and restarts once to apply it", std::to_string(m.boots));
    until(m, "\"state\":\"found\"", 5000);
    expect(!m.entity(0x204D)->is_internal() && m.entity(0x201B)->is_internal(), "entities now fit the new dryer");
    std::string s = m.status();
    expect_has(s, "\"learn\":{\"state\":\"done\"");
    expect_has(s, "\"controls\":[\"Stop\"]");
    m.run(400000);
    expect(m.boots == 4, "no further restarts", std::to_string(m.boots));
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("K. flash that cannot be written");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    sim_flash_fails = true;
    m.get("/dl/learn?mode=quick");
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(60000);
    expect(m.boots == 1, "no restart loop when the result cannot be saved", std::to_string(m.boots));
    sim_flash_fails = false;
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("L. full scan");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    w.classic.boards[0].erds[0x9123] = {0xAB, 0xCD};  // something outside the common and laundry blocks
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=full");
    m.run(300);
    expect_has(m.status(), "\"learn\":{\"state\":\"running\",\"mode\":\"full\"");
    std::string s = m.status();
    size_t p = s.find("\"total\":");
    int total = p == std::string::npos ? 0 : atoi(s.c_str() + p + 8);
    expect(total == 2449, "every known code is asked for", std::to_string(total));
    long t = until(m, "\"learn\":{\"state\":\"done\"", 1500000);
    fprintf(stderr, "   full scan took %.1f min of dryer time\n", t / 60000.0);
    expect(t > 480000 && t < 840000, "full scan takes between 8 and 14 minutes", std::to_string(t));
    expect_has(m.get("/dl/report"), "learned: yes, full scan");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("M. the module's own setup password");
  {
    auto *wifi = wifi::global_wifi_component;
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    expect(wifi->ap.password_.empty(), "no label: the hotspot is open");
    expect(m.base->auth_username_ == nullptr, "no label: the status page asks for no login");
    expect_has(m.status(), "\"secured\":false,\"login\":false");
    expect_has(m.report(), "setup password on this module: none, hotspot is open");

    set_label("kq7m-x2hd-9pwc");
    m.boot(w);
    expect(wifi->ap.password_ == "kq7m-x2hd-9pwc", "label: the hotspot needs the label password", wifi->ap.password_);
    expect(m.base->auth_username_ != nullptr && std::string(m.base->auth_username_) == "admin" && m.base->auth_password_ != nullptr &&
               std::string(m.base->auth_password_) == "kq7m-x2hd-9pwc",
           "label: the status page asks for admin and the label password");
    expect_has(m.status(), "\"secured\":true,\"login\":true");
    expect_has(m.report(), "setup password on this module: yes");
    expect(!has(m.status(), "kq7m") && !has(m.report(), "kq7m"), "the password itself is never served");

    m.status_page_login = false;  // as in the test build
    m.boot(w);
    expect(wifi->ap.password_ == "kq7m-x2hd-9pwc" && m.base->auth_username_ == nullptr, "login switched off: hotspot still locked, page open");
    expect_has(m.status(), "\"secured\":true,\"login\":false");
    m.status_page_login = true;

    // The longest and shortest passwords Wi-Fi allows.
    const std::string longest(63, 'x');
    set_label(longest.c_str());
    m.boot(w);
    expect(wifi->ap.password_ == longest, "63 characters accepted");
    set_label("abcd2345");
    m.boot(w);
    expect(wifi->ap.password_ == "abcd2345", "8 characters accepted");

    // A label that is damaged or not a label is treated as no label, never as a password.
    struct Bad {
      const char *what;
      std::function<void()> make;
    };
    const Bad bad[] = {
        {"one byte of the password flipped", [] { set_label("kq7m-x2hd-9pwc"); g_label[7] ^= 0x01; }},
        {"checksum byte flipped", [] { set_label("kq7m-x2hd-9pwc"); g_label[5 + 14] ^= 0x80; }},
        {"too short for Wi-Fi (7 characters)", [] { set_label("abc2345"); }},
        {"a space in it", [] { set_label("kq7m x2hd 9pwc"); }},
        {"wrong first bytes", [] { set_label("kq7m-x2hd-9pwc"); g_label[0] = 'X'; }},
        {"length byte larger than the partition read", [] { set_label("kq7m-x2hd-9pwc"); g_label[4] = 200; }},
        {"all zeroes", [] { g_label.assign(80, 0x00); }},
        {"erased flash", [] { g_label.assign(80, 0xFF); }},
    };
    for (const auto &b : bad) {
      b.make();
      m.boot(w);
      expect(wifi->ap.password_.empty() && m.base->auth_username_ == nullptr, b.what, wifi->ap.password_);
    }
    set_label(nullptr);
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("N. what is served where");
  {
    const char *const PATHS[] = {"/", "/setup", "/dl/status", "/dl/report", "/dl/find", "/dl/learn?mode=full", "/dl/check?what=door",
                                 "/wifisave?ssid=Other&psk=12345678", "/hotspot-detect.html", "/generate_204"};
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    expect_has(m.get("/dl/status"), "\"hotspot\":true");
    AsyncWebServerRequest post;
    post.method_ = HTTP_POST;
    expect(!m.http("/dl/learn?mode=full", post), "only GET requests are taken");
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(5000);
    expect(!captive_portal::global_captive_portal->active && wifi::global_wifi_component->connected, "joined, hotspot closed");
    // Release build: on the home network the module serves nothing of its own. What is left is ESPHome's status page.
    for (const char *path : PATHS)
      expect(m.get(path) == "<not handled>", path, m.get(path));
    m.run(2000);
    expect_has(m.status(), "\"learn\":{\"state\":\"idle\"");  // the refused requests started nothing
    expect(wifi::global_wifi_component->ssid == "Home", "and changed nothing");

    // Test build: the walkthrough and its API also answer on the home network. Wi-Fi details still cannot be changed.
    fresh();
    m.setup_on_network = true;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(5000);
    expect_has(m.get("/dl/status"), "\"hotspot\":false");
    expect(has(m.get("/setup"), "\x1f\x8b"), "test build: /setup serves the walkthrough");
    expect(m.get("/") == "<not handled>" && m.get("/wifisave?ssid=Other&psk=12345678") == "<not handled>" && m.get("/generate_204") == "<not handled>",
           "test build: nothing else is taken over");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("O. a dryer like the GFD48GSSK0WW: 11 data points, answers no write");
  for (int ignores_broadcast = 0; ignores_broadcast < 2; ignores_broadcast++) {
    fresh();
    World w;
    Board b{0x24, {}, false};
    b.erds[0x0001] = text32("GFD48GSSK0WW");
    b.erds[0x0002] = text32("ZZ654321G");
    b.erds[0x2000] = {0x00};
    b.erds[0x2001] = {0x00};
    b.erds[0x2002] = {0x00};
    b.erds[0x2007] = {0x00, 0x00};
    b.erds[0x200A] = {0x89};
    b.erds[0x2010] = {0x00, 0x00};
    b.erds[0x2012] = {0x01};
    b.erds[0x201B] = {0x01};
    b.erds[0x201C] = {0x01};
    b.accepts_writes = false;
    b.answers_broadcast = ignores_broadcast == 0;
    w.classic.boards.push_back(b);
    Module m;
    m.boot(w);
    long t = until(m, "\"state\":\"found\"", 20000);
    fprintf(stderr, "   found after %.1f s (%s)\n", t / 1000.0, ignores_broadcast ? "board ignores broadcasts" : "board answers broadcasts");
    expect(t >= 0 && t <= (ignores_broadcast ? 10000 : 4000), "found", std::to_string(t));
    expect_has(m.status(), "\"bus\":{\"state\":\"found\",\"lang\":\"classic\",\"address\":\"0x24\"}");
    expect_has(m.status(), "\"dryer\":{\"type\":\"Dryer\",\"model\":\"GFD48GSSK0WW\"");
    m.get("/dl/learn?mode=quick");
    t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
    expect(t >= 0, "learned");
    std::string s = m.status();
    expect_has(s, "\"can\":[\"What the dryer is doing\",\"Stage of the cycle\",\"Time remaining\",\"Selected cycle\",\"Sensor or timed dry\","
                  "\"Door open or closed\",\"Cycle finished\",\"Delay start\",\"Model and serial number\"]");
    expect_has(s, "\"controls\":[]");  // Stop has no data point here, and Extended Tumble can be read but not set
    expect_has(m.report(), "0x201B no  Extended Tumble");
    expect_has(m.report(), "0x201B=01");  // still in the report: it answers a read
    bool only_same = !w.classic.written.empty();
    for (const auto &wr : w.classic.written)
      only_same = only_same && wr.first == 0x201B && wr.second == std::vector<uint8_t>{0x01};
    expect(only_same && w.classic.written.size() <= 3, "the write check only ever sent the value the option already had",
           std::to_string(w.classic.written.size()));
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(12000);
    expect(m.boots == 2 && m.entity(0x201B)->is_internal() && m.entity(0x2040)->is_internal() && !m.entity(0x2012)->is_internal(),
           "the switch and the Stop button are hidden, the sensors stay");
    w.classic.written.clear();
    m.run(120000);
    expect(w.classic.written.empty(), "and nothing is written afterwards");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("P. an older dryer as GE documents them (address 0x24, no appliance type, no door on the bus)");
  {
    fresh();
    World w;
    Board b{0x24, {}, false};
    b.erds[0x0001] = text32("GTDP490ED7WS");
    b.erds[0x0002] = text32("HH112233P");
    b.erds[0x2000] = {0x01};
    b.erds[0x2001] = {0x00};
    b.erds[0x2002] = {0x00};
    b.erds[0x2003] = {0x01, 0x2C};
    b.erds[0x2004] = {0x00, 0x00, 0x00, 0x00};
    b.erds[0x2005] = {0x00};
    b.erds[0x2007] = {0x00, 0x00};
    b.erds[0x200A] = {0x80};
    b.erds[0x200E] = {0x00};
    b.erds[0x200F] = {0x00};
    b.erds[0x2010] = {0x00, 0x00};
    b.answers_broadcast = false;
    w.classic.boards.push_back(b);
    Module m;
    m.boot(w);
    long t = until(m, "\"state\":\"found\"", 20000);
    expect(t >= 0 && t <= 10000, "found by asking the known addresses one by one", std::to_string(t));
    expect_has(m.status(), "\"address\":\"0x24\"");
    m.get("/dl/learn?mode=quick");
    t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
    expect(t >= 0, "learned");
    std::string s = m.status();
    expect_has(s, "\"can\":[\"What the dryer is doing\",\"Stage of the cycle\",\"Time remaining\",\"Selected cycle\",\"Cycle finished\","
                  "\"Delay start\",\"Number of cycles run\",\"Model and serial number\"]");
    expect_has(s, "\"controls\":[]");
    expect_has(s, "\"caps\":{\"door\":false,\"power\":true}");
    expect_has(s, "\"unknown\":4}");  // 0x2004, 0x2005, 0x200E, 0x200F
    expect(w.classic.written.empty(), "nothing written: there is no option to check");
    // A found dryer that ignores broadcasts must also be found again after a restart, and stay found.
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(12000);
    expect(m.boots == 2, "restart after joining Wi-Fi", std::to_string(m.boots));
    t = until(m, "\"state\":\"found\"", 20000);
    expect(t >= 0 && t <= 10000, "found again after the restart", std::to_string(t));
    w.classic.reads.clear();
    m.run(180000);
    expect_has(m.status(), "\"state\":\"found\"");
    expect(w.classic.reads[0x2000] >= 60 && w.classic.reads[0x2003] >= 20, "and polled", std::to_string(w.classic.reads[0x2000]));
    expect(m.boots == 2, "no further restarts", std::to_string(m.boots));
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("Q. new-bus appliances that behave differently from the pretend one in H");
  {
    auto modern = [](World &w) {
      w.newbus.present = true;
      w.newbus.erds = gtd65().erds;
      w.newbus.erds[0x0001] = text32("PFD87ESPVRS");
      w.newbus.erds[0x204D] = {0x03};
      w.newbus.erds[0x2050] = {0x02};
      w.newbus.erds[0x2047] = {0x00};
      w.newbus.erds[0x206F] = {0x00};
      w.newbus.erds[0x2053] = {0x01};
      w.newbus.erds[0x2051] = {0xFF};
      w.newbus.erds[0x201B] = {0x01};
      w.newbus.writable = {0x201B, 0x2051, 0x2040};
    };
    // 1. Answers only at its own address, 0xC0, as GE's adapter assumes.
    fresh();
    {
      World w;
      modern(w);
      w.newbus.answers_broadcast = false;
      Module m;
      m.boot(w);
      long t = until(m, "\"state\":\"found\"", 20000);
      expect(t >= 0 && t <= 8000, "found when it only answers at 0xC0", std::to_string(t));
      m.get("/dl/learn?mode=quick");
      t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
      expect(t >= 0, "learned");
      std::string s = m.status();
      expect_has(s, "\"Dryness level\",\"Heat level\"");
      expect_has(s, "\"Damp alert\",\"Blocked vent warning\"");
      expect_has(s, "\"controls\":[\"Stop\",\"Extended Tumble\"]");
      expect_has(s, "\"skipped\":1,");  // only the cycle counter is missing
      // Both forms of the option were checked by writing back what they already had.
      bool ok = w.newbus.written.size() == 2;
      for (const auto &wr : w.newbus.written)
        ok = ok && (wr.first == 0x201B || wr.first == 0x2051) && wr.second == std::vector<uint8_t>{0x01};
      expect(ok, "the two write checks sent the option's own value", std::to_string(w.newbus.written.size()));
    }
    // 2. Not at 0xC0, but answers the broadcast address.
    fresh();
    {
      World w;
      modern(w);
      w.newbus.addr = 0xC2;
      Module m;
      m.boot(w);
      long t = until(m, "\"state\":\"found\"", 40000);
      expect(t >= 0 && t <= 22000, "found through the broadcast address on the second round", std::to_string(t));
      expect_has(m.status(), "\"lang\":\"new\",\"address\":\"0xC2\"");
    }
    // 3. Its newer option code cannot be written (nothing listens at 0x2051): that switch is left out, the old one stays.
    fresh();
    {
      World w;
      modern(w);
      w.newbus.writable = {0x201B, 0x2040};
      Module m;
      m.boot(w);
      until(m, "\"state\":\"found\"", 20000);
      m.get("/dl/learn?mode=quick");
      until(m, "\"learn\":{\"state\":\"done\"", 400000);
      expect_has(m.status(), "\"controls\":[\"Stop\",\"Extended Tumble\"]");
      expect_has(m.report(), "0x2053 no  Extended Tumble");
      expect_has(m.report(), "0x201B yes Extended Tumble");
    }
    // 4. Says "busy" to the first questions.
    fresh();
    {
      World w;
      modern(w);
      w.newbus.erds.erase(0x2053);
      w.newbus.erds.erase(0x2051);
      Module m;
      m.boot(w);
      until(m, "\"state\":\"found\"", 20000);
      w.newbus.busy_reads = 4;
      m.get("/dl/learn?mode=quick");
      long t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
      expect(t >= 0 && w.newbus.busy_reads == 0, "learned through the busy answers");
      expect_has(m.status(), "\"skipped\":2,");  // the cycle counter and the newer option code, nothing else
    }
    // 5. Refuses the subscription (GE: some appliances allow one subscriber and something else has it).
    fresh();
    {
      World w;
      modern(w);
      w.newbus.refuse_subscription = true;
      Module m;
      m.boot(w);
      long t = until(m, "\"state\":\"found\"", 20000);
      expect(t >= 0 && t <= 8000, "found although it will not publish", std::to_string(t));
      m.get("/dl/learn?mode=quick");
      t = until(m, "\"learn\":{\"state\":\"done\"", 400000);
      expect(t >= 0, "learned by asking");
      expect_has(m.status(), "\"controls\":[\"Stop\",\"Extended Tumble\"]");
      expect_has(m.status(), "\"skipped\":1,");
      m.get("/wifisave?ssid=Home&psk=12345678");
      m.run(12000);
      expect(m.boots == 2, "restart after joining Wi-Fi", std::to_string(m.boots));
      until(m, "\"state\":\"found\"", 20000);
      const int updates = m.entity(0x2000)->updates, subs = w.newbus.subscriptions;
      w.newbus.erds[0x2000] = {0x02};
      m.run(120000);
      expect(m.entity(0x2000)->updates > updates && m.entity(0x2000)->last == std::vector<uint8_t>{0x02}, "values still arrive, by polling");
      expect(w.newbus.subscriptions - subs >= 2 && w.newbus.subscriptions - subs <= 8, "and the subscription is asked for again now and then",
             std::to_string(w.newbus.subscriptions - subs));
      expect_has(m.status(), "\"state\":\"found\"");
      // When the other subscriber goes away, publications take over.
      w.newbus.refuse_subscription = false;
      m.run(60000);
      const int reads = w.newbus.reads;
      m.run(60000);
      expect(w.newbus.reads - reads <= 2, "polling stops once the subscription is accepted", std::to_string(w.newbus.reads - reads));
    }
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("R. the value written back is the one the option has at that moment");
  {
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    m.get("/dl/learn?mode=quick");
    m.run(30000);  // Extended Tumble was read in the first seconds...
    expect_has(m.status(), "\"learn\":{\"state\":\"running\"");
    w.classic.boards[0].erds[0x201B] = {0x01};  // ...and now someone switches it on at the dryer
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    expect(w.classic.written.size() == 1 && w.classic.written[0].second == std::vector<uint8_t>{0x01}, "the write check sent the new value, not the old one");
    expect(w.classic.boards[0].erds[0x201B] == std::vector<uint8_t>{0x01}, "the owner's choice was not undone");
  }

  // ------------------------------------------------------------------------------------------------------------------
  section("S. updating itself");
  {
    const uint32_t MIN = 60000, STEP = 20;
    const uint32_t TRIES_KEY = 0x44727975;
    auto tries = []() -> int {
      auto it = sim_flash().find(0x44727975);
      return it == sim_flash().end() || it->second.size() != 8 ? -1 : it->second[4];
    };
    fresh();
    World w;
    w.classic.boards.push_back(gtd65());
    Module m;
    m.boot(w);
    until(m, "\"state\":\"found\"", 5000);
    // Still on the hotspot, in setup: an offer must wait.
    m.update.offer("0.1.1", "0123456789abcdef0123456789abcdef");
    m.run(11 * MIN, STEP);
    expect(m.update.performs == 0, "no install while the module is in setup", std::to_string(m.update.performs));
    m.get("/dl/learn?mode=quick");
    until(m, "\"learn\":{\"state\":\"done\"", 400000);
    m.update.nothing_offered();
    m.get("/wifisave?ssid=Home&psk=12345678");
    m.run(12000);
    expect(m.boots == 2, "set up", std::to_string(m.boots));
    m.run(11 * MIN, STEP);
    expect(m.update.performs == 0, "nothing offered, nothing installed");

    // Offered while the dryer is running: wait for it to finish.
    w.classic.boards[0].erds[0x2000] = {0x02};
    m.run(10000, STEP);
    m.update.offer("0.1.1", "0123456789abcdef0123456789abcdef");
    m.run(16 * MIN, STEP);
    expect(m.update.performs == 0, "not while the dryer is running", std::to_string(m.update.performs));
    w.classic.boards[0].erds[0x2000] = {0x03};
    m.run(6 * MIN, STEP);
    expect(m.update.performs == 0, "not while it is paused");
    // The module looks every five minutes of running time. Go to one minute past such a moment, so the next
    // steps fall where the comments say.
    m.run((6 * MIN - g_ms % (5 * MIN)) % (5 * MIN), STEP);
    w.classic.boards[0].erds[0x2000] = {0x04};  // end of cycle
    m.run(5 * MIN, STEP);
    expect(m.update.performs == 1, "installed once the cycle has ended", std::to_string(m.update.performs));
    expect(tries() == 1, "and the try is on record before it starts", std::to_string(tries()));
    // The install fails without a restart (say the download broke off): the next try is five minutes later.
    m.run(3 * MIN, STEP);
    expect(m.update.performs == 1, "a failed install is not repeated at once", std::to_string(m.update.performs));
    m.run(2 * MIN, STEP);
    expect(m.update.performs == 2 && tries() == 2, "but five minutes later", std::to_string(m.update.performs));

    // The install "succeeds" (restart), but the same firmware is still offered afterwards: it did not take.
    // Tries so far: 2. The third waits 20 minutes from power-up, the fourth 80.
    w.classic.boards[0].erds[0x2000] = {0x00};
    m.boot(w);
    m.run(19 * MIN, STEP);
    expect(m.update.performs == 2, "third try not before 20 minutes", std::to_string(m.update.performs));
    m.run(7 * MIN, STEP);
    expect(m.update.performs == 3, "third try after 20 minutes", std::to_string(m.update.performs));
    m.boot(w);
    m.run(79 * MIN, STEP);
    expect(m.update.performs == 3, "fourth try not before 80 minutes", std::to_string(m.update.performs));
    m.run(7 * MIN, STEP);
    expect(m.update.performs == 4, "fourth try after 80 minutes", std::to_string(m.update.performs));

    // A different firmware on offer starts from the beginning.
    m.boot(w);
    m.update.offer("0.1.2", "ffffffffffffffffffffffffffffffff");
    m.run(6 * MIN, STEP);
    expect(m.update.performs == 5 && tries() == 1, "a new release is tried at once", std::to_string(m.update.performs));
    // It took: after the restart nothing is offered and the count is cleared.
    m.boot(w);
    m.update.nothing_offered();
    m.run(6 * MIN, STEP);
    expect(m.update.performs == 5 && tries() == 0, "a release that took clears the count", std::to_string(tries()));

    // However often a release has failed, it is tried no more than once a day, and never less.
    {
      dryer_link::UpdateTries rec{fnv1_hash("ffffffffffffffffffffffffffffffff"), 200, {0, 0, 0}};
      sim_flash()[TRIES_KEY].assign((const uint8_t *) &rec, (const uint8_t *) &rec + sizeof(rec));
      m.boot(w);
      m.update.offer("0.1.2", "ffffffffffffffffffffffffffffffff");
      m.run(23 * 60 * MIN + 50 * MIN, 50);
      expect(m.update.performs == 5, "a release that keeps failing is not tried within a day", std::to_string(m.update.performs));
      m.run(20 * MIN, 50);
      expect(m.update.performs == 6, "and is tried once a day", std::to_string(m.update.performs));
      expect_has(m.status(), "\"state\":\"found\"");
    }

    // Flash that cannot be written: no count, so no install.
    m.boot(w);
    m.update.offer("0.1.3", "11111111111111111111111111111111");
    sim_flash_fails = true;
    m.run(16 * MIN, STEP);
    expect(m.update.performs == 6, "no install when the try cannot be recorded", std::to_string(m.update.performs));
    sim_flash_fails = false;

    // Built without self-update (the test build), and built without an update entity at all.
    m.auto_update = false;
    m.boot(w);
    m.update.offer("0.1.3", "11111111111111111111111111111111");
    m.run(16 * MIN, STEP);
    expect(m.update.performs == 6, "auto_update: false installs nothing by itself", std::to_string(m.update.performs));
    m.auto_update = true;
    m.with_update = false;
    m.boot(w);
    m.run(16 * MIN, STEP);
    expect(m.update.performs == 6, "no update entity: nothing happens");
    m.with_update = true;

    // While a dryer is being learned (a module moved to another dryer learns on its own), an offer waits.
    w.classic.boards[0].erds[0x0001] = text32("GFD55ESSN0WW");
    m.boot(w);
    m.update.offer("0.1.3", "11111111111111111111111111111111");
    long t = until(m, "\"learn\":{\"state\":\"running\"", 25000);
    expect(t >= 0, "learning the new dryer");
    const int boots = m.boots;
    m.run(100000, STEP);
    expect(m.update.performs == 6, "no install while learning", std::to_string(m.update.performs));
    m.run(5 * MIN, STEP);
    expect(m.update.performs == 7 && m.boots == boots, "installed after learning finished", std::to_string(m.update.performs));
    expect(g_faults.empty(), "no faults");
  }

  // The wire format the pretend dryers use is the one seen on the real GTD65 (captured 2026-11-22).
  {
    std::vector<uint8_t> f = frame(0xBF, 0x2B, {0xF0, 0x01, 0x60, 0x03});
    const uint8_t seen[] = {0xE2, 0xBF, 0x0B, 0x2B, 0xF0, 0x01, 0x60, 0x03, 0x8B, 0x93, 0xE3};
    expect(f.size() == sizeof(seen) && memcmp(f.data(), seen, sizeof(seen)) == 0, "frame encoding matches a frame captured from the real dryer");
  }

  bool unexpected = false;
  for (auto &f : g_faults) {
    unexpected = true;
    fprintf(stderr, "fault: %s\n", f.c_str());
  }
  expect(!unexpected, "no faults anywhere");
  fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}

// =====================================================================================================================
// Serving the page
// =====================================================================================================================

static int serve(int port, double speed, const std::string &dryer, bool test_build, const std::string &label) {
  fresh();
  if (!label.empty())
    set_label(label.c_str());
  World w;
  if (dryer == "gtd65") {
    w.classic.boards.push_back(gtd65());
  } else if (dryer == "new") {
    w.newbus.present = true;
    w.newbus.erds = gtd65().erds;
    w.newbus.erds[0x0001] = text32("PTD60GBSRWS");
    w.newbus.erds.erase(0x201B);
  } else if (dryer == "washer") {
    Board b = gtd65();
    b.erds[0x0008] = {0x02};
    b.erds[0x0001] = text32("GTW685BSL2WS");
    w.classic.boards.push_back(b);
  } else if (dryer == "none") {
    w.classic.boards.push_back(gtd65());
    w.classic.cable_in = false;
  } else {
    fprintf(stderr, "unknown dryer '%s'\n", dryer.c_str());
    return 2;
  }
  Module m;
  m.setup_on_network = test_build;
  m.status_page_login = !test_build;
  m.auto_update = !test_build;
  m.boot(w);

  int srv = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons((uint16_t) port);
  if (bind(srv, (sockaddr *) &a, sizeof(a)) != 0 || listen(srv, 16) != 0) {
    perror("listen");
    return 2;
  }
  fprintf(stderr, "pretend module (%s) on http://127.0.0.1:%d/ at %gx speed\n", dryer.c_str(), port, speed);

  auto wall0 = std::chrono::steady_clock::now();
  double sim_total = 0;  // milliseconds of module time run so far (across restarts)
  for (;;) {
    double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wall0).count();
    while (sim_total < wall * speed) {
      m.tick(1);
      sim_total += 1;
    }
    pollfd pfd{srv, POLLIN, 0};
    if (poll(&pfd, 1, 2) <= 0)
      continue;
    int c = accept(srv, nullptr, nullptr);
    if (c < 0)
      continue;
    std::string in;
    char buf[2048];
    while (in.find("\r\n\r\n") == std::string::npos) {
      ssize_t n = recv(c, buf, sizeof(buf), 0);
      if (n <= 0)
        break;
      in.append(buf, (size_t) n);
    }
    std::string target = "/";
    if (in.compare(0, 4, "GET ") == 0) {
      size_t sp = in.find(' ', 4);
      target = in.substr(4, sp - 4);
    }
    AsyncWebServerRequest req;
    std::string type = "text/plain", body, extra;
    int code = 200;
    const bool hotspot = captive_portal::global_captive_portal->active;
    if (target.compare(0, 5, "/sim/") == 0) {
      AsyncWebServerRequest parsed;
      m.http(target, parsed);  // only to split the query
      if (target.compare(0, 9, "/sim/door") == 0 && !w.classic.boards.empty()) {
        w.classic.boards[0].erds[0x2012] = {(uint8_t)(parsed.arg("open") == "1" ? 0x00 : 0x01)};
        w.newbus.erds[0x2012] = w.classic.boards[0].erds[0x2012];
      } else if (target.compare(0, 9, "/sim/door") == 0) {
        w.newbus.erds[0x2012] = {(uint8_t)(parsed.arg("open") == "1" ? 0x00 : 0x01)};
      } else if (target.compare(0, 10, "/sim/state") == 0) {
        uint8_t v = (uint8_t) atoi(parsed.arg("v").c_str());
        if (!w.classic.boards.empty())
          w.classic.boards[0].erds[0x2000] = {v};
        w.newbus.erds[0x2000] = {v};
      } else if (target.compare(0, 10, "/sim/cable") == 0) {
        w.classic.cable_in = parsed.arg("in") == "1";
      }
      char info[256];
      snprintf(info, sizeof(info), "{\"boots\":%d,\"ms\":%u,\"hotspot\":%s,\"writes\":%d,\"faults\":%u,\"hidden\":[", m.boots, g_ms,
               hotspot ? "true" : "false", w.classic.writes, (unsigned) g_faults.size());
      body = info;
      bool first = true;
      for (size_t i = 0; i < m.entities.size(); i++) {
        if (m.entities[i]->is_internal()) {
          snprintf(info, sizeof(info), "%s\"0x%04X\"", first ? "" : ",", FEATURES[i].erd);
          body += info;
          first = false;
        }
      }
      body += "]}";
      type = "application/json";
    } else if (m.http(target, req)) {
      code = req.response.code;
      type = req.response.content_type;
      body = req.response.body;
      for (auto &h : req.response.headers)
        extra += h.first + ": " + h.second + "\r\n";
    } else if (hotspot && target.compare(0, 12, "/config.json") == 0) {
      // What ESPHome's captive portal answers, including its empty first entry.
      type = "application/json";
      body = R"({"mac":"AA:BB:CC:3F:A2:C4","name":"dryer-link-3fa2c4","aps":[{},{"ssid":"Maple House","rssi":-48,"lock":1},)"
             R"({"ssid":"Maple House Guest","rssi":-52,"lock":0},{"ssid":"NETGEAR-5G-2241","rssi":-67,"lock":1},{"ssid":"Maple House","rssi":-71,"lock":1}]})";
    } else {
      code = 404;
      body = "status page would be here";
    }
    char head[512];
    snprintf(head, sizeof(head), "HTTP/1.1 %d X\r\nContent-Type: %s\r\nContent-Length: %zu\r\n%sConnection: close\r\n\r\n", code, type.c_str(),
             body.size(), extra.c_str());
    std::string out = std::string(head) + body;
    size_t off = 0;
    while (off < out.size()) {
      ssize_t n = send(c, out.data() + off, out.size() - off, MSG_NOSIGNAL);
      if (n <= 0)
        break;
      off += (size_t) n;
    }
    close(c);
  }
}

int main(int argc, char **argv) {
  std::string mode, dryer = "gtd65", label;
  bool test_build = false;
  int port = 8099;
  double speed = 1;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--test") {
      mode = "test";
    } else if (a == "--serve" && i + 1 < argc) {
      mode = "serve";
      port = atoi(argv[++i]);
    } else if (a == "--speed" && i + 1 < argc) {
      speed = atof(argv[++i]);
    } else if (a == "--dryer" && i + 1 < argc) {
      dryer = argv[++i];
    } else if (a == "--log" && i + 1 < argc) {
      sim_log_level = atoi(argv[++i]);
    } else if (a == "--test-build") {
      test_build = true;
    } else if (a == "--label" && i + 1 < argc) {
      label = argv[++i];
    } else if (a == "--parse-label" && i + 1 < argc) {
      // The firmware's own parser on a partition image given as hex. Prints the password, or nothing.
      const std::string hex = argv[++i];
      std::vector<uint8_t> image;
      for (size_t k = 0; k + 1 < hex.size(); k += 2)
        image.push_back((uint8_t) strtol(hex.substr(k, 2).c_str(), nullptr, 16));
      printf("%s\n", dryer_link::parse_label(image.data(), image.size()).c_str());
      return 0;
    }
  }
  if (mode == "test") {
    sim_log_level = std::max(sim_log_level, 0);
    if (getenv("SIM_QUIET") != nullptr)
      sim_log_level = 0;
    return run_tests();
  }
  if (mode == "serve")
    return serve(port, speed, dryer, test_build, label);
  fprintf(stderr, "usage: sim --test | --serve PORT [--speed N] [--dryer gtd65|new|washer|none] [--test-build] [--label PASSWORD] [--log 0-5]\n"
                  "       sim --parse-label HEX\n");
  return 2;
}
