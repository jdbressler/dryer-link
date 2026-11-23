#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/gea/gea.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/update/update_entity.h"
#include "esphome/components/web_server_base/web_server_base.h"

#include <map>
#include <string>
#include <vector>

namespace esphome {
namespace dryer_link {

// The setup password that tools/flash_module.py wrote into this module's "dl_label" flash partition, or "" when
// there is none. It is different on every module, is printed on the module's label, and is in no firmware file.
std::string read_setup_password();
// Checks and decodes the partition's contents. Layout: "DLK1", one length byte, the password, then a CRC-16
// (polynomial 0x1021, start value 0xFFFF, high byte first) of everything before it.
std::string parse_label(const uint8_t *data, size_t len);

// One thing a dryer may or may not have: the data point that carries it, the words the setup page uses for it,
// and the entities that are hidden when this dryer does not answer for it.
struct Feature {
  uint16_t erd;
  const char *label;
  bool control;        // listed under "what you'll be able to do" instead of "what your dryer can share"
  uint16_t write_erd;  // 0, or: the dryer must also acknowledge a write here (its current value, unchanged)
  std::vector<EntityBase *> entities;
  bool supported{true};
};

// What survives a restart. Kept small: the scan's raw values stay in RAM only.
struct Saved {
  uint32_t magic;
  uint32_t features_hash;  // the firmware's feature list this was learned against
  uint32_t model_hash;     // the dryer it was learned on
  uint32_t supported;      // bit n = feature n answered
  uint16_t unknown;        // answered codes no feature uses
  uint8_t learned;
  uint8_t full;        // 1 = the last learn was a full scan
  uint8_t new_lang;    // 1 = GEA3
  uint8_t door_quiet;  // 1 = the door check timed out and the owner carried on without it
  uint8_t reserved[2];
} __attribute__((packed));

// How often the firmware offered at the update address has been tried. Stops a release that never "takes" from
// being installed over and over.
struct UpdateTries {
  uint32_t md5_hash;
  uint8_t tries;
  uint8_t reserved[3];
} __attribute__((packed));

class DryerLink : public Component, public AsyncWebHandler {
 public:
  // ---- configuration (from YAML) ----
  void set_hub(gea::GEAComponent *hub) { hub_ = hub; }
  void set_web_server_base(web_server_base::WebServerBase *base) { base_ = base; }
  void set_classic_uart(uart::UARTComponent *u) { uart_classic_ = u; }
  void set_new_uart(uart::UARTComponent *u) { uart_new_ = u; }
  void set_version(const char *v) { version_ = v; }
  void set_setup_on_network(bool v) { setup_on_network_ = v; }
  void set_status_page_login(bool v) { status_page_login_ = v; }
  void set_update(update::UpdateEntity *u) { update_ = u; }
  void set_auto_update(bool v) { auto_update_ = v; }
  void add_feature(uint16_t erd, const char *label, bool control, uint16_t write_erd) {
    features_.push_back(Feature{erd, label, control, write_erd, {}});
  }
  void add_feature_entity(EntityBase *e) { features_.back().entities.push_back(e); }

  // ---- component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  // Before Wi-Fi: the hotspot password must be in place before the hotspot starts, and this handler must be in
  // front of the captive portal's.
  float get_setup_priority() const override { return setup_priority::WIFI + 2.0f; }

  // ---- web handler (runs on the web server's own task) ----
  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

  // ---- for YAML lambdas ----
  bool is_dryer_found() const { return bus_ == Bus::FOUND; }
  // The hotspot (and the status page) are protected by this module's own password.
  bool is_secured() const { return !setup_password_.empty(); }

 protected:
  enum class Bus : uint8_t { SEARCHING, FOUND, NONE, UNSUPPORTED };
  // Where the search is: the classic bus by appliance type, by model number, the new bus, the classic bus address
  // by address; then reading the identity; then settled.
  enum class Step : uint8_t { CLASSIC_TYPE, CLASSIC_MODEL, NEW, CLASSIC_SWEEP, IDENTITY, SETTLED };
  enum class Learn : uint8_t { IDLE, RUNNING, DONE };
  enum class Check : uint8_t { NONE, DOOR, POWER };
  enum class Phase : uint8_t { IDLE, WAIT1, WAIT2, OK, TIMEOUT };
  enum class Wifi : uint8_t { IDLE, CONNECTING, OK, FAIL };
  // What the single outstanding probe is for.
  enum class Asked : uint8_t { NOTHING, IDENTITY, LEARN, LIVENESS, CHECK, KEEPALIVE, SWEEP, WRITE_READ, WRITE_CHECK };

