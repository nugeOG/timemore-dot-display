#pragma once

// ============================================================================
// Custom ESPHome external component for the TIMEMORE Black Mirror Dot BLE
// scale. Owns the NimBLE stack exclusively (no esp32_ble_tracker/ble_client
// in YAML) so it has full control over forcing a secure/bonded connection
// with retry logic -- see HANDOFF.md, "Architecture decision" section, for
// why this couldn't be done with ESPHome's declarative BLE components.
//
// Ported from gaggimate/esp-arduino-ble-scales, src/scales/dot.h / dot.cpp
// (MIT-licensed). That repo is the authoritative source for the protocol --
// re-check it if anything here seems to not match your actual scale.
//
// Built against h2zero/NimBLE-Arduino (see __init__.py's pinned version and
// its comment for the full story of why -- esp-nimble-cpp was tried first
// and is architecturally wrong for framework: type: arduino, since it's a
// headers-only wrapper around a NimBLE host that the Arduino framework's
// prebuilt libs don't actually contain). Class/method names are API-
// compatible with esp-nimble-cpp for everything used here (NimBLEDevice,
// NimBLEClient, NimBLEScan, subscribe(), secureConnection(),
// NimBLEScanCallbacks::onResult taking a `const` pointer in 2.x) -- if a
// future library bump breaks the build, check NimBLE-Arduino's own
// migration guide.
// ============================================================================

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/button/button.h"
#include "esphome/components/wifi/wifi_component.h"

#include <NimBLEDevice.h>

#include <atomic>
#include <vector>
#include <string>

namespace esphome {
namespace timemore_dot {

static const char *const SERVICE_UUID = "FFF0";
static const char *const NOTIFY_CHAR_UUID = "FFF1";
static const char *const WRITE_CHAR_UUID = "FFF2";
// Device advertises with a name starting with this prefix (confirmed from
// the reference driver) -- we connect to the first matching advertisement
// seen, there's no support here for picking a specific scale by MAC if you
// have more than one Dot nearby.
static const char *const DEVICE_NAME_PREFIX = "TIMEMORE_Dot";

// Exposed via a text_sensor (see text_sensor.py / set_status_sensor()) so
// the display's "disconnected" screen can show what's actually happening
// (scanning vs. connecting vs. waiting to retry) rather than a single
// generic "not connected" state.
enum class BleStatus {
  WAITING_FOR_WIFI,
  SCANNING,
  CONNECTING,
  CONNECTED,
  RECONNECTING,
};

class TimemoreDot : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After wifi so an early BLE scan doesn't compete with wifi's own radio
  // setup; NimBLE and wifi share the ESP32's single 2.4GHz radio via
  // time-slicing regardless, this just orders init.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_weight_sensor(sensor::Sensor *s) { weight_sensor_ = s; }
  void set_battery_sensor(sensor::Sensor *s) { battery_sensor_ = s; }
  void set_connected_sensor(binary_sensor::BinarySensor *s) { connected_sensor_ = s; }
  void set_status_sensor(text_sensor::TextSensor *s) { status_sensor_ = s; }

  // Writes the tare command frame, then the handshake/poll follow-up frame
  // -- the scale doesn't actually zero until that second write lands (see
  // HANDOFF.md, "BLE protocol" section).
  void tare();

  // Called from the NimBLE scan/client callbacks defined in the .cpp --
  // public because those callback classes aren't members of TimemoreDot.
  //
  // Deliberately does NOT connect (or even stop the scan) itself, even
  // though it used to. Real hardware testing found every single connect
  // attempt timing out at NimBLE's ~30s watchdog (status=13/
  // BLE_HS_ETIMEOUT), which turned out to be self-inflicted: this callback
  // runs ON NimBLE's own host task, and NimBLEClient::connect() blocks
  // waiting for a GAP event that the host task itself has to process --
  // calling it from here blocks the host task waiting on itself, so the
  // connect-complete event NimBLE queues internally can never be handled
  // until this callback returns, which only happens once the connect call
  // gives up. (Matches h2zero/NimBLE-Arduino#1136, not fixed as of 2.3.6.)
  // This just records the address and a flag; loop() (ESPHome's own task,
  // not NimBLE's) does the actual stop-scan-and-connect.
  void on_scan_result(const NimBLEAdvertisedDevice *device);
  void on_connect();
  void on_disconnect();
  void on_notify(const uint8_t *data, size_t length);