  // ---- main-loop steps (all called with lock_ held) ----
  void search_begin_(Step step);
  void search_step_(uint32_t now);
  void identity_begin_(bool is_new, uint8_t address);
  void identity_done_();
  void learn_begin_(bool full);
  void learn_step_(uint32_t now);
  void learn_finish_();
  void learn_abort_(const char *why);
  void check_begin_(Check what);
  void check_step_(uint32_t now);
  void keepalive_step_(uint32_t now);
  void wifi_step_(uint32_t now);
  void auto_update_step_(uint32_t now);
  bool quiet_() const;
  void apply_polling_();
  void save_();
  void take_identity_(uint16_t erd, const std::vector<uint8_t> &data);
  uint32_t hidden_mask_() const;
  void on_probe_(uint16_t erd, gea::ProbeResult result, const std::vector<uint8_t> &data);
  void ask_(uint16_t erd, uint8_t retries, Asked why);
  bool portal_active_() const;
  uint32_t features_hash_() const;
  size_t feature_index_(uint16_t erd) const;

  // ---- page and API (web task; take lock_ themselves) ----
  std::string status_json_();
  std::string report_();
  void send_page_(AsyncWebServerRequest *request);

  gea::GEAComponent *hub_{nullptr};
  web_server_base::WebServerBase *base_{nullptr};
  uart::UARTComponent *uart_classic_{nullptr};
  uart::UARTComponent *uart_new_{nullptr};
  update::UpdateEntity *update_{nullptr};
  const char *version_{""};
  bool setup_on_network_{false};   // serve the walkthrough and its API on the home network too (test builds)
  bool status_page_login_{true};   // ask for the label password on the status page
  bool auto_update_{true};
  std::string setup_password_;
  std::vector<Feature> features_;
  ESPPreferenceObject pref_;
  Saved saved_{};
  bool saved_ok_{true};
  uint32_t boot_hidden_{0};  // bit n = feature n's entities were hidden at start-up
  bool active_new_{false};   // which UART the hub is on (main loop only)

  ESPPreferenceObject tries_pref_;
  uint32_t update_tick_{0};
  uint32_t update_last_try_{0};
  bool update_tried_this_boot_{false};

  // Everything below is shared between the main loop and the web task and is guarded by lock_.
  Mutex lock_;

  Bus bus_{Bus::SEARCHING};
  Step step_{Step::CLASSIC_TYPE};
  uint32_t step_started_{0};
  uint8_t search_rounds_{0};
  uint8_t sweep_index_{0};
  bool new_lang_{false};
  uint8_t address_{0xFF};
  uint8_t identity_index_{0};
  bool type_known_{false};
  uint8_t type_{0};
  bool state_answers_{false};
  std::string model_;
  std::string serial_;
  std::string type_name_;
  std::vector<uint8_t> candidates_;  // boards that answered, when more than one did

  Asked asked_{Asked::NOTHING};
  uint16_t asked_erd_{0};

  Learn learn_{Learn::IDLE};
  bool learn_full_{false};
  bool learned_{false};
  bool auto_learn_tried_{false};
  std::vector<uint16_t> learn_list_;
  size_t learn_careful_{0};  // the first this-many entries are asked with repeats
  size_t learn_index_{0};
  size_t learn_since_liveness_{0};
  uint8_t learn_busy_tries_{0};
  size_t learn_write_index_{0};       // next feature whose write is to be checked
  bool learn_write_fresh_{false};     // its value has just been read again
  uint32_t learn_write_refused_{0};   // bit n = feature n answers reads but did not acknowledge a write
  uint32_t learn_settle_until_{0};
  uint16_t unknown_{0};
  std::map<uint16_t, std::vector<uint8_t>> found_;  // every code that answered, with its last value
  bool save_wanted_{false};

  Check check_{Check::NONE};
  Phase phase_{Phase::IDLE};
  uint32_t check_started_{0};
  uint32_t check_asked_{0};
  bool check_has_baseline_{false};
  uint8_t check_baseline_{0};
  bool door_quiet_{false};

  bool live_state_known_{false};
  uint8_t live_state_{0};
  bool live_door_known_{false};
  bool live_door_open_{false};
  uint32_t keepalive_at_{0};

  Wifi wifi_{Wifi::IDLE};
  std::string wifi_ssid_;
  uint32_t wifi_started_{0};
  uint32_t online_since_{0};

  // Requests from the page, picked up by the main loop.
  bool want_find_{false};
  uint8_t want_learn_{0};  // 0 none, 1 quick, 2 full
};

}  // namespace dryer_link
}  // namespace esphome