 protected:
  // Everything that used to run directly in setup() -- NimBLEDevice::init()
  // and onward. A real boot crashed 100% reproducibly inside NimBLE's own
  // init sequence (nimble_port_run/host_task) when this ran at setup()
  // time, i.e. right as wifi's own setup() had *just* finished but before
  // it had actually associated -- a wifi/BT coexistence sdkconfig fix
  // (still applied, see __init__.py) did not resolve it. Deferred to the
  // first loop() iteration after wifi actually reports connected, not
  // just past its own setup(), as the next thing to try -- see HANDOFF.md.
  void start_ble_stack_();
  void start_scan_();
  // Takes an address, not the NimBLEAdvertisedDevice* the scan callback
  // hands us -- that pointer is only valid for the duration of the
  // callback, and (see on_scan_result()'s comment) the actual connect
  // attempt now happens later, from loop(), by which point it may be
  // dangling. Retries a few times with a fresh client each time, matching
  // the reference driver -- a single attempt right after scanning stops
  // was observed on real hardware to sometimes need a retry or two before
  // the scale accepts the connection.
  void connect_(const NimBLEAddress &address);
  // Sets marked_for_reconnect_ and stamps last_reconnect_attempt_ to now --
  // every failure path needs both, not just the flag, so loop()'s backoff
  // check actually waits RECONNECT_INTERVAL_MS before retrying instead of
  // firing on the very next loop() tick (member defaults to 0, so an
  // unstamped first failure looks like it happened at boot and passes the
  // backoff check immediately). A real boot hit this: an instant retry
  // raced NimBLE's own async teardown of the just-timed-out connection
  // attempt and left the component stuck (see start_scan_()'s comment).
  void mark_for_reconnect_();
  // Buffers a status_sensor_ update the same way weight/battery/connected
  // are buffered -- see those members' comment for why this can't publish
  // directly, since this is called from both the host task (scan/connect
  // callbacks) and loop() itself.
  void set_status_(BleStatus status);
  bool write_frame_(const uint8_t *data, size_t length);
  void handle_frame_(const uint8_t *payload, size_t payload_len, uint8_t frame_class, uint8_t frame_type);

  // Guards start_ble_stack_() so it runs exactly once, from loop(), once
  // wifi is confirmed connected.
  bool ble_started_{false};

  NimBLEClient *client_{nullptr};
  NimBLERemoteCharacteristic *write_char_{nullptr};
  // Owned for the process lifetime; declared as the base callback types so
  // this header doesn't need to know about the concrete callback classes
  // defined in timemore_dot.cpp.
  NimBLEClientCallbacks *client_callbacks_{nullptr};
  NimBLEScanCallbacks *scan_callbacks_{nullptr};
  bool connected_{false};
  bool scanning_{false};
  // Mirrors the reference driver's "markedForReconnection" retry pattern --
  // set on any connect failure or disconnect, cleared once a new scan
  // actually starts. loop() polls this instead of reconnecting inline from
  // a BLE callback, since NimBLE callbacks run on their own task/stack.
  bool marked_for_reconnect_{false};
  uint32_t last_reconnect_attempt_{0};
  std::string target_address_;

  // Set by on_scan_result() (NimBLE host task), consumed by loop()
  // (ESPHome's own task) -- see on_scan_result()'s comment for why the
  // connect attempt itself can't happen on the host task.
  bool have_pending_connect_{false};
  NimBLEAddress pending_address_;

  sensor::Sensor *weight_sensor_{nullptr};
  sensor::Sensor *battery_sensor_{nullptr};
  binary_sensor::BinarySensor *connected_sensor_{nullptr};
  text_sensor::TextSensor *status_sensor_{nullptr};

  // publish_state() must only ever be called from loop() (ESPHome's main
  // task), never directly from on_notify()/on_connect()/on_disconnect(),
  // which all run on NimBLE's own host task. publish_state() synchronously
  // runs that sensor's automations, and scale-display-lvgl.yaml updates
  // LVGL widgets from them -- LVGL is not thread-safe and must only be
  // touched from the main loop task. Calling it from the host task
  // silently corrupted something and hung the whole device with no
  // crash/reboot at all: a real boot's log went completely dead right
  // after the first burst of notifications, with a single garbled
  // "[E][lvgl:000]" line (itself tagged as running on the nimble_host
  // task) as the only symptom, and never logged anything again.
  // std::atomic since the host task and main loop task can run truly
  // concurrently on the ESP32's two cores, not just interleaved.
  std::atomic<bool> weight_dirty_{false};
  std::atomic<float> pending_weight_{0.0f};
  std::atomic<bool> battery_dirty_{false};
  std::atomic<uint8_t> pending_battery_{0};
  std::atomic<bool> connected_dirty_{false};
  std::atomic<bool> pending_connected_{false};
  std::atomic<bool> status_dirty_{false};
  std::atomic<BleStatus> pending_status_{BleStatus::WAITING_FOR_WIFI};

  // Reassembly buffer -- a single NimBLE notification isn't guaranteed to
  // land as exactly one protocol frame, so we buffer and slice frames out
  // by their length field rather than assuming a 1:1 mapping.
  std::vector<uint8_t> rx_buffer_;
};

// Exposes tare() as a regular ESPHome button entity (platform: timemore_dot
// under button:) so it's callable from Home Assistant / the API, not just
// the on-device touchscreen -- part of the "fan data/control out over the
// network" decision in HANDOFF.md.
class TareButton : public button::Button, public Component {
 public:
  void set_parent(TimemoreDot *parent) { parent_ = parent; }

 protected:
  void press_action() override { parent_->tare(); }
  TimemoreDot *parent_{nullptr};
};

}  // namespace timemore_dot
}  // namespace esphome
